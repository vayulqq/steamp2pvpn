#include "steam_tunnel.h"
#include <iostream>
#include <algorithm>
#include <chrono>

SteamVpnTunnel* SteamVpnTunnel::s_pInstance = nullptr;

static double GetCurrentTimeSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

SteamVpnTunnel::SteamVpnTunnel() {
    s_pInstance = this;
}

SteamVpnTunnel::~SteamVpnTunnel() {
    Shutdown();
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

void SteamVpnTunnel::SetError(const std::string& err) {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = err;
}

void SteamVpnTunnel::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    if (s_pInstance) {
        s_pInstance->HandleConnectionStatusChanged(pInfo);
    }
}

void SteamVpnTunnel::HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    if (!pInfo || !SteamNetworkingSockets()) return;

    HSteamNetConnection hostConn = m_hostConn.load();

    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
    case k_ESteamNetworkingConnectionState_FindingRoute: {
        if (m_isClient.load() && pInfo->m_hConn == hostConn) {
            m_state.store(TunnelState::Connecting);
        } else if (m_isHost.load()) {
            size_t currentPeers;
            {
                std::lock_guard<std::mutex> lock(m_peersMutex);
                currentPeers = m_peers.size();
            }
            if (currentPeers >= static_cast<size_t>(VpnConfig::kMaxPeerOctet - VpnConfig::kMinPeerOctet + 1)) {
                SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, "VPN full", false);
                break;
            }

            if (SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn) == k_EResultOK) {
                SteamNetworkingSockets()->SetConnectionPollGroup(pInfo->m_hConn, m_hPollGroup);
            }
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_Connected: {
        uint64_t remoteSteamID = pInfo->m_info.m_identityRemote.GetSteamID64();

        if (m_isClient.load() && pInfo->m_hConn == hostConn && remoteSteamID != m_targetSteamID.load()) {
            SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, "Identity mismatch", false);
            m_hostConn.store(k_HSteamNetConnection_Invalid);
            m_isClient.store(false);
            m_state.store(TunnelState::Failed);
            SetError("SteamID удалённой стороны не совпадает с ожидаемым");
            break;
        }

        ConnectedPeer peer;
        peer.steamID = remoteSteamID;
        peer.hConn = pInfo->m_hConn;
        peer.pingMs = 0;

        if (m_isHost.load()) {
            std::lock_guard<std::mutex> lock(m_peersMutex);

            bool usedIPs[256] = { false };
            for (const auto& p : m_peers) {
                if (p.lastOctet >= VpnConfig::kMinPeerOctet && p.lastOctet <= VpnConfig::kMaxPeerOctet) {
                    usedIPs[p.lastOctet] = true;
                }
            }

            int freeOctet = VpnConfig::kMinPeerOctet;
            while (freeOctet <= VpnConfig::kMaxPeerOctet && usedIPs[freeOctet]) {
                freeOctet++;
            }

            if (freeOctet > VpnConfig::kMaxPeerOctet) {
                SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, "VPN full", false);
                break;
            }

            peer.lastOctet = freeOctet;
            peer.virtualIP = std::string(VpnConfig::kSubnetPrefix) + std::to_string(freeOctet);
            m_peers.push_back(peer);
        } else if (m_isClient.load() && pInfo->m_hConn == hostConn) {
            peer.lastOctet = VpnConfig::kHostOctet;
            peer.virtualIP = std::string(VpnConfig::kSubnetPrefix) + std::to_string(VpnConfig::kHostOctet);
            {
                std::lock_guard<std::mutex> lock(m_peersMutex);
                m_peers.push_back(peer);
            }
            m_state.store(TunnelState::Connected);
            SetError("");
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        std::string reasonStr;
        if (pInfo->m_info.m_szEndDebug[0] != '\0') {
            reasonStr = pInfo->m_info.m_szEndDebug;
        } else {
            switch (pInfo->m_info.m_eEndReason) {
            case k_ESteamNetConnectionEnd_Remote_Timeout:
            case k_ESteamNetConnectionEnd_Misc_Timeout:
                reasonStr = "Таймаут соединения (Хост оффлайн или не отвечает)";
                break;
            case k_ESteamNetConnectionEnd_App_Generic:
                reasonStr = "Соединение закрыто пользователем";
                break;
            default:
                reasonStr = "Не удалось установить P2P-соединение (Код: " + std::to_string(pInfo->m_info.m_eEndReason) + ")";
                break;
            }
        }

        SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            std::erase_if(m_peers, [pInfo](const ConnectedPeer& p) { return p.hConn == pInfo->m_hConn; });
        }

        if (m_isClient.load() && pInfo->m_hConn == hostConn) {
            m_hostConn.store(k_HSteamNetConnection_Invalid);
            m_isClient.store(false);
            m_state.store(TunnelState::Failed);
            SetError(reasonStr);
        }
        break;
    }
    default:
        break;
    }
}

