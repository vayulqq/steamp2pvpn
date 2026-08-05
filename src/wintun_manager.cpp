#include "wintun_manager.h"
#include <iostream>
#include <sstream>

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
    pfnWintunStartSession = (WINTUN_START_SESSION_FUNC)GetProcAddress(m_hWintunDll, "WintunStartSession");
    pfnWintunEndSession = (WINTUN_END_SESSION_FUNC)GetProcAddress(m_hWintunDll, "WintunEndSession");
    pfnWintunGetReadWaitEvent = (WINTUN_GET_READ_WAIT_EVENT_FUNC)GetProcAddress(m_hWintunDll, "WintunGetReadWaitEvent");
    pfnWintunReceivePacket = (WINTUN_RECEIVE_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunReceivePacket");
    pfnWintunReleaseReceivePacket = (WINTUN_RELEASE_RECEIVE_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunReleaseReceivePacket");
    pfnWintunAllocateSendPacket = (WINTUN_ALLOCATE_SEND_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunAllocateSendPacket");
    pfnWintunSendPacket = (WINTUN_SEND_PACKET_FUNC)GetProcAddress(m_hWintunDll, "WintunSendPacket");

    if (!pfnWintunCreateAdapter || !pfnWintunStartSession || !pfnWintunReceivePacket || !pfnWintunSendPacket) {
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

    m_Session = pfnWintunStartSession(m_Adapter, 0x400000); // Буфер 4MB
    if (!m_Session) {
        std::cerr << "[Wintun] Ошибка открытия сессии Wintun\n";
        Shutdown();
        return false;
    }

    std::wstringstream ipCmd;
    ipCmd << L"netsh interface ipv4 set address name=\"" << adapterName
          << L"\" static " << std::wstring(ipAddress.begin(), ipAddress.end())
          << L" " << std::wstring(netmask.begin(), netmask.end());
    ExecuteSilentCmd(ipCmd.str());

    std::wstringstream mtuCmd;
    mtuCmd << L"netsh interface ipv4 set subinterface \"" << adapterName
           << L"\" mtu=" << mtu << L" store=persistent";
    ExecuteSilentCmd(mtuCmd.str());

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

void WintunManager::ExecuteSilentCmd(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(L'\0');

    if (CreateProcessW(NULL, cmdBuffer.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}
