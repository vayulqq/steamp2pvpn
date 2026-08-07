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

// Единое место для параметров виртуальной сети — раньше строки "192.168.137.x"
// были продублированы в main.cpp и steam_tunnel.cpp, что легко рассинхронизировать.
namespace VpnConfig {
    constexpr const char* kHostVirtualIP   = "192.168.137.1";
    constexpr const char* kClientVirtualIP = "192.168.137.2";
    constexpr const char* kSubnetPrefix    = "192.168.137."; // + октет
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

    // Запускаются из UI-потока. Сами по себе быстрые (без блокировок на сеть),
    // после успешной инициализации поднимают фоновый сетевой поток.
    bool InitHost(const std::string& virtualIP = VpnConfig::kHostVirtualIP);
    bool InitClient(uint64_t targetSteamID, const std::string& virtualIP = VpnConfig::kClientVirtualIP);
    void Shutdown();

    // Больше не требуется вызывать из основного цикла рендера — сеть крутится
    // в своём потоке. Метод оставлен как no-op для обратной совместимости.
    void Tick() {}

    bool IsHost() const { return m_isHost.load(std::memory_order_relaxed); }
    bool IsClient() const { return m_isClient.load(std::memory_order_relaxed); }
    bool IsActive() const {
        TunnelState s = GetState();
        return s == TunnelState::Connected || s == TunnelState::Connecting || IsHost();
    }

    TunnelState GetState() const { return m_state.load(std::memory_order_relaxed); }
    uint64_t GetTargetSteamID() const { return m_targetSteamID; }

    std::string GetLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    // Возвращает СНИМОК списка пиров — безопасно вызывать из UI-потока,
    // пока сетевой поток параллельно модифицирует оригинал под мьютексом.
    std::vector<ConnectedPeer> GetPeers() const {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        return m_peers;
    }

    static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

private:
    void HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);
    void SetError(const std::string& err);

    // Тело фонового сетевого потока: обслуживает SteamAPI_RunCallbacks,
    // перекачку пакетов Wintun<->Steam и троттлинг пинга. Не зависит от FPS окна.
    void NetworkThreadLoop();
    void TickInternal();

    WintunManager m_wintun;
    HSteamListenSocket m_hListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_hPollGroup = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection m_hostConn = k_HSteamNetConnection_Invalid;

    std::atomic<bool> m_isHost{false};
    std::atomic<bool> m_isClient{false};
    std::atomic<TunnelState> m_state{TunnelState::Disconnected};

    mutable std::mutex m_errorMutex;
    std::string m_lastError;

    uint64_t m_targetSteamID = 0;
    double m_connectStartTime = 0.0;
    double m_lastPingUpdateTime = 0.0;

    std::string m_localVirtualIP;

    mutable std::mutex m_peersMutex;
    std::vector<ConnectedPeer> m_peers;

    std::vector<uint8_t> m_packetBuffer; // Переиспользуемый буфер, живёт только в сетевом потоке

    std::thread m_networkThread;
    std::atomic<bool> m_threadRunning{false};

    static SteamVpnTunnel* s_pInstance;
};
