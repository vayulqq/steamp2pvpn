// relay_test.cpp
// -----------------------------------------------------------------------------
// Минимальный консольный тест: инициализирует Steam API с AppID 480 (Spacewar,
// тестовый AppID Valve), вызывает InitRelayNetworkAccess() и опрашивает
// GetRelayNetworkStatus() раз в секунду, пока статус не станет Current (или
// пока не нажмут Ctrl+C). Никакого GUI, никакого туннеля — только диагностика
// сети релеев (SDR).
//
// Сборка (Windows, MSVC, из папки со steam_api64.dll/.lib из SDK):
//   cl /EHsc /I sdk\public relay_test.cpp /link sdk\redistributable_bin\win64\steam_api64.lib
//
// Перед запуском рядом с exe должен лежать steam_api64.dll (из sdk\redistributable_bin\win64)
// — программа сама создаст steam_appid.txt с "480", если его нет.
// -----------------------------------------------------------------------------

#include <steam/steam_api.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

static std::string TimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

static void Log(const std::string& message) {
    printf("[%s] %s\n", TimestampNow().c_str(), message.c_str());
    fflush(stdout);
}

// Человекочитаемое имя статуса вместо голого числа
static const char* AvailabilityToString(ESteamNetworkingAvailability status) {
    switch (status) {
        case k_ESteamNetworkingAvailability_CannotTry: return "CannotTry";
        case k_ESteamNetworkingAvailability_Failed:     return "Failed";
        case k_ESteamNetworkingAvailability_Previously: return "Previously";
        case k_ESteamNetworkingAvailability_Retrying:   return "Retrying";
        case k_ESteamNetworkingAvailability_NeverTried: return "NeverTried";
        case k_ESteamNetworkingAvailability_Waiting:    return "Waiting";
        case k_ESteamNetworkingAvailability_Attempting: return "Attempting";
        case k_ESteamNetworkingAvailability_Current:    return "Current";
        case k_ESteamNetworkingAvailability_Unknown:    return "Unknown";
        default: return "???";
    }
}

int main() {
    // Гарантируем AppID 480 (Spacewar), если рядом ещё нет steam_appid.txt
    {
        std::ifstream check("steam_appid.txt");
        if (!check.good()) {
            std::ofstream out("steam_appid.txt");
            out << "480";
            Log("[Setup] steam_appid.txt не найден, создан с AppID 480 (Spacewar).");
        }
    }

    Log("[SteamAPI] Вызов SteamAPI_InitEx()...");
    SteamErrMsg errMsg = {0};
    if (SteamAPI_InitEx(&errMsg) != k_ESteamAPIInitResult_OK) {
        Log(std::string("[SteamAPI] ОШИБКА инициализации: ") + errMsg);
        Log("[SteamAPI] Убедитесь, что клиент Steam запущен и вы вошли в аккаунт.");
        return 1;
    }
    Log("[SteamAPI] SteamAPI_InitEx: успех.");

    Log("[Relay] Вызов SteamNetworkingUtils()->InitRelayNetworkAccess()...");
    SteamNetworkingUtils()->InitRelayNetworkAccess();

    ESteamNetworkingAvailability lastStatus = k_ESteamNetworkingAvailability_Unknown;
    bool loggedOnce = false;
    auto startTime = std::chrono::steady_clock::now();

    // Опрашиваем статус раз в секунду. SteamAPI_RunCallbacks() обязателен —
    // без него статус никогда не обновится, т.к. Steam доставляет результат
    // через callback-очередь, которую нужно вычитывать вручную.
    while (true) {
        SteamAPI_RunCallbacks();

        SteamRelayNetworkStatus_t details;
        ESteamNetworkingAvailability status = SteamNetworkingUtils()->GetRelayNetworkStatus(&details);

        if (!loggedOnce || status != lastStatus) {
            std::ostringstream oss;
            oss << "[Relay] Статус: " << AvailabilityToString(status);
            if (details.m_debugMsg[0] != '\0') {
                oss << " | debug: " << details.m_debugMsg;
            }
            Log(oss.str());
            lastStatus = status;
            loggedOnce = true;
        }

        if (status == k_ESteamNetworkingAvailability_Current) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            Log("[Relay] Готово! Заняло " + std::to_string(elapsed) + " мс.");
            break;
        }

        if (status == k_ESteamNetworkingAvailability_Failed ||
            status == k_ESteamNetworkingAvailability_CannotTry) {
            Log("[Relay] Инициализация провалилась окончательно, дальше ждать нет смысла.");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    Log("[SteamAPI] Завершение (SteamAPI_Shutdown)...");
    SteamAPI_Shutdown();

#if defined(_WIN32)
    printf("\nНажмите Enter для выхода...");
    fflush(stdout);
    getchar();
#endif
    return 0;
}