bool SteamVpnTunnel::InitHost(const std::string& virtualIP) {
    Shutdown();
    SetError("");

    if (!SteamNetworkingSockets()) {
        m_state.store(TunnelState::Failed);
        SetError("Steam API не активен");
        return false;
    }

    if (!m_wintun.Initialize(L"SteamVpnAdapter", virtualIP)) {
        m_state.store(TunnelState::Failed);
        SetError("Не удалось создать Wintun адаптер (запустите от Администратора)");
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();
    m_hListenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, nullptr);

    if (m_hListenSocket == k_HSteamListenSocket_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        m_state.store(TunnelState::Failed);
        SetError("Не удалось создать P2P сокет Steam");
        return false;
    }

    m_isHost.store(true);
    m_state.store(TunnelState::Connected);

    m_threadRunning.store(true);
    m_networkThread = std::thread(&SteamVpnTunnel::NetworkThreadLoop, this);
    return true;
}

bool SteamVpnTunnel::InitClient(uint64_t targetSteamID, const std::string& virtualIP) {
    Shutdown();
    m_targetSteamID.store(targetSteamID);
    SetError("");

    if (!SteamNetworkingSockets()) {
        m_state.store(TunnelState::Failed);
        SetError("Steam API не активен");
        return false;
    }

    if (targetSteamID == 0) {
        m_state.store(TunnelState::Failed);
        SetError("Некорректный SteamID");
        return false;
    }

    if (!m_wintun.Initialize(L"SteamVpnAdapter", virtualIP)) {
        m_state.store(TunnelState::Failed);
        SetError("Не удалось создать Wintun адаптер (запустите от Администратора)");
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(targetSteamID);

    HSteamNetConnection conn = SteamNetworkingSockets()->ConnectP2P(identity, 0, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        m_state.store(TunnelState::Failed);
        SetError("Ошибка инициализации P2P-подключения");
        return false;
    }

    m_hostConn.store(conn);
    SteamNetworkingSockets()->SetConnectionPollGroup(conn, m_hPollGroup);
    m_isClient.store(true);
    m_state.store(TunnelState::Connecting);
    m_connectStartTime = GetCurrentTimeSeconds();

    m_threadRunning.store(true);
    m_networkThread = std::thread(&SteamVpnTunnel::NetworkThreadLoop, this);
    return true;
}

void SteamVpnTunnel::NetworkThreadLoop() {
    HANDLE wintunEvent = m_wintun.GetReadWaitEvent();

    while (m_threadRunning.load(std::memory_order_relaxed)) {
        if (wintunEvent) {
            WaitForSingleObject(wintunEvent, 10);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        TickInternal();
    }
}

void SteamVpnTunnel::TickInternal() {
    if (m_state.load() == TunnelState::Disconnected || !SteamNetworkingSockets()) return;

    SteamAPI_RunCallbacks();

    HSteamNetConnection hostConn = m_hostConn.load();

    if (m_isClient.load() && m_state.load() == TunnelState::Connecting) {
        if (GetCurrentTimeSeconds() - m_connectStartTime > VpnConfig::kConnectTimeoutSeconds) {
            if (hostConn != k_HSteamNetConnection_Invalid) {
                SteamNetworkingSockets()->CloseConnection(hostConn, 0, nullptr, false);
                m_hostConn.store(k_HSteamNetConnection_Invalid);
            }
            m_isClient.store(false);
            m_state.store(TunnelState::Failed);
            SetError("Превышено время ожидания ответа от Хоста (15с)");
            return;
        }
    }

    bool isHost = m_isHost.load();
    if (m_state.load() != TunnelState::Connected && !isHost) return;

    std::vector<uint8_t> packetBuffer;
    packetBuffer.reserve(2048);

    while (m_wintun.ReceivePacket(packetBuffer)) {
        if (isHost) {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            bool looksIPv4 = packetBuffer.size() >= 20 && (packetBuffer[0] >> 4) == 4;
            uint8_t destFirstOctet = looksIPv4 ? packetBuffer[16] : 0;
            uint8_t destLastOctet = looksIPv4 ? packetBuffer[19] : static_cast<uint8_t>(VpnConfig::kBroadcastOctet);
            bool isMulticast = looksIPv4 && (destFirstOctet >= 224 && destFirstOctet <= 239);
            bool isBroadcast = !looksIPv4 || isMulticast || (destLastOctet == VpnConfig::kBroadcastOctet);

            for (const auto& peer : m_peers) {
                if (isBroadcast || peer.lastOctet == destLastOctet) {
                    SteamNetworkingSockets()->SendMessageToConnection(
                        peer.hConn, packetBuffer.data(), static_cast<uint32_t>(packetBuffer.size()),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

                    if (!isBroadcast) break;
                }
            }
        } else if (m_isClient.load() && hostConn != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->SendMessageToConnection(
                hostConn, packetBuffer.data(), static_cast<uint32_t>(packetBuffer.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
        packetBuffer.clear();
    }

    if (m_hPollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingMessage_t* pMsgs[32];
        int numMsgs;
        do {
            numMsgs = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_hPollGroup, pMsgs, 32);
            for (int i = 0; i < numMsgs; ++i) {
                uint8_t* data = static_cast<uint8_t*>(pMsgs[i]->m_pData);
                size_t size = pMsgs[i]->m_cbSize;
                HSteamNetConnection sourceConn = pMsgs[i]->m_conn;

                bool looksIPv4 = size >= 20 && (data[0] >> 4) == 4;

                if (isHost && looksIPv4) {
                    uint8_t destFirstOctet = data[16];
                    uint8_t destLastOctet = data[19];
                    bool isMulticast = destFirstOctet >= 224 && destFirstOctet <= 239;
                    bool isBroadcast = isMulticast || (destLastOctet == VpnConfig::kBroadcastOctet);

                    if (destLastOctet == VpnConfig::kHostOctet || isBroadcast) {
                        m_wintun.SendPacket(data, size);
                    }

                    if (destLastOctet != VpnConfig::kHostOctet || isBroadcast) {
                        std::lock_guard<std::mutex> lock(m_peersMutex);
                        for (const auto& peer : m_peers) {
                            if (peer.hConn == sourceConn) continue;

                            if (isBroadcast || peer.lastOctet == destLastOctet) {
                                SteamNetworkingSockets()->SendMessageToConnection(
                                    peer.hConn, data, static_cast<uint32_t>(size),
                                    k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

                                if (!isBroadcast) break;
                            }
                        }
                    }
                } else {
                    m_wintun.SendPacket(data, size);
                }

                pMsgs[i]->Release();
            }
        } while (numMsgs == 32);
    }

    double currentTime = GetCurrentTimeSeconds();
    if (currentTime - m_lastPingUpdateTime >= VpnConfig::kPingUpdateIntervalSeconds) {
        m_lastPingUpdateTime = currentTime;
        std::lock_guard<std::mutex> lock(m_peersMutex);
        for (auto& peer : m_peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.hConn, &status, 0, nullptr) == k_EResultOK) {
                peer.pingMs = status.m_nPing;
            }
        }
    }
}

void SteamVpnTunnel::Shutdown() {
    m_threadRunning.store(false, std::memory_order_relaxed);
    if (m_networkThread.joinable()) {
        m_networkThread.join();
    }

    HSteamNetConnection hostConn = m_hostConn.exchange(k_HSteamNetConnection_Invalid);

    if (SteamNetworkingSockets()) {
        if (m_hListenSocket != k_HSteamListenSocket_Invalid) {
            SteamNetworkingSockets()->CloseListenSocket(m_hListenSocket);
            m_hListenSocket = k_HSteamListenSocket_Invalid;
        }

        if (hostConn != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->CloseConnection(hostConn, 0, nullptr, false);
        }

        if (m_hPollGroup != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingSockets()->DestroyPollGroup(m_hPollGroup);
            m_hPollGroup = k_HSteamNetPollGroup_Invalid;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        m_peers.clear();
    }
    m_wintun.Shutdown();
    m_isHost.store(false);
    m_isClient.store(false);
    m_state.store(TunnelState::Disconnected);
    m_targetSteamID.store(0);
}
