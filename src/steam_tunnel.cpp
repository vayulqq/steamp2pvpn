#include "steam_tunnel.h"
#include <iostream>
#include <algorithm>

SteamVpnTunnel* SteamVpnTunnel::s_pInstance = nullptr;

SteamVpnTunnel::SteamVpnTunnel() {
    s_pInstance = this;
}

SteamVpnTunnel::~SteamVpnTunnel() {
    Shutdown();
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

void SteamVpnTunnel::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    if (s_pInstance) {
        s_pInstance->HandleConnectionStatusChanged(pInfo);
    }
}

void SteamVpnTunnel::HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting: {
        if (m_isHost) {
            if (SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn) == k_EResultOK) {
                SteamNetworkingSockets()->SetConnectionPollGroup(pInfo->m_hConn, m_hPollGroup);
            }
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_Connected: {
        uint64_t remoteSteamID = pInfo->m_info.m_identityRemote.GetSteamID64();
        ConnectedPeer peer;
        peer.steamID = remoteSteamID;
        peer.hConn = pInfo->m_hConn;
        peer.pingMs = 0;
        peer.virtualIP = m_isHost ? "192.168.137.2" : "192.168.137.1";

        m_peers.push_back(peer);
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
        std::erase_if(m_peers, [pInfo](const ConnectedPeer& p) { return p.hConn == pInfo->m_hConn; });
        if (m_isClient && pInfo->m_hConn == m_hostConn) {
            m_hostConn = k_HSteamNetConnection_Invalid;
            m_isClient = false;
        }
        break;
    }
    default:
        break;
    }
}

bool SteamVpnTunnel::InitHost(const std::string& virtualIP) {
    Shutdown();
    m_localVirtualIP = virtualIP;

    if (!m_wintun.Initialize(L"SteamVpnAdapter", m_localVirtualIP)) {
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();
    m_hListenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, nullptr);

    if (m_hListenSocket == k_HSteamListenSocket_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        return false;
    }

    m_isHost = true;
    return true;
}

bool SteamVpnTunnel::InitClient(uint64_t targetSteamID, const std::string& virtualIP) {
    Shutdown();
    m_localVirtualIP = virtualIP;

    if (!m_wintun.Initialize(L"SteamVpnAdapter", m_localVirtualIP)) {
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(targetSteamID);

    m_hostConn = SteamNetworkingSockets()->ConnectP2P(identity, 0, 0, nullptr);
    if (m_hostConn == k_HSteamNetConnection_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        return false;
    }

    SteamNetworkingSockets()->SetConnectionPollGroup(m_hostConn, m_hPollGroup);
    m_isClient = true;
    return true;
}

void SteamVpnTunnel::Tick() {
    if (!IsActive()) return;

    // ШАГ 1: Обработка статусов и событий сети Steam
    SteamAPI_RunCallbacks();

    // ШАГ 2: Чтение IP-пакетов из Wintun и отправка в Steam
    std::vector<uint8_t> packetBuffer;
    while (m_wintun.ReceivePacket(packetBuffer)) {
        if (m_isHost) {
            for (const auto& peer : m_peers) {
                SteamNetworkingSockets()->SendMessageToConnection(
                    peer.hConn, packetBuffer.data(), static_cast<uint32_t>(packetBuffer.size()),
                    k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
            }
        } else if (m_isClient && m_hostConn != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->SendMessageToConnection(
                m_hostConn, packetBuffer.data(), static_cast<uint32_t>(packetBuffer.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
    }

    // ШАГ 3: Чтение P2P-кадров из Steam PollGroup и запись в Wintun
    if (m_hPollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingMessage_t* pMsgs[32];
        int numMsgs = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_hPollGroup, pMsgs, 32);
        for (int i = 0; i < numMsgs; ++i) {
            m_wintun.SendPacket(pMsgs[i]->m_pData, pMsgs[i]->m_cbSize);
            pMsgs[i]->Release();
        }
    }

    // Обновление сетевой метрики Ping RTT
    for (auto& peer : m_peers) {
        SteamNetConnectionRealTimeStatus_t status;
        if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.hConn, &status, 0, nullptr) == k_EResultOK) {
            peer.pingMs = status.m_nPing;
        }
    }
}

void SteamVpnTunnel::Shutdown() {
    if (m_hListenSocket != k_HSteamListenSocket_Invalid) {
        SteamNetworkingSockets()->CloseListenSocket(m_hListenSocket);
        m_hListenSocket = k_HSteamListenSocket_Invalid;
    }

    if (m_hostConn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_hostConn, 0, nullptr, false);
        m_hostConn = k_HSteamNetConnection_Invalid;
    }

    if (m_hPollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_hPollGroup);
        m_hPollGroup = k_HSteamNetPollGroup_Invalid;
    }

    m_peers.clear();
    m_wintun.Shutdown();
    m_isHost = false;
    m_isClient = false;
}
