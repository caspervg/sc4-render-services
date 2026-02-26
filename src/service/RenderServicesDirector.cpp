#include "DrawService.h"
#include "ImGuiService.h"
#include "S3DCameraService.h"

#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "utils/Logger.h"
#include "utils/Settings.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

#include <Windows.h>

namespace {
    constexpr auto kRenderServicesDirectorID = 0xC17F4B21;
    constexpr std::string_view kSettingsFileName = "SC4RenderServices.ini";

    std::wstring GetModulePath(HMODULE moduleHandle) {
        std::vector<wchar_t> pathBuffer(MAX_PATH);

        while (true) {
            DWORD copiedLength =
                GetModuleFileNameW(moduleHandle, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
            if (copiedLength == 0) {
                throw std::runtime_error("GetModuleFileNameW failed.");
            }
            if (copiedLength < pathBuffer.size() - 1) {
                return std::wstring(pathBuffer.data(), copiedLength);
            }

            pathBuffer.resize(pathBuffer.size() * 2);
        }
    }

    std::filesystem::path GetDllFolderPath() {
        HMODULE moduleHandle = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetDllFolderPath),
                &moduleHandle)) {
            throw std::runtime_error("GetModuleHandleExW failed.");
        }
        const std::filesystem::path temp(GetModulePath(moduleHandle));
        return temp.parent_path();
    }
}

class RenderServicesDirector final : public cRZCOMDllDirector {
public:
    RenderServicesDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kRenderServicesDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        // Step 1: Get DLL folder path for settings and logging
        std::filesystem::path dllFolderPath;
        try {
            dllFolderPath = GetDllFolderPath();
        } catch (const std::exception& e) {
            Logger::Initialize("SC4RenderServices", "");
            LOG_ERROR("RenderServicesDirector: failed to get DLL folder path: {}", e.what());
        }

        // Step 2: Load settings from INI (before logger configuration so we know LogToFile)
        Settings settings;
        if (!dllFolderPath.empty()) {
            std::filesystem::path settingsFilePath = dllFolderPath / kSettingsFileName;
            // Initialize logger first with defaults so Settings::Load can log
            Logger::Initialize("SC4RenderServices", "", true);
            settings.Load(settingsFilePath);

            // Step 3: Apply settings to logger
            Logger::SetLevel(settings.GetLogLevel());
        } else {
            Logger::Initialize("SC4RenderServices", "");
        }

        LOG_INFO("RenderServicesDirector: OnStart");
        LOG_INFO("RenderServicesDirector: settings loaded (LogLevel={}, FontSize={}, Theme={}, UIScale={}, KeyboardNav={})",
                 spdlog::level::to_short_c_str(settings.GetLogLevel()),
                 settings.GetFontSize(),
                 settings.GetTheme(),
                 settings.GetUIScale(),
                 settings.GetKeyboardNav());

        if (!mpFrameWork) {
            LOG_WARN("RenderServicesDirector: framework not available");
            return true;
        }

        // Build ImGui init settings from INI
        ImGuiInitSettings imguiSettings;
        imguiSettings.fontSize = settings.GetFontSize();
        imguiSettings.fontOversample = settings.GetFontOversample();
        imguiSettings.theme = settings.GetTheme();
        imguiSettings.keyboardNav = settings.GetKeyboardNav();
        imguiSettings.uiScale = settings.GetUIScale();

        // Resolve font file path relative to DLL folder
        const std::string fontFile = settings.GetFontFile();
        if (!fontFile.empty() && !dllFolderPath.empty()) {
            imguiSettings.fontFile = (dllFolderPath / fontFile).string();
        }

        // Register ImGui service (641-gated inside Init)
        if (settings.GetEnableImGuiService()) {
            imguiService_.SetInitSettings(imguiSettings);
            if (imguiService_.Init()) {
                mpFrameWork->AddHook(this);
                mpFrameWork->AddSystemService(&imguiService_);
                mpFrameWork->AddToTick(&imguiService_);
                LOG_INFO("RenderServicesDirector: ImGuiService registered");
            } else {
                LOG_WARN("RenderServicesDirector: ImGuiService not registered (version check failed)");
            }
        } else {
            LOG_INFO("RenderServicesDirector: ImGuiService disabled by settings");
        }

        // Register camera service (641-gated inside Init)
        if (settings.GetEnableS3DCameraService()) {
            if (cameraService_.Init()) {
                mpFrameWork->AddSystemService(&cameraService_);
                LOG_INFO("RenderServicesDirector: S3DCameraService registered");
            } else {
                LOG_WARN("RenderServicesDirector: S3DCameraService not registered (version check failed)");
            }
        } else {
            LOG_INFO("RenderServicesDirector: S3DCameraService disabled by settings");
        }

        // Register draw service (641-gated inside Init)
        if (settings.GetEnableDrawService()) {
            if (drawService_.Init()) {
                mpFrameWork->AddSystemService(&drawService_);
                LOG_INFO("RenderServicesDirector: DrawService registered");
            } else {
                LOG_WARN("RenderServicesDirector: DrawService not registered (version check failed)");
            }
        } else {
            LOG_INFO("RenderServicesDirector: DrawService disabled by settings");
        }

        return true;
    }

    bool PostAppShutdown() override {
        LOG_INFO("RenderServicesDirector: PostAppShutdown");

        if (mpFrameWork) {
            mpFrameWork->RemoveFromTick(&imguiService_);
            mpFrameWork->RemoveSystemService(&imguiService_);
            mpFrameWork->RemoveSystemService(&cameraService_);
            mpFrameWork->RemoveSystemService(&drawService_);
            mpFrameWork->RemoveHook(this);
        }

        imguiService_.Shutdown();
        cameraService_.Shutdown();
        drawService_.Shutdown();
        return true;
    }

private:
    ImGuiService imguiService_;
    S3DCameraService cameraService_;
    DrawService drawService_;
};

static RenderServicesDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
