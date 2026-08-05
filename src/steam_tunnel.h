#pragma once

#include "wintun_manager.h"
#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <string>
#include <vector>
#include <cstdint>

enum class TunnelState {
    Disconnected,
    Connecting,
    Connected,
    Failed
};

struct ConnectedPeer {
    uint64_t steamID = 0;
    std::string virtualIP;
    int pingMs = 0;
    HSteamNetConnection hConn = k_HSteamNetConnection_Invalid;
};

class SteamVpnTunnel {
public:
    SteamVpnTunnel();
    ~SteamVpnTunnel();

    bool InitHost(const std::string& virtualIP = "192.168.137.1");
    bool InitClient(uint64_t targetSteamID, const std::string& virtualIP = "192.168.137.2");
    void Tick();
    void Shutdown();

    bool IsHost() const { return m_isHost; }
    bool IsClient() const { return m_isClient; }
    bool IsActive() const { return m_state == TunnelState::Connected || m_state == TunnelState::Connecting || m_isHost; }

    TunnelState GetState() const { return m_state; }
    const std::string& GetLastError() const { return m_lastError; }
    uint64_t GetTargetSteamID() const { return m_targetSteamID; }

    const std::vector<ConnectedPeer>& GetPeers() const { return m_peers; }
    static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

private:
    void HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

    WintunManager m_wintun;
    HSteamListenSocket m_hListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_hPollGroup = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection m_hostConn = k_HSteamNetConnection_Invalid;

    bool m_isHost = false;
    bool m_isClient = false;
    TunnelState m_state = TunnelState::Disconnected;
    std::string m_lastError;
    uint64_t m_targetSteamID = 0;
    double m_connectStartTime = 0.0;

    std::string m_localVirtualIP;
    std::vector<ConnectedPeer> m_peers;

    static SteamVpnTunnel* s_pInstance;
};
