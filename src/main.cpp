#include <winsock2.h>
#include <windows.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <steam/steam_api.h>
#include "steam_tunnel.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Простой файловый логгер для диагностики сетевых проблем (InitRelayNetworkAccess,
// смены статуса SDR-сети и т.п.). Пишет одновременно в vpn_log.txt рядом с exe
// и в stderr (видно, если запускать из консоли). Каждая строка сразу сбрасывается
// на диск (std::endl), чтобы лог не терялся, если приложение зависнет/упадёт.
// ---------------------------------------------------------------------------
static std::ofstream g_logFile;

static std::string TimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

static void LogLine(const std::string& message) {
    std::string line = "[" + TimestampNow() + "] " + message;
    std::cerr << line << "\n";
    if (g_logFile.is_open()) {
        g_logFile << line << std::endl; // endl = flush, лог не теряется при зависании/крэше
    }
}

// Человекочитаемое имя для ESteamNetworkingAvailability, чтобы в логе не были
// голые числа вроде "100" или "-101".
static const char* RelayAvailabilityToString(ESteamNetworkingAvailability status) {
    switch (status) {
        case k_ESteamNetworkingAvailability_CannotTry: return "CannotTry (нет зависимого ресурса, напр. нет интернета)";
        case k_ESteamNetworkingAvailability_Failed:     return "Failed (пробовали достаточно долго — не получилось)";
        case k_ESteamNetworkingAvailability_Previously:  return "Previously (раньше работало, сейчас проблема)";
        case k_ESteamNetworkingAvailability_Retrying:    return "Retrying (была ошибка, повторяем попытку)";
        case k_ESteamNetworkingAvailability_NeverTried:  return "NeverTried (ещё не пытались)";
        case k_ESteamNetworkingAvailability_Waiting:     return "Waiting (ждём зависимый ресурс)";
        case k_ESteamNetworkingAvailability_Attempting:  return "Attempting (активно пытаемся, ещё не готово)";
        case k_ESteamNetworkingAvailability_Current:     return "Current (готово)";
        case k_ESteamNetworkingAvailability_Unknown:     return "Unknown (внутреннее служебное значение)";
        default: return "???";
    }
}


// Если рядом нет steam_appid.txt — создаём его с тестовым AppID Spacewar (480),
// НО только если он не задан переменной окружения SteamAppId. Это позволяет
// собрать релиз под реальную игру без ручного редактирования файла:
// пользователь может выставить свой AppID через переменную окружения или
// положить собственный steam_appid.txt рядом с exe — в обоих случаях
// программа его не перезапишет.
void EnsureSteamAppId() {
    if (std::getenv("SteamAppId") != nullptr) {
        return; // AppID уже задан через окружение — файл не нужен
    }

    std::ifstream check("steam_appid.txt");
    if (!check.good()) {
        std::ofstream out("steam_appid.txt");
        out << "480"; // Spacewar AppID для тестов, если ничего другого не указано
        std::cerr << "[SteamAppId] Файл steam_appid.txt не найден, создан с тестовым AppID 480 (Spacewar).\n"
                     "[SteamAppId] Для релизной сборки положите рядом с exe свой steam_appid.txt "
                     "или задайте переменную окружения SteamAppId.\n";
    }
}

