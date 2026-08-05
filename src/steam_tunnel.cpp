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

void SteamVpnTunnel::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    if (s_pInstance) {
        s_pInstance->HandleConnectionStatusChanged(pInfo);
    }
}

void SteamVpnTunnel::HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
    case k_ESteamNetworkingConnectionState_FindingRoute: {
        if (m_isClient && pInfo->m_hConn == m_hostConn) {
            m_state = TunnelState::Connecting;
        } else if (m_isHost) {
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

        if (m_isHost) {
            // Авто-выделение следующего IP: 192.168.137.2, .3, .4 ...
            int nextOctet = 2 + static_cast<int>(m_peers.size());
            peer.virtualIP = "192.168.137." + std::to_string(nextOctet);
            m_peers.push_back(peer);
        } else if (m_isClient && pInfo->m_hConn == m_hostConn) {
            peer.virtualIP = "192.168.137.1";
            m_peers.push_back(peer);
            m_state = TunnelState::Connected;
            m_lastError.clear();
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
        std::erase_if(m_peers, [pInfo](const ConnectedPeer& p) { return p.hConn == pInfo->m_hConn; });

        if (m_isClient && pInfo->m_hConn == m_hostConn) {
            m_hostConn = k_HSteamNetConnection_Invalid;
            m_isClient = false;
            m_state = TunnelState::Failed;
            m_lastError = reasonStr;
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
    m_lastError.clear();

    if (!m_wintun.Initialize(L"SteamVpnAdapter", m_localVirtualIP)) {
        m_state = TunnelState::Failed;
        m_lastError = "Не удалось создать Wintun адаптер (запустите от Администратора)";
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();
    m_hListenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, nullptr);

    if (m_hListenSocket == k_HSteamListenSocket_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        m_state = TunnelState::Failed;
        m_lastError = "Не удалось создать P2P сокет Steam";
        return false;
    }

    m_isHost = true;
    m_state = TunnelState::Connected;
    return true;
}

bool SteamVpnTunnel::InitClient(uint64_t targetSteamID, const std::string& virtualIP) {
    Shutdown();
    m_localVirtualIP = virtualIP;
    m_targetSteamID = targetSteamID;
    m_lastError.clear();

    if (targetSteamID == 0) {
        m_state = TunnelState::Failed;
        m_lastError = "Некорректный SteamID";
        return false;
    }

    if (!m_wintun.Initialize(L"SteamVpnAdapter", m_localVirtualIP)) {
        m_state = TunnelState::Failed;
        m_lastError = "Не удалось создать Wintun адаптер (запустите от Администратора)";
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(targetSteamID);

    m_hostConn = SteamNetworkingSockets()->ConnectP2P(identity, 0, 0, nullptr);
    if (m_hostConn == k_HSteamNetConnection_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        m_state = TunnelState::Failed;
        m_lastError = "Ошибка инициализации P2P-подключения";
        return false;
    }

    SteamNetworkingSockets()->SetConnectionPollGroup(m_hostConn, m_hPollGroup);
    m_isClient = true;
    m_state = TunnelState::Connecting;
    m_connectStartTime = GetCurrentTimeSeconds();
    return true;
}

void SteamVpnTunnel::Tick() {
    if (m_state == TunnelState::Disconnected) return;

    // ШАГ 1: Обработка статусов и событий сети Steam
    SteamAPI_RunCallbacks();

    // Таймаут подключения клиентов (15 секунд)
    if (m_isClient && m_state == TunnelState::Connecting) {
        if (GetCurrentTimeSeconds() - m_connectStartTime > 15.0) {
            Shutdown();
            m_state = TunnelState::Failed;
            m_lastError = "Превышено время ожидания ответа от Хоста (15с)";
            return;
        }
    }

    // Трафик передаем только при полном установлении связи
    if (m_state != TunnelState::Connected && !m_isHost) return;

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
    m_state = TunnelState::Disconnected;
    m_targetSteamID = 0;
}
