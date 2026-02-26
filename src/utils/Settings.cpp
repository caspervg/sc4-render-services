#include "Settings.h"
#include "Logger.h"

#include "mini/ini.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>
#include <string>

namespace {
    constexpr spdlog::level::level_enum kDefaultLogLevel = spdlog::level::info;
    constexpr bool kDefaultLogToFile = true;
    constexpr float kDefaultFontSize = 13.0f;
    constexpr float kMinFontSize = 8.0f;
    constexpr float kMaxFontSize = 32.0f;
    constexpr int kDefaultFontOversample = 2;
    constexpr int kMinFontOversample = 1;
    constexpr int kMaxFontOversample = 3;
    constexpr bool kDefaultKeyboardNav = true;
    constexpr float kDefaultUIScale = 1.0f;
    constexpr float kMinUIScale = 0.25f;
    constexpr float kMaxUIScale = 4.0f;
    constexpr bool kDefaultEnableImGuiService = true;
    constexpr bool kDefaultEnableS3DCameraService = true;
    constexpr bool kDefaultEnableDrawService = true;

    const std::string kDefaultTheme = "dark";
    const std::string kSectionName = "SC4RenderServices";

    std::string ToLower(const std::string& value) {
        std::string normalized(value);
        std::ranges::transform(normalized, normalized.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return normalized;
    }

    spdlog::level::level_enum ParseLogLevel(const std::string& value, bool& valid) {
        const std::string normalized = ToLower(value);

        if (normalized == "trace") { valid = true; return spdlog::level::trace; }
        if (normalized == "debug") { valid = true; return spdlog::level::debug; }
        if (normalized == "info") { valid = true; return spdlog::level::info; }
        if (normalized == "warn" || normalized == "warning") { valid = true; return spdlog::level::warn; }
        if (normalized == "error") { valid = true; return spdlog::level::err; }
        if (normalized == "critical") { valid = true; return spdlog::level::critical; }
        if (normalized == "off") { valid = true; return spdlog::level::off; }

        valid = false;
        return kDefaultLogLevel;
    }

    bool ParseBool(const std::string& value, bool& valid) {
        const std::string normalized = ToLower(value);

        if (normalized == "true" || normalized == "1" || normalized == "yes") {
            valid = true;
            return true;
        }
        if (normalized == "false" || normalized == "0" || normalized == "no") {
            valid = true;
            return false;
        }

        valid = false;
        return false;
    }

    float ParseFloat(const std::string& value, bool& valid) {
        float result = 0.0f;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);

        if (ec != std::errc() || ptr != value.data() + value.size()) {
            valid = false;
            return 0.0f;
        }

        if (std::isnan(result) || std::isinf(result)) {
            valid = false;
            return 0.0f;
        }

        valid = true;
        return result;
    }

    int ParseInt(const std::string& value, bool& valid) {
        int result = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);

        if (ec != std::errc() || ptr != value.data() + value.size()) {
            valid = false;
            return 0;
        }

        valid = true;
        return result;
    }
} // namespace

Settings::Settings()
    : logLevel_(kDefaultLogLevel)
    , logToFile_(kDefaultLogToFile)
    , fontSize_(kDefaultFontSize)
    , fontOversample_(kDefaultFontOversample)
    , theme_(kDefaultTheme)
    , keyboardNav_(kDefaultKeyboardNav)
    , uiScale_(kDefaultUIScale)
    , enableImGuiService_(kDefaultEnableImGuiService)
    , enableS3DCameraService_(kDefaultEnableS3DCameraService)
    , enableDrawService_(kDefaultEnableDrawService) {}