int main(int argc, char** argv) {
    g_logFile.open("vpn_log.txt", std::ios::app);
    LogLine("========== Запуск Steam P2P VPN ==========");

    EnsureSteamAppId();

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (!glfwInit()) {
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(720, 480, "Steam P2P VPN", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // Пытаемся найти системный шрифт с кириллицей. Раньше при отсутствии
    // обоих файлов (нестандартная/минимальная установка Windows, другой
    // язык интерфейса ОС) ImGui оставался ВООБЩЕ без загруженного шрифта,
    // и все русские надписи превращались в пустые прямоугольники.
    // Теперь при неудаче явно подгружаем встроенный шрифт ImGui по умолчанию
    // (без кириллицы, но хотя бы читаемый), чтобы UI не ломался визуально.
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    if (!font) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    }
    if (!font) {
        std::cerr << "[Fonts] Системные шрифты с кириллицей не найдены, использую встроенный шрифт ImGui "
                     "(кириллица отображаться не будет).\n";
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Инициализация Steam API с проверкой
    SteamErrMsg errMsg = {0};
    bool steamInitialized = (SteamAPI_InitEx(&errMsg) == k_ESteamAPIInitResult_OK);
    LogLine(steamInitialized
        ? "[SteamAPI] SteamAPI_InitEx: успех"
        : std::string("[SteamAPI] SteamAPI_InitEx: ОШИБКА — ") + errMsg);

    if (steamInitialized && SteamNetworkingUtils()) {
        SteamNetworkingUtils()->SetGlobalConfigValuePtr(
            k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            (void*)SteamVpnTunnel::OnSteamNetConnectionStatusChanged
        );

        // Раньше ConnectP2P мог мгновенно вернуть невалидный хендл ("Ошибка
        // инициализации P2P-подключения"), если вызывался сразу после старта
        // программы — Steam-клиент ещё не успел получить конфигурацию сети
        // релеев (SDR) и измерить пинги до них. InitRelayNetworkAccess() явно
        // запускает этот процесс заранее, а не откладывает его до первого
        // реального ConnectP2P/CreateListenSocketP2P.
        LogLine("[Relay] Вызов InitRelayNetworkAccess() (первичная инициализация при старте)");
        SteamNetworkingUtils()->InitRelayNetworkAccess();
    } else {
        std::cerr << "[SteamAPI] Ошибка инициализации: " << errMsg << "\n";
    }

    SteamVpnTunnel tunnel;
    char targetSteamIDBuf[64] = "";
    double relayWaitStartTime = -1.0; // момент, когда впервые заметили "сеть не готова"

    // Для логирования только ИЗМЕНЕНИЙ статуса (а не каждый кадр — иначе лог
    // распухнет за секунду до гигабайт, т.к. цикл крутится на частоте кадров).
    ESteamNetworkingAvailability lastLoggedRelayStatus = k_ESteamNetworkingAvailability_Unknown;
    bool relayStatusLoggedOnce = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Сетевой цикл (Wintun <-> Steam) теперь крутится в фоновом потоке
        // внутри SteamVpnTunnel и не зависит от FPS окна — tunnel.Tick()
        // оставлен как no-op вызов только для наглядности жизненного цикла.
        // SteamAPI_RunCallbacks() тоже вызывается из фонового потока.
        tunnel.Tick();

        // Статус готовности relay-сети Steam (SDR) опрашивается каждый кадр —
        // вызов дешёвый (просто читает закешированное состояние), тяжёлая
        // работа по факту происходит асинхронно внутри Steam-клиента после
        // InitRelayNetworkAccess(). Пока статус не "Current", ConnectP2P и
        // CreateListenSocketP2P могут не работать как надо.
        ESteamNetworkingAvailability relayStatus = k_ESteamNetworkingAvailability_Unknown;
        SteamRelayNetworkStatus_t relayDetails;
        if (steamInitialized && SteamNetworkingUtils()) {
            relayStatus = SteamNetworkingUtils()->GetRelayNetworkStatus(&relayDetails);
        }
        bool relayNetworkReady = (relayStatus == k_ESteamNetworkingAvailability_Current);

        // Логируем КАЖДОЕ изменение статуса сети релеев с таймстампом и
        // диагностическим сообщением от Steam (relayDetails.m_debugMsg) —
        // это как раз то, что видно в vpn_log.txt при разборе зависшего
        // "Attempt #1 to fetch config..." и подобных проблем.
        if (!relayStatusLoggedOnce || relayStatus != lastLoggedRelayStatus) {
            std::ostringstream oss;
            oss << "[Relay] Статус: " << RelayAvailabilityToString(relayStatus);
            if (relayDetails.m_debugMsg[0] != '\0') {
                oss << " | debug: " << relayDetails.m_debugMsg;
            }
            LogLine(oss.str());
            lastLoggedRelayStatus = relayStatus;
            relayStatusLoggedOnce = true;
        }

        // Считаем, сколько времени подряд сеть релеев не готова, чтобы отличить
        // "обычная пара секунд после запуска" от "что-то реально не так"
        // (фаервол, офлайн-режим Steam, недоступен api.steampowered.com и т.п.).
        double now = glfwGetTime();
        if (relayNetworkReady) {
            relayWaitStartTime = -1.0;
        } else if (relayWaitStartTime < 0.0) {
            relayWaitStartTime = now;
        }
        double relayWaitSeconds = (relayWaitStartTime >= 0.0) ? (now - relayWaitStartTime) : 0.0;
        bool relayWaitTooLong = relayWaitSeconds > 30.0;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Steam P2P VPN Panel", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        if (steamInitialized && SteamUser()) {
            CSteamID myID = SteamUser()->GetSteamID();
            const char* myName = SteamFriends() ? SteamFriends()->GetPersonaName() : "Unknown";
            ImGui::Text("Профиль Steam: %s (ID: %llu)", myName, myID.ConvertToUint64());

            if (relayNetworkReady) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Сеть релеев Steam (SDR): готова");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
                ImGui::TextWrapped("Сеть релеев Steam (SDR): не готова уже %.0f сек (%s)",
                    relayWaitSeconds,
                    relayDetails.m_debugMsg[0] != '\0' ? relayDetails.m_debugMsg : "инициализация...");
                ImGui::PopStyleColor();

                if (relayWaitTooLong) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextWrapped(
                        "Долгое ожидание обычно значит: Steam-клиент в оффлайн-режиме, "
                        "фаервол/антивирус блокирует api.steampowered.com или UDP к relay-серверам Valve, "
                        "либо для этого AppID не настроен P2P Networking в Steamworks.");
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    if (ImGui::Button("Повторить попытку (InitRelayNetworkAccess)", ImVec2(400, 26))) {
                        // Согласно докам Valve, повторный вызов InitRelayNetworkAccess()
                        // форсирует новую попытку, если предыдущая зависла/провалилась —
                        // не требует перезапуска всего приложения.
                        LogLine("[Relay] Вызов InitRelayNetworkAccess() (ручной повтор, ждали " +
                            std::to_string((int)relayWaitSeconds) + " сек, текущий статус: " +
                            RelayAvailabilityToString(relayStatus) + ")");
                        if (SteamNetworkingUtils()) {
                            SteamNetworkingUtils()->InitRelayNetworkAccess();
                        }
                        relayWaitStartTime = now; // сбрасываем таймер ожидания заново
                    }
                }
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::Text("Steam API не активен! Убедитесь, что клиент Steam запущен.");
            if (errMsg[0] != '\0') {
                ImGui::Text("Детали: %s", errMsg);
            }
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        TunnelState state = tunnel.GetState();

        if (state == TunnelState::Disconnected || state == TunnelState::Failed) {
            if (state == TunnelState::Failed) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("Ошибка подключения: %s", tunnel.GetLastError().c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::Separator();
            }

            // Блокируем кнопки, если Steam API выключен или сеть релеев ещё не готова
            if (!steamInitialized) ImGui::BeginDisabled();
            if (!relayNetworkReady) ImGui::BeginDisabled();

            if (ImGui::Button("Запустить сеть (Хост)", ImVec2(340, 30))) {
                tunnel.InitHost();
            }

            ImGui::Spacing();
            ImGui::InputText("Target SteamID", targetSteamIDBuf, sizeof(targetSteamIDBuf));

            uint64_t targetID = std::strtoull(targetSteamIDBuf, nullptr, 10);
            // Грубая проверка формата SteamID64: они лежат в диапазоне,
            // начинающемся с 0x0110000100000000 (~76561197960265728).
            // Это не полноценная валидация (не проверяет тип аккаунта/юниверс),
            // но отсекает случайный мусор вроде "1" или "123".
            bool steamIdLooksValid = (targetID >= 76561197960265728ULL);
            bool canConnect = steamIdLooksValid; // relayNetworkReady уже учтён внешним BeginDisabled выше

            if (!canConnect) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Подключиться к Хосту (Клиент)", ImVec2(340, 30))) {
                tunnel.InitClient(targetID);
            }
            if (!canConnect) {
                ImGui::EndDisabled();
            }

            if (!steamIdLooksValid && targetSteamIDBuf[0] != '\0') {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("Некорректный SteamID64 (например, 76561198000000000)");
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Виртуальная подсеть: %s0/24 (Хост: .%d)",
                VpnConfig::kSubnetPrefix, VpnConfig::kHostOctet);

            if (!relayNetworkReady) ImGui::EndDisabled();
            if (!steamInitialized) ImGui::EndDisabled();
        }
        else if (state == TunnelState::Connecting) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Подключение к %llu...", tunnel.GetTargetSteamID());
            ImGui::Text("Поиск P2P-маршрута и ожидание ответа от Хоста...");
            ImGui::Spacing();

            if (ImGui::Button("Отмена", ImVec2(150, 30))) {
                tunnel.Shutdown();
            }
        }
        else if (state == TunnelState::Connected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Статус: %s", tunnel.IsHost() ? "Хостинг сети" : "Подключен к хосту");

            if (ImGui::Button("Отключиться", ImVec2(150, 30))) {
                tunnel.Shutdown();
            }

            ImGui::Separator();
            ImGui::Text("Подключенные Пиры:");

            // GetPeers() теперь возвращает снимок (копию) списка под мьютексом,
            // так что безопасно итерироваться, пока сетевой поток параллельно
            // может добавлять/удалять пиров.
            auto peers = tunnel.GetPeers();

            if (ImGui::BeginTable("PeersTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("SteamID");
                ImGui::TableSetupColumn("Virtual IP");
                ImGui::TableSetupColumn("Ping (RTT ms)");
                ImGui::TableHeadersRow();

                for (const auto& peer : peers) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%llu", peer.steamID);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", peer.virtualIP.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d ms", peer.pingMs);
                }
                ImGui::EndTable();
            }
        }

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    tunnel.Shutdown();

    if (steamInitialized) {
        SteamAPI_Shutdown();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    WSACleanup();

    LogLine("========== Завершение работы ==========");
    g_logFile.close();
    return 0;
}
