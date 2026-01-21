#include "cRZCOMDllDirector.h"

#include "cIGZFrameWork.h"
#include "imgui.h"
#include "public/cIGZImGuiService.h"
#include "public/ImGuiServiceIds.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kImGuiSampleDemoDirectorID = 0xB4F5A17D;
    constexpr uint32_t kSampleDemoPanelId = 0x6D2EBA44;

    struct SampleDemoState
    {
        bool showDemo = true;
    };

    void RenderDemoPanel(void* userData)
    {
        auto* state = static_cast<SampleDemoState*>(userData);
        if (!state || !state->showDemo) {
            return;
        }

        static auto loggedFirstRender = false;
        if (!loggedFirstRender) {
            LOG_INFO("ImGuiSampleDemo: rendering demo window");
            loggedFirstRender = true;
        }

        ImGui::ShowDemoWindow(&state->showDemo);
    }

    void ShutdownSampleDemo(void* userData)
    {
        delete static_cast<SampleDemoState*>(userData);
    }
}

class ImGuiSampleDemoDirector final : public cRZCOMDllDirector
{
public:
    ImGuiSampleDemoDirector()
        : service(nullptr),
          panelRegistered(false) {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kImGuiSampleDemoDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4ImGuiSampleDemo", "");
        LOG_INFO("ImGuiSampleDemo: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("ImGuiSampleDemo: framework hook added");
        }
        else {
            LOG_WARN("ImGuiSampleDemo: mpFrameWork not available on start");
        }
        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("ImGuiSampleDemo: PostAppInit");
        if (!mpFrameWork || panelRegistered) {
            if (!mpFrameWork) {
                LOG_WARN("ImGuiSampleDemo: mpFrameWork not available in PostAppInit");
            }
            if (panelRegistered) {
                LOG_WARN("ImGuiSampleDemo: panel already registered");
            }
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&service))) {
            LOG_WARN("ImGuiSampleDemo: ImGui service not available");
            return true;
        }

        LOG_INFO("ImGuiSampleDemo: obtained ImGui service (api={})", service->GetApiVersion());
        if (!service->GetContext()) {
            LOG_WARN("ImGuiSampleDemo: ImGui context not ready yet");
        }

        auto* state = new SampleDemoState();
        ImGuiPanelDesc desc{};
        desc.id = kSampleDemoPanelId;
        desc.order = 120;
        desc.visible = true;
        desc.on_render = &RenderDemoPanel;
        desc.on_shutdown = &ShutdownSampleDemo;
        desc.data = state;
        desc.fontId = 0;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("ImGuiSampleDemo: failed to register panel");
            ShutdownSampleDemo(state);
            service->Release();
            service = nullptr;
            return true;
        }

        LOG_INFO("ImGuiSampleDemo: registered panel {}", kSampleDemoPanelId);
        panelRegistered = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (service) {
            service->UnregisterPanel(kSampleDemoPanelId);
            service->Release();
            service = nullptr;
        }
        panelRegistered = false;
        return true;
    }

private:
    cIGZImGuiService* service;
    bool panelRegistered;
};

static ImGuiSampleDemoDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
