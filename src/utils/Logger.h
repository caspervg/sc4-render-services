#pragma once
#include <spdlog/spdlog.h>
#include <memory>
#include <string>

class Logger
{
public:
    // Get the singleton instance
    static std::shared_ptr<spdlog::logger> Get();

    // Initialize the logger (called once at startup)
    // If userDir is provided, logs will be written there; otherwise falls back to Documents\SimCity 4
    static void Initialize(const std::string& logName = "UnknownDllMod", const std::string& userDir = "");

    // Shutdown the logger (called at exit)
    static void Shutdown();

private:
    static std::shared_ptr<spdlog::logger> s_logger;
    static std::string s_logName;
    static bool s_initialized;

    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
};

// Convenience macros for logging
#define LOG_TRACE(...) Logger::Get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::Get()->debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::Get()->info(__VA_ARGS__)
#define LOG_WARN(...) Logger::Get()->warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::Get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) Logger::Get()->critical(__VA_ARGS__)