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
    m_packetBuffer.reserve(2048); // Предварительно резервируем память под MTU кадра
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
    if (!pInfo || !SteamNetworkingSockets()) return;

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
            // Быстрый поиск минимального свободного октета без выделения строк в куче
            bool usedIPs[256] = { false };
            for (const auto& p : m_peers) {
                if (p.lastOctet >= 2 && p.lastOctet <= 254) {
                    usedIPs[p.lastOctet] = true;
                }
            }

            int freeOctet = 2;
            while (freeOctet <= 254 && usedIPs[freeOctet]) {
                freeOctet++;
            }

            peer.lastOctet = freeOctet;
            peer.virtualIP = "192.168.137." + std::to_string(freeOctet);
            m_peers.push_back(peer);
        } else if (m_isClient && pInfo->m_hConn == m_hostConn) {
            peer.lastOctet = 1;
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

    if (!SteamNetworkingSockets()) {
        m_state = TunnelState::Failed;
        m_lastError = "Steam API не активен";
        return false;
    }

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

    if (!SteamNetworkingSockets()) {
        m_state = TunnelState::Failed;
        m_lastError = "Steam API не активен";
        return false;
    }

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
    if (m_state == TunnelState::Disconnected || !SteamNetworkingSockets()) return;

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

    if (m_state != TunnelState::Connected && !m_isHost) return;

    // ШАГ 2: Чтение IP-пакетов из Wintun Хоста и адресная пересылка в Steam
    m_packetBuffer.clear();
    while (m_wintun.ReceivePacket(m_packetBuffer)) {
        if (m_isHost) {
            // 19-й байт в IPv4 заголовке — это последний октет Destination IP (192.168.137.X)
            uint8_t destOctet = (m_packetBuffer.size() >= 20) ? m_packetBuffer[19] : 255;

            for (const auto& peer : m_peers) {
                // Отправляем только адресату ИЛИ всем, если это Broadcast (.255)
                if (destOctet == 255 || peer.lastOctet == destOctet) {
                    SteamNetworkingSockets()->SendMessageToConnection(
                        peer.hConn, m_packetBuffer.data(), static_cast<uint32_t>(m_packetBuffer.size()),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

                    if (destOctet != 255) break; // Точечный адресат найден — прерываем цикл
                }
            }
        } else if (m_isClient && m_hostConn != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->SendMessageToConnection(
                m_hostConn, m_packetBuffer.data(), static_cast<uint32_t>(m_packetBuffer.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
        m_packetBuffer.clear();
    }

    // ШАГ 3: Чтение P2P-кадров из Steam PollGroup + Коммутация Клиент <-> Клиент
    if (m_hPollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingMessage_t* pMsgs[32];
        int numMsgs = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_hPollGroup, pMsgs, 32);
        for (int i = 0; i < numMsgs; ++i) {
            uint8_t* data = static_cast<uint8_t*>(pMsgs[i]->m_pData);
            size_t size = pMsgs[i]->m_cbSize;

            if (m_isHost && size >= 20 && (data[0] >> 4) == 4) { // Проверка IPv4
                uint8_t destOctet = data[19];

                if (destOctet == 1 || destOctet == 255) {
                    // Пакет предназначен Хосту или всем — отсылаем в Wintun Хоста
                    m_wintun.SendPacket(data, size);
                }
                
                // Если пакет идет от Клиента к другому Клиенту — пересылаем его напрямую!
                if (destOctet != 1) {
                    for (const auto& peer : m_peers) {
                        if (peer.lastOctet == destOctet || destOctet == 255) {
                            SteamNetworkingSockets()->SendMessageToConnection(
                                peer.hConn, data, static_cast<uint32_t>(size),
                                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

                            if (destOctet != 255) break;
                        }
                    }
                }
            } else {
                // На Клиенте — просто пишем пришедший кадр в Wintun
                m_wintun.SendPacket(data, size);
            }

            pMsgs[i]->Release();
        }
    }

    // Троттлинг обновления сетевой метрики Ping RTT (1 раз в секунду)
    double currentTime = GetCurrentTimeSeconds();
    if (currentTime - m_lastPingUpdateTime >= 1.0) {
        m_lastPingUpdateTime = currentTime;
        for (auto& peer : m_peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.hConn, &status, 0, nullptr) == k_EResultOK) {
                peer.pingMs = status.m_nPing;
            }
        }
    }
}

void SteamVpnTunnel::Shutdown() {
    if (SteamNetworkingSockets()) {
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
    }

    m_peers.clear();
    m_packetBuffer.clear();
    m_wintun.Shutdown();
    m_isHost = false;
    m_isClient = false;
    m_state = TunnelState::Disconnected;
    m_targetSteamID = 0;
}
