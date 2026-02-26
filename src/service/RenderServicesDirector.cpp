#include "DrawService.h"
#include "ImGuiService.h"
#include "S3DCameraService.h"

#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kRenderServicesDirectorID = 0xC17F4B21;
}

class RenderServicesDirector final : public cRZCOMDllDirector {
public:
    RenderServicesDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kRenderServicesDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize("SC4RenderServices", "");
        LOG_INFO("RenderServicesDirector: OnStart");

        if (!mpFrameWork) {
            LOG_WARN("RenderServicesDirector: framework not available");
            return true;
        }

        // Register ImGui service (641-gated inside Init)
        if (imguiService_.Init()) {
            mpFrameWork->AddHook(this);
            mpFrameWork->AddSystemService(&imguiService_);
            mpFrameWork->AddToTick(&imguiService_);
            LOG_INFO("RenderServicesDirector: ImGuiService registered");
        } else {
            LOG_WARN("RenderServicesDirector: ImGuiService not registered (version check failed)");
        }

        // Register camera service (641-gated inside Init)
        if (cameraService_.Init()) {
            mpFrameWork->AddSystemService(&cameraService_);
            LOG_INFO("RenderServicesDirector: S3DCameraService registered");
        } else {
            LOG_WARN("RenderServicesDirector: S3DCameraService not registered (version check failed)");
        }

        // Register draw service (641-gated inside Init)
        if (drawService_.Init()) {
            mpFrameWork->AddSystemService(&drawService_);
            LOG_INFO("RenderServicesDirector: DrawService registered");
        } else {
            LOG_WARN("RenderServicesDirector: DrawService not registered (version check failed)");
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

