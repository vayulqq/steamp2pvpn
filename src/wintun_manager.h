#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

typedef void* WINTUN_ADAPTER_HANDLE;
typedef void* WINTUN_SESSION_HANDLE;

typedef WINTUN_ADAPTER_HANDLE(WINAPI* WINTUN_CREATE_ADAPTER_FUNC)(PCWSTR Name, PCWSTR TunnelType, const GUID* RequestedGUID);
typedef WINTUN_ADAPTER_HANDLE(WINAPI* WINTUN_OPEN_ADAPTER_FUNC)(PCWSTR Name);
typedef VOID(WINAPI* WINTUN_CLOSE_ADAPTER_FUNC)(WINTUN_ADAPTER_HANDLE Adapter);
typedef WINTUN_SESSION_HANDLE(WINAPI* WINTUN_START_SESSION_FUNC)(WINTUN_ADAPTER_HANDLE Adapter, DWORD Capacity);
typedef VOID(WINAPI* WINTUN_END_SESSION_FUNC)(WINTUN_SESSION_HANDLE Session);
typedef HANDLE(WINAPI* WINTUN_GET_READ_WAIT_EVENT_FUNC)(WINTUN_SESSION_HANDLE Session);
typedef BYTE*(WINAPI* WINTUN_RECEIVE_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, DWORD* PacketSize);
typedef VOID(WINAPI* WINTUN_RELEASE_RECEIVE_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, const BYTE* Packet);
typedef BYTE*(WINAPI* WINTUN_ALLOCATE_SEND_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, DWORD PacketSize);
typedef VOID(WINAPI* WINTUN_SEND_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, const BYTE* Packet);

class WintunManager {
public:
    WintunManager() = default;
    ~WintunManager();

    bool Initialize(const std::wstring& adapterName, const std::string& ipAddress, const std::string& netmask = "255.255.255.0", uint32_t mtu = 1200);
    void Shutdown();

    bool ReceivePacket(std::vector<uint8_t>& outBuffer);
    bool SendPacket(const void* data, size_t size);
    HANDLE GetReadWaitEvent() const;

    bool IsInitialized() const { return m_Session != nullptr; }

private:
    void ExecuteSilentCmd(const std::wstring& cmd);

    HMODULE m_hWintunDll = nullptr;
    WINTUN_ADAPTER_HANDLE m_Adapter = nullptr;
    WINTUN_SESSION_HANDLE m_Session = nullptr;

    WINTUN_CREATE_ADAPTER_FUNC pfnWintunCreateAdapter = nullptr;
    WINTUN_OPEN_ADAPTER_FUNC pfnWintunOpenAdapter = nullptr;
    WINTUN_CLOSE_ADAPTER_FUNC pfnWintunCloseAdapter = nullptr;
    WINTUN_START_SESSION_FUNC pfnWintunStartSession = nullptr;
    WINTUN_END_SESSION_FUNC pfnWintunEndSession = nullptr;
    WINTUN_GET_READ_WAIT_EVENT_FUNC pfnWintunGetReadWaitEvent = nullptr;
    WINTUN_RECEIVE_PACKET_FUNC pfnWintunReceivePacket = nullptr;
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC pfnWintunReleaseReceivePacket = nullptr;
    WINTUN_ALLOCATE_SEND_PACKET_FUNC pfnWintunAllocateSendPacket = nullptr;
    WINTUN_SEND_PACKET_FUNC pfnWintunSendPacket = nullptr;
};
