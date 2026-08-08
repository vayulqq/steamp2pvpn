#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace Logger {

    enum class Level {
        Debug,
        Info,
        Warn,
        Error
    };

    namespace detail {

        inline std::mutex& GetMutex() {
            static std::mutex m;
            return m;
        }

        inline const char* LevelToTag(Level level) {
            switch (level) {
                case Level::Debug: return "DEBUG";
                case Level::Info:  return "INFO ";
                case Level::Warn:  return "WARN ";
                case Level::Error: return "ERROR";
                default:           return "?????";
            }
        }

        inline std::string TimestampNow() {
            using namespace std::chrono;
            auto now = system_clock::now();
            auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
            std::time_t t = system_clock::to_time_t(now);
            std::tm tmBuf;
            localtime_s(&tmBuf, &t);
            std::ostringstream oss;
            oss << std::put_time(&tmBuf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
            return oss.str();
        }

        inline bool IsConsoleAttached() {
            static const bool result = [] {
                HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
                if (h == nullptr || h == INVALID_HANDLE_VALUE) {
                    return false;
                }
                return GetFileType(h) == FILE_TYPE_CHAR;
            }();
            return result;
        }

    }

    inline void Log(Level level, const std::string& tag, const std::string& message) {
        if (!detail::IsConsoleAttached()) {
            return;
        }
        std::lock_guard<std::mutex> lock(detail::GetMutex());
        std::cerr << "[" << detail::TimestampNow() << "] "
                   << "[" << detail::LevelToTag(level) << "] "
                   << "[" << tag << "] "
                   << message << "\n";
    }

    inline void Debug(const std::string& tag, const std::string& message) { Log(Level::Debug, tag, message); }
    inline void Info(const std::string& tag, const std::string& message)  { Log(Level::Info,  tag, message); }
    inline void Warn(const std::string& tag, const std::string& message)  { Log(Level::Warn,  tag, message); }
    inline void Error(const std::string& tag, const std::string& message) { Log(Level::Error, tag, message); }

}

#ifndef LOG_TAG
#define LOG_TAG "App"
#endif

#define LOG_DEBUG(msg) ::Logger::Debug(LOG_TAG, (msg))
#define LOG_INFO(msg)  ::Logger::Info(LOG_TAG, (msg))
#define LOG_WARN(msg)  ::Logger::Warn(LOG_TAG, (msg))
#define LOG_ERROR(msg) ::Logger::Error(LOG_TAG, (msg))
