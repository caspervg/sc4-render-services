#include "cRZCOMDllDirector.h"

#include "cIGZFrameWork.h"
#include "imgui.h"
#include "public/cIGZImGuiService.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "utils/Logger.h"

namespace {
    constexpr auto kImGuiSampleDirectorID = 0xD4C89190;
    constexpr uint32_t kSamplePanelId = 0x1B5A2F01;

    class SamplePanel final : public ImGuiPanel
    {
    public:
        void OnInit() override
        {
            LOG_INFO("ImGuiSample: panel initialized");
        }

        void OnRender() override
        {
            static auto loggedFirstRender = false;
            if (!loggedFirstRender) {
                LOG_INFO("ImGuiSample: rendering sample panel");
                loggedFirstRender = true;
            }

            ImGui::Begin("ImGui Service Sample", nullptr, 0);
            ImGui::TextUnformatted("Hello from the ImGui service sample DLL.");
            ImGui::Checkbox("Show details", &showDetails_);
            if (showDetails_) {
                ImGui::Separator();
                ImGui::TextUnformatted("This window is rendered via the shared ImGui service.");
            }
            if (ImGui::Button("Click", ImVec2(0.0f, 0.0f))) {
                clickCount_++;
            }
            ImGui::SameLine(0.0f, -1.0f);
            char buffer[64]{};
            snprintf(buffer, sizeof(buffer), "Clicks: %d", clickCount_);
            ImGui::TextUnformatted(buffer);
            ImGui::End();
        }

        void OnUpdate() override
        {
            updateCount_++;
            if (updateCount_ == 1) {
                LOG_INFO("ImGuiSample: first update");
            }
        }

        void OnVisibleChanged(bool visible) override
        {
            LOG_INFO("ImGuiSample: visibility changed to {}", visible);
        }

        void OnShutdown() override
        {
            LOG_INFO("ImGuiSample: shutdown callback");
            delete this;
        }

        void OnUnregister() override
        {
            LOG_INFO("ImGuiSample: unregister callback");
            // Don't delete here - panel will be deleted in OnShutdown
        }

    private:
        bool showDetails_ = true;
        int clickCount_ = 0;
        int updateCount_ = 0;
    };
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

        auto* panel = new SamplePanel();
        ImGuiPanelDesc desc = ImGuiPanelAdapter<SamplePanel>::MakeDesc(panel, kSamplePanelId, 100, true);
        desc.fontId = 0;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("ImGuiSample: failed to register panel");
            delete panel;
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
