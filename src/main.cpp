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

    if (steamInitialized && SteamNetworkingUtils()) {
        SteamNetworkingUtils()->SetGlobalConfigValuePtr(
            k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            (void*)SteamVpnTunnel::OnSteamNetConnectionStatusChanged
        );
    } else {
        std::cerr << "[SteamAPI] Ошибка инициализации: " << errMsg << "\n";
    }

    SteamVpnTunnel tunnel;
    char targetSteamIDBuf[64] = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Сетевой цикл (Wintun <-> Steam) теперь крутится в фоновом потоке
        // внутри SteamVpnTunnel и не зависит от FPS окна — tunnel.Tick()
        // оставлен как no-op вызов только для наглядности жизненного цикла.
        // SteamAPI_RunCallbacks() тоже вызывается из фонового потока.
        tunnel.Tick();

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

            // Блокируем кнопки, если Steam API выключен
            if (!steamInitialized) ImGui::BeginDisabled();

            if (ImGui::Button("Запустить сеть (Хост)", ImVec2(340, 30))) {
                tunnel.InitHost();
            }

            ImGui::Spacing();
            ImGui::InputText("Target SteamID", targetSteamIDBuf, sizeof(targetSteamIDBuf));
            if (ImGui::Button("Подключиться к Хосту (Клиент)", ImVec2(340, 30))) {
                uint64_t targetID = std::strtoull(targetSteamIDBuf, nullptr, 10);
                if (targetID == 0) {
                    // Раньше некорректный/пустой ввод просто уходил в InitClient
                    // и там отбраковывался асинхронно; проверяем сразу в UI,
                    // чтобы не плодить лишний цикл Init/Shutdown.
                } else {
                    tunnel.InitClient(targetID);
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Виртуальная подсеть: %s0/24 (Хост: .%d)",
                VpnConfig::kSubnetPrefix, VpnConfig::kHostOctet);

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
    return 0;
}
