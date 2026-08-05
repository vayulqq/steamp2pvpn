#include <winsock2.h>
#include <windows.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "steam_tunnel.h"
#include <iostream>
#include <fstream>

void EnsureSteamAppId() {
    std::ifstream check("steam_appid.txt");
    if (!check.good()) {
        std::ofstream out("steam_appid.txt");
        out << "480"; // Spacewar AppID для тестов
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

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    SteamErrMsg errMsg;
    if (SteamAPI_InitEx(&errMsg) != k_ESteamAPIInitResult_OK) {
        std::cerr << "[SteamAPI] Ошибка инициализации: " << errMsg << "\n";
    }

    SteamNetworkingUtils()->SetGlobalConfigValuePtr(
        k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        (void*)SteamVpnTunnel::OnSteamNetConnectionStatusChanged
    );

    SteamVpnTunnel tunnel;
    char targetSteamIDBuf[64] = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Единый опрос кадра
        tunnel.Tick();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Steam P2P VPN Panel", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        if (SteamUser()) {
            CSteamID myID = SteamUser()->GetSteamID();
            const char* myName = SteamFriends() ? SteamFriends()->GetPersonaName() : "Unknown";
            ImGui::Text("Профиль Steam: %s (ID: %llu)", myName, myID.ConvertToUint64());
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Steam API не активен!");
        }

        ImGui::Separator();

        if (!tunnel.IsActive()) {
            if (ImGui::Button("Запустить сеть (Хост - 192.168.137.1)", ImVec2(300, 30))) {
                tunnel.InitHost("192.168.137.1");
            }

            ImGui::Spacing();
            ImGui::InputText("Target SteamID", targetSteamIDBuf, sizeof(targetSteamIDBuf));
            if (ImGui::Button("Подключиться к Хосту (Клиент - 192.168.137.2)", ImVec2(300, 30))) {
                uint64_t targetID = std::strtoull(targetSteamIDBuf, nullptr, 10);
                if (targetID != 0) {
                    tunnel.InitClient(targetID, "192.168.137.2");
                }
            }
        } else {
            ImGui::Text("Статус: %s", tunnel.IsHost() ? "Хостинг сети" : "Подключен к хосту");
            if (ImGui::Button("Отключиться", ImVec2(150, 30))) {
                tunnel.Shutdown();
            }

            ImGui::Separator();
            ImGui::Text("Подключенные Пиры:");

            if (ImGui::BeginTable("PeersTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("SteamID");
                ImGui::TableSetupColumn("Virtual IP");
                ImGui::TableSetupColumn("Ping (RTT ms)");
                ImGui::TableHeadersRow();

                for (const auto& peer : tunnel.GetPeers()) {
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
        glClear();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    tunnel.Shutdown();
    SteamAPI_Shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    WSACleanup();
    return 0;
}
