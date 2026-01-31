#include "ImGuiService.h"
#include "S3DCameraService.h"

#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kCustomServicesDirectorID = 0xC17F4B21; // reuse previous ImGui director ID to avoid duplicates
}

class CustomServicesDirector final : public cRZCOMDllDirector {
public:
    CustomServicesDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kCustomServicesDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize("SC4CustomServices", "");
        LOG_INFO("CustomServicesDirector: OnStart");

        if (!mpFrameWork) {
            LOG_WARN("CustomServicesDirector: framework not available");
            return true;
        }

        // Register ImGui service (641-gated inside Init)
        if (imguiService_.Init()) {
            mpFrameWork->AddHook(this);
            mpFrameWork->AddSystemService(&imguiService_);
            mpFrameWork->AddToTick(&imguiService_);
            LOG_INFO("CustomServicesDirector: ImGuiService registered");
        } else {
            LOG_WARN("CustomServicesDirector: ImGuiService not registered (version check failed)");
        }

        // Register camera service (641-gated inside Init)
        if (cameraService_.Init()) {
            mpFrameWork->AddSystemService(&cameraService_);
            LOG_INFO("CustomServicesDirector: S3DCameraService registered");
        } else {
            LOG_WARN("CustomServicesDirector: S3DCameraService not registered (version check failed)");
        }

        return true;
    }

    bool PostAppShutdown() override {
        LOG_INFO("CustomServicesDirector: PostAppShutdown");

        if (mpFrameWork) {
            mpFrameWork->RemoveFromTick(&imguiService_);
            mpFrameWork->RemoveSystemService(&imguiService_);
            mpFrameWork->RemoveSystemService(&cameraService_);
            mpFrameWork->RemoveHook(this);
        }

        imguiService_.Shutdown();
        cameraService_.Shutdown();
        return true;
    }

private:
    ImGuiService imguiService_;
    S3DCameraService cameraService_;
};

static CustomServicesDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}

