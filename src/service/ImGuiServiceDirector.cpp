#include "ImGuiService.h"

#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kImGuiServiceDirectorID = 0xC17F4B21;
}

class ImGuiServiceDirector final : public cRZCOMDllDirector
{
public:
    ImGuiServiceDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kImGuiServiceDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize("SC4ImGuiService", "");
        LOG_INFO("ImGuiServiceDirector: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("ImGuiServiceDirector: framework hook added");
            mpFrameWork->AddSystemService(&service);
            mpFrameWork->AddToTick(&service);
            service.Init();
            LOG_INFO("ImGuiServiceDirector: service registered and tick enabled");
        }
        else {
            LOG_WARN("ImGuiServiceDirector: framework not available");
        }
        return true;
    }

    bool PostAppShutdown() override {
        service.Shutdown();
        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
            mpFrameWork->RemoveFromTick(&service);
            mpFrameWork->RemoveSystemService(&service);
            LOG_INFO("ImGuiServiceDirector: service unregistered");
        }
        return true;
    }

private:
    ImGuiService service;
};

static ImGuiServiceDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
