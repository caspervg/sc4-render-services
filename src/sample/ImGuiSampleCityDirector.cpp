#include "cIGZFrameWork.h"
#include "cIGZImGuiService.h"
#include "cISC4App.h"
#include "cRZCOMDllDirector.h"
#include "GZServPtrs.h"
#include "imgui.h"
#include "ImGuiServiceIds.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kImGuiSampleCityDirectorID = 0xA9B1D3C2;
    constexpr uint32_t kSampleCityPanelId = 0x5C4E2A11;

    struct SampleCityState
    {
        int frameCounter = 0;
    };

    bool IsCityView()
    {
        cISC4AppPtr app;
        if (!app) {
            return false;
        }
        return app->GetCity() != nullptr;
    }

    void RenderSampleCityPanel(void* userData)
    {
        if (!IsCityView()) {
            return;
        }

        auto* state = static_cast<SampleCityState*>(userData);
        if (!state) {
            return;
        }

        static auto loggedFirstRender = false;
        if (!loggedFirstRender) {
            LOG_INFO("ImGuiSampleCity: rendering city-only panel");
            loggedFirstRender = true;
        }

        state->frameCounter++;

        ImGui::Begin("ImGui City Panel", nullptr, 0);
        ImGui::TextUnformatted("City view detected.");
        ImGui::Text("Frames in city view: %d", state->frameCounter);
        ImGui::End();
    }

    void ShutdownSampleCity(void* userData)
    {
        delete static_cast<SampleCityState*>(userData);
    }
}

class ImGuiSampleCityDirector final : public cRZCOMDllDirector
{
public:
    ImGuiSampleCityDirector()
        : service(nullptr),
          panelRegistered(false) {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kImGuiSampleCityDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4ImGuiSampleCity", "");
        LOG_INFO("ImGuiSampleCity: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("ImGuiSampleCity: framework hook added");
        }
        else {
            LOG_WARN("ImGuiSampleCity: mpFrameWork not available on start");
        }
        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("ImGuiSampleCity: PostAppInit");
        if (!mpFrameWork || panelRegistered) {
            if (!mpFrameWork) {
                LOG_WARN("ImGuiSampleCity: mpFrameWork not available in PostAppInit");
            }
            if (panelRegistered) {
                LOG_WARN("ImGuiSampleCity: panel already registered");
            }
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&service))) {
            LOG_WARN("ImGuiSampleCity: ImGui service not available");
            return true;
        }

        LOG_INFO("ImGuiSampleCity: obtained ImGui service (api={})", service->GetApiVersion());
        if (!service->GetContext()) {
            LOG_WARN("ImGuiSampleCity: ImGui context not ready yet");
        }

        auto* state = new SampleCityState();
        ImGuiPanelDesc desc{};
        desc.panel_id = kSampleCityPanelId;
        desc.order = 110;
        desc.visible = true;
        desc.render = &RenderSampleCityPanel;
        desc.on_shutdown = &ShutdownSampleCity;
        desc.data = state;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("ImGuiSampleCity: failed to register panel");
            ShutdownSampleCity(state);
            service->Release();
            service = nullptr;
            return true;
        }

        LOG_INFO("ImGuiSampleCity: registered panel {}", kSampleCityPanelId);
        panelRegistered = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (service) {
            service->UnregisterPanel(kSampleCityPanelId);
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

static ImGuiSampleCityDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
