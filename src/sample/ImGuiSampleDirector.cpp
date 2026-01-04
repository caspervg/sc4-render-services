#include "cRZCOMDllDirector.h"

#include "cIGZFrameWork.h"
#include "cIGZImGuiService.h"
#include "imgui.h"
#include "ImGuiServiceIds.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kImGuiSampleDirectorID = 0xD4C89190;
    constexpr uint32_t kSamplePanelId = 0x1B5A2F01;

    struct SampleState
    {
        bool showDetails = true;
        int clickCount = 0;
    };

    void RenderSamplePanel(void* userData)
    {
        auto* state = static_cast<SampleState*>(userData);
        if (!state) {
            return;
        }

        static auto loggedFirstRender = false;
        if (!loggedFirstRender) {
            LOG_INFO("ImGuiSample: rendering sample panel");
            loggedFirstRender = true;
        }

        ImGui::Begin("ImGui Service Sample", nullptr, 0);
        ImGui::TextUnformatted("Hello from the ImGui service sample DLL.");
        ImGui::Checkbox("Show details", &state->showDetails);
        if (state->showDetails) {
            ImGui::Separator();
            ImGui::TextUnformatted("This window is rendered via the shared ImGui service.");
        }
        if (ImGui::Button("Click", ImVec2(0.0f, 0.0f))) {
            state->clickCount++;
        }
        ImGui::SameLine(0.0f, -1.0f);
        char buffer[64]{};
        snprintf(buffer, sizeof(buffer), "Clicks: %d", state->clickCount);
        ImGui::TextUnformatted(buffer);
        ImGui::End();
    }

    void ShutdownSample(void* userData)
    {
        delete static_cast<SampleState*>(userData);
    }
}

class ImGuiSampleDirector final : public cRZCOMDllDirector
{
public:
    ImGuiSampleDirector()
        : service(nullptr),
          panelRegistered(false) {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kImGuiSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4ImGuiSample", "");
        LOG_INFO("ImGuiSample: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("ImGuiSample: framework hook added");
        }
        else {
            LOG_WARN("ImGuiSample: mpFrameWork not available on start");
        }
        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("ImGuiSample: PostAppInit");
        if (!mpFrameWork || panelRegistered) {
            if (!mpFrameWork) {
                LOG_WARN("ImGuiSample: mpFrameWork not available in PostAppInit");
            }
            if (panelRegistered) {
                LOG_WARN("ImGuiSample: panel already registered");
            }
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&service))) {
            LOG_WARN("ImGuiSample: ImGui service not available");
            return true;
        }

        LOG_INFO("ImGuiSample: obtained ImGui service (api={})", service->GetApiVersion());
        if (!service->GetContext()) {
            LOG_WARN("ImGuiSample: ImGui context not ready yet");
        }

        auto* state = new SampleState();
        ImGuiPanelDesc desc{};
        desc.panel_id = kSamplePanelId;
        desc.order = 100;
        desc.visible = true;
        desc.render = &RenderSamplePanel;
        desc.on_shutdown = &ShutdownSample;
        desc.data = state;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("ImGuiSample: failed to register panel");
            ShutdownSample(state);
            service->Release();
            service = nullptr;
            return true;
        }

        LOG_INFO("ImGuiSample: registered panel {}", kSamplePanelId);
        panelRegistered = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (service) {
            service->UnregisterPanel(kSamplePanelId);
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

static ImGuiSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
