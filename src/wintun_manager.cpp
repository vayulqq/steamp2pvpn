#define LOG_TAG "Wintun"
#include "wintun_manager.h"
#include "logger.h"

WintunManager::~WintunManager() {
    Shutdown();
}

std::string WintunManager::WstringToString(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

bool WintunManager::Initialize(const std::wstring& adapterName, const std::string& ipAddress, const std::string& netmask, uint32_t mtu) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_hWintunDll || m_Adapter || m_Session) {
        return false;
    }

    LOG_INFO("Initialize: adapter='" + WstringToString(adapterName) +
             "', ip=" + ipAddress + ", netmask=" + netmask + ", mtu=" + std::to_string(mtu));

    m_hWintunDll = LoadLibraryW(L"wintun.dll");
    if (!m_hWintunDll) {
        LOG_ERROR("Не удалось загрузить wintun.dll (код ошибки: " + std::to_string(GetLastError()) + ")");
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
        LOG_ERROR("Не удалось получить указатели на функции Wintun API");
        Shutdown();
        return false;
    }

    m_Adapter = pfnWintunCreateAdapter(adapterName.c_str(), L"SteamVPN", nullptr);
    if (!m_Adapter && pfnWintunOpenAdapter) {
        m_Adapter = pfnWintunOpenAdapter(adapterName.c_str());
    }

    if (!m_Adapter) {
        LOG_ERROR("Ошибка создания виртуального адаптера");
        Shutdown();
        return false;
    }
    LOG_INFO("Виртуальный адаптер создан успешно");

    NET_LUID luid;
    pfnWintunGetAdapterLUID(m_Adapter, &luid);
    if (!SetAdapterIPAndMTU(luid, ipAddress, netmask, mtu)) {
        LOG_ERROR("Ошибка установки IP-адреса через IPHlpApi");
        Shutdown();
        return false;
    }

    m_Session = pfnWintunStartSession(m_Adapter, 0x400000);
    if (!m_Session) {
        LOG_ERROR("Ошибка открытия сессии Wintun");
        Shutdown();
        return false;
    }

    LOG_INFO("Сессия Wintun открыта, IP=" + ipAddress + " успешно назначен");
    return true;
}

bool WintunManager::UpdateIP(const std::string& ipAddress, const std::string& netmask, uint32_t mtu) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_Adapter || !pfnWintunGetAdapterLUID) {
        LOG_WARN("UpdateIP вызван до инициализации адаптера, ip=" + ipAddress);
        return false;
    }
    LOG_INFO("UpdateIP: новый адрес " + ipAddress);
    NET_LUID luid;
    pfnWintunGetAdapterLUID(m_Adapter, &luid);
    bool ok = SetAdapterIPAndMTU(luid, ipAddress, netmask, mtu);
    if (!ok) {
        LOG_ERROR("UpdateIP: не удалось применить адрес " + ipAddress);
    }
    return ok;
}

bool WintunManager::SetAdapterIPAndMTU(NET_LUID luid, const std::string& ipAddress, const std::string& netmask, uint32_t mtu) {
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &table) == NO_ERROR) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            if (table->Table[i].InterfaceLuid.Value == luid.Value) {
                DeleteUnicastIpAddressEntry(&table->Table[i]);
            }
        }
        FreeMibTable(table);
    }

    MIB_UNICASTIPADDRESS_ROW ipRow;
    InitializeUnicastIpAddressEntry(&ipRow);
    ipRow.InterfaceLuid = luid;
    ipRow.Address.Ipv4.sin_family = AF_INET;

    if (inet_pton(AF_INET, ipAddress.c_str(), &ipRow.Address.Ipv4.sin_addr) != 1) {
        return false;
    }

    ULONG mask = 0;
    UINT8 prefixLength = 24;
    if (inet_pton(AF_INET, netmask.c_str(), &mask) == 1) {
        if (ConvertIpv4MaskToLength(ntohl(mask), &prefixLength) != NO_ERROR) {
            return false;
        }
    }
    ipRow.OnLinkPrefixLength = prefixLength;
    ipRow.DadState = IpDadStatePreferred;

    DWORD status = CreateUnicastIpAddressEntry(&ipRow);
    if (status == ERROR_OBJECT_ALREADY_EXISTS) {
        if (SetUnicastIpAddressEntry(&ipRow) != ERROR_SUCCESS) {
            return false;
        }
    } else if (status != ERROR_SUCCESS) {
        return false;
    }

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
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_Session || m_Adapter) {
        LOG_INFO("Shutdown: освобождение ресурсов Wintun");
    }
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

    pfnWintunCreateAdapter = nullptr;
    pfnWintunOpenAdapter = nullptr;
    pfnWintunCloseAdapter = nullptr;
    pfnWintunGetAdapterLUID = nullptr;
    pfnWintunStartSession = nullptr;
    pfnWintunEndSession = nullptr;
    pfnWintunGetReadWaitEvent = nullptr;
    pfnWintunReceivePacket = nullptr;
    pfnWintunReleaseReceivePacket = nullptr;
    pfnWintunAllocateSendPacket = nullptr;
    pfnWintunSendPacket = nullptr;
}

bool WintunManager::ReceivePacket(std::vector<uint8_t>& outBuffer) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_Session || !pfnWintunReceivePacket || !pfnWintunReleaseReceivePacket) return false;

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
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_Session || size == 0 || !pfnWintunAllocateSendPacket || !pfnWintunSendPacket) return false;

    BYTE* packet = pfnWintunAllocateSendPacket(m_Session, static_cast<DWORD>(size));
    if (!packet) {
        LOG_WARN("SendPacket: не удалось выделить буфер размером " + std::to_string(size) + " байт (кольцевой буфер переполнен?)");
        return false;
    }

    memcpy(packet, data, size);
    pfnWintunSendPacket(m_Session, packet);
    return true;
}

HANDLE WintunManager::GetReadWaitEvent() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_Session || !pfnWintunGetReadWaitEvent) return nullptr;
    return pfnWintunGetReadWaitEvent(m_Session);
}

bool WintunManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_Session != nullptr;
}