void Settings::Load(const std::filesystem::path& settingsFilePath) {
    // Reset to defaults
    *this = Settings();

    try {
        const mINI::INIFile file(settingsFilePath.string());
        mINI::INIStructure ini;

        if (!file.read(ini)) {
            LOG_INFO("Using default settings, no configuration file detected: {}", settingsFilePath.string());
            return;
        }

        if (!ini.has(kSectionName)) {
            LOG_INFO("Using default settings, section [{}] missing in {}", kSectionName, settingsFilePath.string());
            return;
        }
        auto section = ini.get(kSectionName);

        // LogLevel
        if (section.has("LogLevel")) {
            bool valid = false;
            const std::string text = section.get("LogLevel");
            logLevel_ = ParseLogLevel(text, valid);
            if (!valid) {
                logLevel_ = kDefaultLogLevel;
                LOG_ERROR("Invalid LogLevel value '{}' in {}. Using default info.", text, settingsFilePath.string());
            }
        }

        // LogToFile
        if (section.has("LogToFile")) {
            bool valid = false;
            const std::string text = section.get("LogToFile");
            logToFile_ = ParseBool(text, valid);
            if (!valid) {
                logToFile_ = kDefaultLogToFile;
                LOG_ERROR("Invalid LogToFile value '{}' in {}. Using default true.", text, settingsFilePath.string());
            }
        }

        // FontSize
        if (section.has("FontSize")) {
            bool valid = false;
            const std::string text = section.get("FontSize");
            float parsed = ParseFloat(text, valid);
            if (!valid) {
                LOG_ERROR("Invalid FontSize value '{}' in {}. Using default {}.", text, settingsFilePath.string(), kDefaultFontSize);
            } else if (parsed > kMaxFontSize) {
                LOG_WARN("FontSize value {} exceeds {} and has been capped.", parsed, kMaxFontSize);
                fontSize_ = kMaxFontSize;
            } else if (parsed < kMinFontSize) {
                LOG_WARN("FontSize value {} is below {} and has been raised.", parsed, kMinFontSize);
                fontSize_ = kMinFontSize;
            } else {
                fontSize_ = parsed;
            }
        }

        // FontFile
        if (section.has("FontFile")) {
            fontFile_ = section.get("FontFile");
        }

        // FontOversample
        if (section.has("FontOversample")) {
            bool valid = false;
            const std::string text = section.get("FontOversample");
            int parsed = ParseInt(text, valid);
            if (!valid) {
                LOG_ERROR("Invalid FontOversample value '{}' in {}. Using default {}.", text, settingsFilePath.string(), kDefaultFontOversample);
            } else if (parsed > kMaxFontOversample) {
                LOG_WARN("FontOversample value {} exceeds {} and has been capped.", parsed, kMaxFontOversample);
                fontOversample_ = kMaxFontOversample;
            } else if (parsed < kMinFontOversample) {
                LOG_WARN("FontOversample value {} is below {} and has been raised.", parsed, kMinFontOversample);
                fontOversample_ = kMinFontOversample;
            } else {
                fontOversample_ = parsed;
            }
        }

        // Theme
        if (section.has("Theme")) {
            const std::string text = ToLower(section.get("Theme"));
            if (text == "dark" || text == "light" || text == "classic") {
                theme_ = text;
            } else {
                LOG_ERROR("Invalid Theme value '{}' in {}. Using default dark.", section.get("Theme"), settingsFilePath.string());
            }
        }

        // KeyboardNav
        if (section.has("KeyboardNav")) {
            bool valid = false;
            const std::string text = section.get("KeyboardNav");
            keyboardNav_ = ParseBool(text, valid);
            if (!valid) {
                keyboardNav_ = kDefaultKeyboardNav;
                LOG_ERROR("Invalid KeyboardNav value '{}' in {}. Using default true.", text, settingsFilePath.string());
            }
        }

        // UIScale
        if (section.has("UIScale")) {
            bool valid = false;
            const std::string text = section.get("UIScale");
            float parsed = ParseFloat(text, valid);
            if (!valid) {
                LOG_ERROR("Invalid UIScale value '{}' in {}. Using default {}.", text, settingsFilePath.string(), kDefaultUIScale);
            } else if (parsed > kMaxUIScale) {
                LOG_WARN("UIScale value {} exceeds {} and has been capped.", parsed, kMaxUIScale);
                uiScale_ = kMaxUIScale;
            } else if (parsed < kMinUIScale) {
                LOG_WARN("UIScale value {} is below {} and has been raised.", parsed, kMinUIScale);
                uiScale_ = kMinUIScale;
            } else {
                uiScale_ = parsed;
            }
        }

        // EnableImGuiService
        if (section.has("EnableImGuiService")) {
            bool valid = false;
            const std::string text = section.get("EnableImGuiService");
            enableImGuiService_ = ParseBool(text, valid);
            if (!valid) {
                enableImGuiService_ = kDefaultEnableImGuiService;
                LOG_ERROR("Invalid EnableImGuiService value '{}' in {}. Using default true.", text, settingsFilePath.string());
            }
        }

        // EnableS3DCameraService
        if (section.has("EnableS3DCameraService")) {
            bool valid = false;
            const std::string text = section.get("EnableS3DCameraService");
            enableS3DCameraService_ = ParseBool(text, valid);
            if (!valid) {
                enableS3DCameraService_ = kDefaultEnableS3DCameraService;
                LOG_ERROR("Invalid EnableS3DCameraService value '{}' in {}. Using default true.", text, settingsFilePath.string());
            }
        }

        // EnableDrawService
        if (section.has("EnableDrawService")) {
            bool valid = false;
            const std::string text = section.get("EnableDrawService");
            enableDrawService_ = ParseBool(text, valid);
            if (!valid) {
                enableDrawService_ = kDefaultEnableDrawService;
                LOG_ERROR("Invalid EnableDrawService value '{}' in {}. Using default true.", text, settingsFilePath.string());
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Error reading settings file {}: {}", settingsFilePath.string(), e.what());
        *this = Settings();
    }
}

spdlog::level::level_enum Settings::GetLogLevel() const noexcept { return logLevel_; }
bool Settings::GetLogToFile() const noexcept { return logToFile_; }
float Settings::GetFontSize() const noexcept { return fontSize_; }
std::string Settings::GetFontFile() const noexcept { return fontFile_; }
int Settings::GetFontOversample() const noexcept { return fontOversample_; }
std::string Settings::GetTheme() const noexcept { return theme_; }
bool Settings::GetKeyboardNav() const noexcept { return keyboardNav_; }
float Settings::GetUIScale() const noexcept { return uiScale_; }
bool Settings::GetEnableImGuiService() const noexcept { return enableImGuiService_; }
bool Settings::GetEnableS3DCameraService() const noexcept { return enableS3DCameraService_; }
bool Settings::GetEnableDrawService() const noexcept { return enableDrawService_; }
