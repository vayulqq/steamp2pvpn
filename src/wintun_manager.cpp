#include "wintun_manager.h"
#include <iostream>

WintunManager::~WintunManager() {
    Shutdown();
}

bool WintunManager::Initialize(const std::wstring& adapterName, const std::string& ipAddress, const std::string& netmask, uint32_t mtu) {
    m_hWintunDll = LoadLibraryW(L"wintun.dll");
    if (!m_hWintunDll) {
        std::cerr << "[Wintun] Не удалось загрузить wintun.dll\n";
        return false;
    }

    pfnWintunCreateAdapter = (WINTUN_CREATE_ADAPTER_FUNC)GetProcAddress(m_hWintunDll, "WintunCreateAdapter");
    pfnWintunOpenAdapter = (WINTUN_OPEN_ADAPTER_FUNC)GetProcAddress(m_hWintunDll, "WintunOpenAdapter");
    pfnWintunCloseAdapter = (WINTUN_CLOSE_ADAPTER_FUNC)GetProcAddress(m_hWintunDll, "WintunCloseAdapter");
    pfnWintunGetAdapterLUID = (WINTUN_GET_ADAPTER_LUID_FUNC)GetProcAddress(m_hWintunDll, "WintunGetAdapterLUID");
    pfnWintunStartSession = (WINTUN_START_SESSION_FUNC)GetProcAddress(m_hWintunDll, "WintunStartSession");
    pfnWintunEndSession = (WINTUN_END_SESSION_FUNC)GetProcAddress(m_hWintunDll, "WintunEndSession");
    pfnWintunGetReadWaitEvent = (WINTUN_GET_READ_WAIT_EVENT_FUNC)GetProcAddress(m_hWintunDll, "WintunGetReadWaitEvent");
    pfnWintunReceivePacket = (WINTUN_RECEIVE_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunReceivePacket");
    pfnWintunReleaseReceivePacket = (WINTUN_RELEASE_RECEIVE_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunReleaseReceivePacket");
    pfnWintunAllocateSendPacket = (WINTUN_ALLOCATE_SEND_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunAllocateSendPacket");
    pfnWintunSendPacket = (WINTUN_SEND_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunSendPacket");

    if (!pfnWintunCreateAdapter || !pfnWintunStartSession || !pfnWintunReceivePacket || !pfnWintunSendPacket || !pfnWintunGetAdapterLUID) {
        std::cerr << "[Wintun] Не удалось получить указатели на функции Wintun API\n";
        Shutdown();
        return false;
    }

    m_Adapter = pfnWintunCreateAdapter(adapterName.c_str(), L"SteamVPN", nullptr);
    if (!m_Adapter) {
        m_Adapter = pfnWintunOpenAdapter(adapterName.c_str());
    }

    if (!m_Adapter) {
        std::cerr << "[Wintun] Ошибка создания виртуального адаптера\n";
        Shutdown();
        return false;
    }

    // Настройка IP адреса и MTU через чистый Windows IP Helper API (без netsh процесса)
    NET_LUID luid;
    pfnWintunGetAdapterLUID(m_Adapter, &luid);
    if (!SetAdapterIPAndMTU(luid, ipAddress, mtu)) {
        std::cerr << "[Wintun] Ошибка установки IP-адреса через IPHlpApi\n";
    }

    m_Session = pfnWintunStartSession(m_Adapter, 0x400000); // Буфер 4MB
    if (!m_Session) {
        std::cerr << "[Wintun] Ошибка открытия сессии Wintun\n";
        Shutdown();
        return false;
    }

    return true;
}

bool WintunManager::SetAdapterIPAndMTU(NET_LUID luid, const std::string& ipAddress, uint32_t mtu) {
    // 1. Установка IP-адреса
    MIB_UNICASTIPADDRESS_ROW ipRow;
    InitializeUnicastIpAddressEntry(&ipRow);
    ipRow.InterfaceLuid = luid;
    ipRow.Address.Ipv4.sin_family = AF_INET;
    
    if (inet_pton(AF_INET, ipAddress.c_str(), &ipRow.Address.Ipv4.sin_addr) != 1) {
        return false;
    }

    ipRow.OnLinkPrefixLength = 24; // Подсеть 255.255.255.0 (/24)
    ipRow.DadState = IpDadStatePreferred;

    // Создаем запись адреса или обновляем существующую
    DWORD status = CreateUnicastIpAddressEntry(&ipRow);
    if (status != ERROR_SUCCESS && status != ERROR_OBJECT_ALREADY_EXISTS) {
        // Если уже был установлен другой IP, меняем его
        SetUnicastIpAddressEntry(&ipRow);
    }

    // 2. Установка MTU на интерфейсе
    MIB_IPINTERFACE_ROW ifRow;
    InitializeIpInterfaceEntry(&ifRow);
    ifRow.InterfaceLuid = luid;
    ifRow.Family = AF_INET;

    if (GetIpInterfaceEntry(&ifRow) == ERROR_SUCCESS) {
        ifRow.NlMtu = mtu;
        SetIpInterfaceEntry(&ifRow);
    }

    return true;
}

void WintunManager::Shutdown() {
    if (m_Session && pfnWintunEndSession) {
        pfnWintunEndSession(m_Session);
        m_Session = nullptr;
    }
    if (m_Adapter && pfnWintunCloseAdapter) {
        pfnWintunCloseAdapter(m_Adapter);
        m_Adapter = nullptr;
    }
    if (m_hWintunDll) {
        FreeLibrary(m_hWintunDll);
        m_hWintunDll = nullptr;
    }
}

bool WintunManager::ReceivePacket(std::vector<uint8_t>& outBuffer) {
    if (!m_Session) return false;

    DWORD packetSize = 0;
    BYTE* packet = pfnWintunReceivePacket(m_Session, &packetSize);
    if (!packet) {
        return false;
    }

    outBuffer.assign(packet, packet + packetSize);
    pfnWintunReleaseReceivePacket(m_Session, packet);
    return true;
}

bool WintunManager::SendPacket(const void* data, size_t size) {
    if (!m_Session || size == 0) return false;

    BYTE* packet = pfnWintunAllocateSendPacket(m_Session, static_cast<DWORD>(size));
    if (!packet) {
        return false;
    }

    memcpy(packet, data, size);
    pfnWintunSendPacket(m_Session, packet);
    return true;
}

HANDLE WintunManager::GetReadWaitEvent() const {
    if (!m_Session || !pfnWintunGetReadWaitEvent) return nullptr;
    return pfnWintunGetReadWaitEvent(m_Session);
}
