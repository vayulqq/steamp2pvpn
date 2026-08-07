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
    // ВАЖНО: этот коллбек всегда прилетает из сетевого потока (там, где мы
    // вызываем SteamAPI_RunCallbacks), поэтому все обращения к разделяемому
    // состоянию (m_peers и т.п.) идут через мьютексы/атомики.
    if (!pInfo || !SteamNetworkingSockets()) return;

    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
    case k_ESteamNetworkingConnectionState_FindingRoute: {
        if (m_isClient.load() && pInfo->m_hConn == m_hostConn) {
            m_state.store(TunnelState::Connecting);
        } else if (m_isHost.load()) {
            // Простейшая защита от бесконтрольного роста сети: не принимаем
            // больше пиров, чем есть свободных октетов подсети.
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

        // Проверка личности: клиент принимает "успешное подключение" только
        // если это действительно тот SteamID, к которому он подключался.
        // Раньше любое событие Connected на m_hostConn считалось валидным,
        // но SteamID уже зашит в идентификаторе назначения при ConnectP2P,
        // так что несовпадение практически невозможно без спуфинга —
        // однако явная проверка не будет лишней и документирует инвариант.
        if (m_isClient.load() && pInfo->m_hConn == m_hostConn && remoteSteamID != m_targetSteamID) {
            SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, "Identity mismatch", false);
            m_hostConn = k_HSteamNetConnection_Invalid;
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

            // Быстрый поиск минимального свободного октета без выделения строк в куче
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
                // Подсеть исчерпана (гонка с проверкой выше) — отклоняем.
                SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, "VPN full", false);
                break;
            }

            peer.lastOctet = freeOctet;
            peer.virtualIP = std::string(VpnConfig::kSubnetPrefix) + std::to_string(freeOctet);
            m_peers.push_back(peer);
        } else if (m_isClient.load() && pInfo->m_hConn == m_hostConn) {
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

        if (m_isClient.load() && pInfo->m_hConn == m_hostConn) {
            m_hostConn = k_HSteamNetConnection_Invalid;
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
    m_localVirtualIP = virtualIP;
    SetError("");

    if (!SteamNetworkingSockets()) {
        m_state.store(TunnelState::Failed);
        SetError("Steam API не активен");
        return false;
    }

    if (!m_wintun.Initialize(L"SteamVpnAdapter", m_localVirtualIP)) {
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
    m_localVirtualIP = virtualIP;
    m_targetSteamID = targetSteamID;
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

    if (!m_wintun.Initialize(L"SteamVpnAdapter", m_localVirtualIP)) {
        m_state.store(TunnelState::Failed);
        SetError("Не удалось создать Wintun адаптер (запустите от Администратора)");
        return false;
    }

    m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(targetSteamID);

    m_hostConn = SteamNetworkingSockets()->ConnectP2P(identity, 0, 0, nullptr);
    if (m_hostConn == k_HSteamNetConnection_Invalid || m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
        Shutdown();
        m_state.store(TunnelState::Failed);
        SetError("Ошибка инициализации P2P-подключения");
        return false;
    }

    SteamNetworkingSockets()->SetConnectionPollGroup(m_hostConn, m_hPollGroup);
    m_isClient.store(true);
    m_state.store(TunnelState::Connecting);
    m_connectStartTime = GetCurrentTimeSeconds();

    m_threadRunning.store(true);
    m_networkThread = std::thread(&SteamVpnTunnel::NetworkThreadLoop, this);
    return true;
}

void SteamVpnTunnel::NetworkThreadLoop() {
    // Сетевой поток живёт независимо от рендер-цикла ImGui/GLFW: раньше вся
    // перекачка пакетов вызывалась из главного цикла окна, из-за чего
    // пропускная способность VPN была искусственно привязана к FPS/VSync
    // (glfwSwapInterval(1)) и к тому, крутится ли вообще окно (сворачивание,
    // перетаскивание и т.д. останавливают glfwPollEvents). Здесь мы будим
    // поток либо по приходу пакета в Wintun (событие ядра), либо по таймеру,
    // чтобы не реже определённой частоты сервисировать SteamAPI_RunCallbacks
    // и опрашивать PollGroup — это нужно, даже если трафика в TUN нет.
    HANDLE wintunEvent = m_wintun.GetReadWaitEvent();

    while (m_threadRunning.load(std::memory_order_relaxed)) {
        if (wintunEvent) {
            // Таймаут ограничивает интервал обслуживания Steam-коллбэков и
            // входящих P2P-сообщений даже при отсутствии локального трафика.
            WaitForSingleObject(wintunEvent, 10);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        TickInternal();
    }
}

void SteamVpnTunnel::TickInternal() {
    if (m_state.load() == TunnelState::Disconnected || !SteamNetworkingSockets()) return;

    // ШАГ 1: Обработка статусов и событий сети Steam
    SteamAPI_RunCallbacks();

    // Таймаут подключения клиентов
    if (m_isClient.load() && m_state.load() == TunnelState::Connecting) {
        if (GetCurrentTimeSeconds() - m_connectStartTime > VpnConfig::kConnectTimeoutSeconds) {
            // Не вызываем полный Shutdown() из сетевого потока (он делает
            // join самого себя), просто закрываем соединение и переходим в Failed.
            if (m_hostConn != k_HSteamNetConnection_Invalid) {
                SteamNetworkingSockets()->CloseConnection(m_hostConn, 0, nullptr, false);
                m_hostConn = k_HSteamNetConnection_Invalid;
            }
            m_isClient.store(false);
            m_state.store(TunnelState::Failed);
            SetError("Превышено время ожидания ответа от Хоста (15с)");
            return;
        }
    }

    bool isHost = m_isHost.load();
    if (m_state.load() != TunnelState::Connected && !isHost) return;

    // ШАГ 2: Чтение IP-пакетов из Wintun и адресная пересылка в Steam.
    // Полный дренаж очереди Wintun за одну итерацию — без искусственного
    // ограничения "один кадр GUI = один пакет".
    m_packetBuffer.clear();
    while (m_wintun.ReceivePacket(m_packetBuffer)) {
        if (isHost) {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            // 19-й байт в IPv4 заголовке — это последний октет Destination IP (192.168.137.X).
            // Если пакет не похож на IPv4 (например IPv6) — раздать адресно
            // мы не можем, поэтому рассылаем всем как broadcast (best effort).
            bool looksIPv4 = m_packetBuffer.size() >= 20 && (m_packetBuffer[0] >> 4) == 4;
            uint8_t destOctet = looksIPv4 ? m_packetBuffer[19] : static_cast<uint8_t>(VpnConfig::kBroadcastOctet);

            for (const auto& peer : m_peers) {
                if (destOctet == VpnConfig::kBroadcastOctet || peer.lastOctet == destOctet) {
                    SteamNetworkingSockets()->SendMessageToConnection(
                        peer.hConn, m_packetBuffer.data(), static_cast<uint32_t>(m_packetBuffer.size()),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

                    if (destOctet != VpnConfig::kBroadcastOctet) break; // Точечный адресат найден
                }
            }
        } else if (m_isClient.load() && m_hostConn != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->SendMessageToConnection(
                m_hostConn, m_packetBuffer.data(), static_cast<uint32_t>(m_packetBuffer.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
        m_packetBuffer.clear();
    }

    // ШАГ 3: Чтение P2P-кадров из Steam PollGroup + коммутация Клиент <-> Клиент.
    // Дренируем очередь ПОЛНОСТЬЮ (а не максимум 32 сообщения за "кадр"),
    // иначе при всплеске трафика остаток обрабатывался бы только на
    // следующем цикле рендера — раньше это было узким местом.
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
                    uint8_t destOctet = data[19];

                    if (destOctet == VpnConfig::kHostOctet || destOctet == VpnConfig::kBroadcastOctet) {
                        // Пакет предназначен Хосту (или всем) — отсылаем в Wintun Хоста.
                        m_wintun.SendPacket(data, size);
                    }

                    // Если пакет адресован не хосту — коммутируем его между клиентами
                    // напрямую, не гоняя через локальный TUN.
                    if (destOctet != VpnConfig::kHostOctet) {
                        std::lock_guard<std::mutex> lock(m_peersMutex);
                        for (const auto& peer : m_peers) {
                            // Не отправляем broadcast обратно тому же соединению,
                            // от которого он пришёл — раньше это создавало эхо
                            // пакета себе же при destOctet == 255.
                            if (peer.hConn == sourceConn) continue;

                            if (peer.lastOctet == destOctet || destOctet == VpnConfig::kBroadcastOctet) {
                                SteamNetworkingSockets()->SendMessageToConnection(
                                    peer.hConn, data, static_cast<uint32_t>(size),
                                    k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

                                if (destOctet != VpnConfig::kBroadcastOctet) break;
                            }
                        }
                    }
                } else {
                    // На Клиенте (или не-IPv4 кадр на Хосте, который мы не можем
                    // маршрутизировать адресно) — просто пишем пришедший кадр в свой Wintun.
                    m_wintun.SendPacket(data, size);
                }

                pMsgs[i]->Release();
            }
        } while (numMsgs == 32); // вычерпываем очередь, если пришло ровно до предела буфера
    }

    // Троттлинг обновления сетевой метрики Ping RTT (1 раз в секунду)
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
    // Останавливаем и join'им сетевой поток ДО закрытия Steam-хэндлов,
    // чтобы TickInternal() не работал с уже уничтоженными объектами.
    m_threadRunning.store(false, std::memory_order_relaxed);
    if (m_networkThread.joinable()) {
        m_networkThread.join();
    }

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

    {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        m_peers.clear();
    }
    m_packetBuffer.clear();
    m_wintun.Shutdown();
    m_isHost.store(false);
    m_isClient.store(false);
    m_state.store(TunnelState::Disconnected);
    m_targetSteamID = 0;
}
