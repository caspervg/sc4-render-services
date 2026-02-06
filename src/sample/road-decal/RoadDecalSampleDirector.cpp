#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZImGuiService.h"
#include "sample/road-decal/RoadDecalData.hpp"
#include "sample/road-decal/RoadDecalInputControl.hpp"
#include "utils/Logger.h"
#include "SC4UI.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace
{
    constexpr uint32_t kRoadDecalDirectorID = 0xE59A5D21;
    constexpr uint32_t kRoadDecalPanelId = 0x9B4A7A11;
    constexpr uintptr_t kDynamicCallSiteAddress = 0x007CB853;
    constexpr size_t kHookByteCount = 5;

    struct CallSitePatch
    {
        const char* name = nullptr;
        uintptr_t callSiteAddress = 0;
        uintptr_t originalTarget = 0;
        int32_t originalRel = 0;
        void* hookFn = nullptr;
        bool installed = false;
    };

    void(__thiscall* gOrigDynamic)(void*) = nullptr;
    CallSitePatch gDynamicPatch{
        "Draw::DrawDynamicView_",
        kDynamicCallSiteAddress,
        0,
        0,
        nullptr,
        false};

    RoadDecalInputControl* gRoadDecalTool = nullptr;
    std::atomic<bool> gRoadDecalToolEnabled{false};
    std::atomic<int> gRoadDecalStyle{0};
    std::atomic<float> gRoadDecalWidth{0.5f};

    bool ComputeRelativeCallTarget(uintptr_t callSiteAddress, uintptr_t targetAddress, int32_t& relOut)
    {
        const auto delta =
            static_cast<intptr_t>(targetAddress) - static_cast<intptr_t>(callSiteAddress + kHookByteCount);
        if (delta < static_cast<intptr_t>(INT32_MIN) || delta > static_cast<intptr_t>(INT32_MAX)) {
            return false;
        }

        relOut = static_cast<int32_t>(delta);
        return true;
    }

    bool InstallCallSitePatch(CallSitePatch& patch)
    {
        if (patch.installed) {
            return true;
        }

        auto* site = reinterpret_cast<uint8_t*>(patch.callSiteAddress);
        if (site[0] != 0xE8) {
            LOG_ERROR("RoadDecalSample: expected CALL rel32 at 0x{:08X} for {}",
                      static_cast<uint32_t>(patch.callSiteAddress),
                      patch.name);
            return false;
        }

        std::memcpy(&patch.originalRel, site + 1, sizeof(patch.originalRel));
        patch.originalTarget = patch.callSiteAddress + kHookByteCount + patch.originalRel;

        int32_t newRel = 0;
        if (!ComputeRelativeCallTarget(patch.callSiteAddress, reinterpret_cast<uintptr_t>(patch.hookFn), newRel)) {
            LOG_ERROR("RoadDecalSample: rel32 range failure for {}", patch.name);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(site + 1, sizeof(newRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("RoadDecalSample: VirtualProtect failed for {}", patch.name);
            return false;
        }

        std::memcpy(site + 1, &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), site, kHookByteCount);
        VirtualProtect(site + 1, sizeof(newRel), oldProtect, &oldProtect);

        patch.installed = true;
        return true;
    }

    void UninstallCallSitePatch(CallSitePatch& patch)
    {
        if (!patch.installed) {
            return;
        }

        auto* site = reinterpret_cast<uint8_t*>(patch.callSiteAddress);
        DWORD oldProtect = 0;
        if (VirtualProtect(site + 1, sizeof(patch.originalRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(site + 1, &patch.originalRel, sizeof(patch.originalRel));
            FlushInstructionCache(GetCurrentProcess(), site, kHookByteCount);
            VirtualProtect(site + 1, sizeof(patch.originalRel), oldProtect, &oldProtect);
        }

        patch.installed = false;
        patch.originalTarget = 0;
        patch.originalRel = 0;
    }

    bool InstallDynamicHook()
    {
        if (gDynamicPatch.installed) {
            return true;
        }

        if (!InstallCallSitePatch(gDynamicPatch)) {
            gOrigDynamic = nullptr;
            return false;
        }

        gOrigDynamic = reinterpret_cast<void(__thiscall*)(void*)>(gDynamicPatch.originalTarget);
        LOG_INFO("RoadDecalSample: installed dynamic pass hook");
        return true;
    }

    void UninstallDynamicHook()
    {
        if (gDynamicPatch.installed) {
            LOG_INFO("RoadDecalSample: removed dynamic pass hook");
        }

        UninstallCallSitePatch(gDynamicPatch);
        gOrigDynamic = nullptr;
    }

    bool EnableRoadDecalTool()
    {
        if (gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return true;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (!view3D) {
            LOG_WARN("RoadDecalSample: View3D not available");
            return false;
        }

        if (!gRoadDecalTool) {
            gRoadDecalTool = new RoadDecalInputControl();
            gRoadDecalTool->AddRef();
            gRoadDecalTool->SetStyle(gRoadDecalStyle.load(std::memory_order_relaxed));
            gRoadDecalTool->SetWidth(gRoadDecalWidth.load(std::memory_order_relaxed));
            gRoadDecalTool->SetOnCancel([]() {});
            gRoadDecalTool->Activate();
        }

        if (!view3D->SetCurrentViewInputControl(gRoadDecalTool,
                                                cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl)) {
            LOG_WARN("RoadDecalSample: failed to set current view input control");
            return false;
        }

        gRoadDecalToolEnabled.store(true, std::memory_order_relaxed);
        return true;
    }

    void DisableRoadDecalTool()
    {
        if (!gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (view3D) {
            auto* currentControl = view3D->GetCurrentViewInputControl();
            if (currentControl == gRoadDecalTool) {
                view3D->RemoveCurrentViewInputControl(false);
            }
        }

        gRoadDecalToolEnabled.store(false, std::memory_order_relaxed);
    }

    void DestroyRoadDecalTool()
    {
        DisableRoadDecalTool();
        if (gRoadDecalTool) {
            gRoadDecalTool->Release();
            gRoadDecalTool = nullptr;
        }
    }

    void __fastcall HookDynamic(void* self, void*)
    {
        if (gOrigDynamic) {
            gOrigDynamic(self);
        }
        DrawRoadDecals();
    }

    class RoadDecalPanel final : public ImGuiPanel
    {
    public:
        void OnRender() override
        {
            ImGui::Begin("Road Decal Sample");
            ImGui::TextUnformatted("Dynamic pass road decal overlay");

            bool toolEnabled = gRoadDecalToolEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Enable Road Decal Tool", &toolEnabled)) {
                if (toolEnabled) {
                    toolEnabled = EnableRoadDecalTool();
                } else {
                    DisableRoadDecalTool();
                }
                gRoadDecalToolEnabled.store(toolEnabled, std::memory_order_relaxed);
            }

            static const char* styleItems[] = {"White", "Yellow", "Red"};
            int style = gRoadDecalStyle.load(std::memory_order_relaxed);
            if (ImGui::Combo("Style", &style, styleItems, static_cast<int>(std::size(styleItems)))) {
                gRoadDecalStyle.store(style, std::memory_order_relaxed);
                if (gRoadDecalTool) {
                    gRoadDecalTool->SetStyle(style);
                }
            }

            float width = gRoadDecalWidth.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("Width", &width, 0.05f, 8.0f, "%.2f")) {
                gRoadDecalWidth.store(width, std::memory_order_relaxed);
                if (gRoadDecalTool) {
                    gRoadDecalTool->SetWidth(width);
                }
            }

            if (ImGui::Button("Rebuild Geometry")) {
                RebuildRoadDecalGeometry();
            }
            ImGui::SameLine();
            if (ImGui::Button("Undo Last")) {
                if (!gRoadDecalStrokes.empty()) {
                    gRoadDecalStrokes.pop_back();
                    RebuildRoadDecalGeometry();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                if (!gRoadDecalStrokes.empty()) {
                    gRoadDecalStrokes.clear();
                    RebuildRoadDecalGeometry();
                }
            }

            ImGui::Text("Strokes: %u", static_cast<uint32_t>(gRoadDecalStrokes.size()));
            ImGui::TextUnformatted("Input: LMB place points, move for preview, RMB/Enter finish stroke.");
            ImGui::TextUnformatted("Shortcuts: Ctrl+Z undo, Delete clear all, Esc cancel current stroke.");
            ImGui::End();
        }
    };
}

extern std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay;

class RoadDecalSampleDirector final : public cRZCOMDllDirector
{
public:
    [[nodiscard]] uint32_t GetDirectorID() const override
    {
        return kRoadDecalDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override
    {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4RoadDecalSample", "");
        LOG_INFO("RoadDecalSample: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        }
        return true;
    }

    bool PostAppInit() override
    {
        if (!mpFrameWork || panelRegistered_) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID,
                                           GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("RoadDecalSample: ImGui service not available");
            return true;
        }

        auto* panel = new RoadDecalPanel();
        const ImGuiPanelDesc desc =
            ImGuiPanelAdapter<RoadDecalPanel>::MakeDesc(panel, kRoadDecalPanelId, 120, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("RoadDecalSample: failed to register panel");
            delete panel;
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        panelRegistered_ = true;
        gImGuiServiceForD3DOverlay.store(imguiService_, std::memory_order_release);

        gDynamicPatch.hookFn = reinterpret_cast<void*>(&HookDynamic);
        if (!InstallDynamicHook()) {
            LOG_WARN("RoadDecalSample: dynamic hook install failed");
        }
        return true;
    }

    bool PostAppShutdown() override
    {
        UninstallDynamicHook();
        DestroyRoadDecalTool();
        gImGuiServiceForD3DOverlay.store(nullptr, std::memory_order_release);

        if (imguiService_) {
            imguiService_->UnregisterPanel(kRoadDecalPanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }

        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_ = nullptr;
    bool panelRegistered_ = false;
};

static RoadDecalSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector()
{
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
