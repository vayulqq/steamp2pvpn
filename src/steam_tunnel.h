#pragma once

#include "wintun_manager.h"
#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>

namespace VpnConfig {
    constexpr const char* kHostVirtualIP   = "192.168.137.1";
    constexpr const char* kClientVirtualIP = "192.168.137.2";
    constexpr const char* kSubnetPrefix    = "192.168.137.";
    constexpr int         kHostOctet       = 1;
    constexpr int         kBroadcastOctet  = 255;
    constexpr int         kMinPeerOctet    = 2;
    constexpr int         kMaxPeerOctet    = 254;
    constexpr double      kConnectTimeoutSeconds = 15.0;
    constexpr double      kPingUpdateIntervalSeconds = 1.0;
}

enum class TunnelState {
    Disconnected,
    Connecting,
    Connected,
    Failed
};

struct ConnectedPeer {
    uint64_t steamID = 0;
    std::string virtualIP;
    int lastOctet = 0;
    int pingMs = 0;
    HSteamNetConnection hConn = k_HSteamNetConnection_Invalid;
};

class SteamVpnTunnel {
public:
    SteamVpnTunnel();
    ~SteamVpnTunnel();

    bool InitHost(const std::string& virtualIP = VpnConfig::kHostVirtualIP);
    bool InitClient(uint64_t targetSteamID, const std::string& virtualIP = VpnConfig::kClientVirtualIP);
    void Shutdown();

    bool IsHost() const { return m_isHost.load(std::memory_order_relaxed); }
    bool IsClient() const { return m_isClient.load(std::memory_order_relaxed); }
    bool IsThreadRunning() const { return m_threadRunning.load(std::memory_order_relaxed); }

    TunnelState GetState() const { return m_state.load(std::memory_order_relaxed); }
    uint64_t GetTargetSteamID() const { return m_targetSteamID.load(std::memory_order_relaxed); }

    std::string GetLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    std::vector<ConnectedPeer> GetPeers() const {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        return m_peers;
    }

    static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

private:
    void HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);
    void SetError(const std::string& err);

    void NetworkThreadLoop();
    void TickInternal();

    WintunManager m_wintun;
    HSteamListenSocket m_hListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_hPollGroup = k_HSteamNetPollGroup_Invalid;
    std::atomic<HSteamNetConnection> m_hostConn{k_HSteamNetConnection_Invalid};

    std::atomic<bool> m_isHost{false};
    std::atomic<bool> m_isClient{false};
    std::atomic<TunnelState> m_state{TunnelState::Disconnected};

    mutable std::mutex m_errorMutex;
    std::string m_lastError;

    std::atomic<uint64_t> m_targetSteamID{0};
    double m_connectStartTime = 0.0;
    double m_lastPingUpdateTime = 0.0;

    mutable std::mutex m_peersMutex;
    std::vector<ConnectedPeer> m_peers;

    std::thread m_networkThread;
    std::atomic<bool> m_threadRunning{false};

    static SteamVpnTunnel* s_pInstance;
};
