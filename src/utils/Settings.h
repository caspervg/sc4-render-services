#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <spdlog/common.h>

class Settings {
public:
    Settings();

    void Load(const std::filesystem::path& settingsFilePath);

    // Logging
    [[nodiscard]] spdlog::level::level_enum GetLogLevel() const noexcept;
    [[nodiscard]] bool GetLogToFile() const noexcept;

    // ImGui appearance
    [[nodiscard]] float GetFontSize() const noexcept;
    [[nodiscard]] std::string GetFontFile() const noexcept;
    [[nodiscard]] int GetFontOversample() const noexcept;
    [[nodiscard]] std::string GetTheme() const noexcept;
    [[nodiscard]] bool GetKeyboardNav() const noexcept;
    [[nodiscard]] float GetUIScale() const noexcept;
    [[nodiscard]] bool GetShowDemoPanel() const noexcept;

    // Service toggles
    [[nodiscard]] bool GetEnableImGuiService() const noexcept;
    [[nodiscard]] bool GetEnableS3DCameraService() const noexcept;
    [[nodiscard]] bool GetEnableDrawService() const noexcept;

private:
    spdlog::level::level_enum logLevel_;
    bool logToFile_;
    float fontSize_;
    std::string fontFile_;
    int fontOversample_;
    std::string theme_;
    bool keyboardNav_;
    float uiScale_;
    bool showDemoPanel_;
    bool enableImGuiService_;
    bool enableS3DCameraService_;
    bool enableDrawService_;
};
