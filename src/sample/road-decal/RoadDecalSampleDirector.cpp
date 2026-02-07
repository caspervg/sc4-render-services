#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZDrawService.h"
#include "public/cIGZImGuiService.h"
#include "sample/road-decal/RoadDecalData.hpp"
#include "sample/road-decal/RoadDecalInputControl.hpp"
#include "utils/Logger.h"
#include "SC4UI.h"

#include <atomic>
#include <cstdint>

namespace
{
    constexpr uint32_t kRoadDecalDirectorID = 0xE59A5D21;
    constexpr uint32_t kRoadDecalPanelId = 0x9B4A7A11;

    RoadDecalInputControl* gRoadDecalTool = nullptr;
    std::atomic<bool> gRoadDecalToolEnabled{false};

    RoadMarkupType gSelectedType = RoadMarkupType::SolidWhiteLine;
    PlacementMode gPlacementMode = PlacementMode::Freehand;
    float gWidth = 0.15f;
    float gLength = 3.0f;
    float gRotationDeg = 0.0f;
    bool gDashed = false;
    float gDashLength = 3.0f;
    float gGapLength = 9.0f;
    bool gAutoAlign = true;
    char gSavePath[260] = "road_markups.dat";

    void SyncToolSettings()
    {
        if (!gRoadDecalTool) {
            return;
        }
        gRoadDecalTool->SetMarkupType(gSelectedType);
        gRoadDecalTool->SetPlacementMode(gPlacementMode);
        gRoadDecalTool->SetWidth(gWidth);
        gRoadDecalTool->SetLength(gLength);
        gRoadDecalTool->SetRotation(gRotationDeg * 3.1415926f / 180.0f);
        gRoadDecalTool->SetDashed(gDashed);
        gRoadDecalTool->SetDashPattern(gDashLength, gGapLength);
        gRoadDecalTool->SetAutoAlign(gAutoAlign);
    }

    bool EnableRoadDecalTool()
    {
        if (gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return true;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (!view3D) {
            LOG_WARN("RoadMarkup: View3D not available");
            return false;
        }

        if (!gRoadDecalTool) {
            gRoadDecalTool = new RoadDecalInputControl();
            gRoadDecalTool->AddRef();
            gRoadDecalTool->SetOnCancel([]() {});
            gRoadDecalTool->Activate();
        }
        SyncToolSettings();

        if (!view3D->SetCurrentViewInputControl(gRoadDecalTool,
                                                cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl)) {
            LOG_WARN("RoadMarkup: failed to set current view input control");
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

    void DrawPassRoadDecalCallback(DrawServicePass pass, bool begin, void*)
    {
        if (pass != DrawServicePass::PreDynamic || begin) {
            return;
        }
        DrawRoadDecals();
    }

    void DrawTypeButtons(RoadMarkupCategory category)
    {
        const auto& types = GetRoadMarkupTypesForCategory(category);
        for (size_t i = 0; i < types.size(); ++i) {
            const auto type = types[i];
            const auto& props = GetRoadMarkupProperties(type);
            if (ImGui::Selectable(props.displayName, gSelectedType == type)) {
                gSelectedType = type;
                gWidth = props.defaultWidth > 0.0f ? props.defaultWidth : gWidth;
                gLength = props.defaultLength > 0.0f ? props.defaultLength : gLength;
                gDashed = props.supportsDashing;

                if (category == RoadMarkupCategory::DirectionalArrow) {
                    gPlacementMode = PlacementMode::SingleClick;
                } else if (category == RoadMarkupCategory::Crossing) {
                    gPlacementMode = PlacementMode::TwoPoint;
                } else {
                    gPlacementMode = PlacementMode::Freehand;
                }
            }
            if ((i + 1) % 2 != 0 && (i + 1) < types.size()) {
                ImGui::SameLine();
            }
        }
    }

    class RoadDecalPanel final : public ImGuiPanel
    {
    public:
        void OnRender() override
        {
            EnsureDefaultRoadMarkupLayer();
            ImGui::Begin("Road Markings");

            bool toolEnabled = gRoadDecalToolEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Enable Tool", &toolEnabled)) {
                if (toolEnabled) {
                    toolEnabled = EnableRoadDecalTool();
                } else {
                    DisableRoadDecalTool();
                }
                gRoadDecalToolEnabled.store(toolEnabled, std::memory_order_relaxed);
            }

            if (ImGui::BeginTabBar("##RoadMarkupTabs")) {
                if (ImGui::BeginTabItem("Lane Lines")) {
                    DrawTypeButtons(RoadMarkupCategory::LaneDivider);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Arrows")) {
                    DrawTypeButtons(RoadMarkupCategory::DirectionalArrow);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Crossings")) {
                    DrawTypeButtons(RoadMarkupCategory::Crossing);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Zones")) {
                    DrawTypeButtons(RoadMarkupCategory::ZoneMarking);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            const auto& props = GetRoadMarkupProperties(gSelectedType);
            ImGui::Text("Selected: %s", props.displayName);
            ImGui::SliderFloat("Width", &gWidth, 0.05f, 4.0f, "%.2f m");
            ImGui::SliderFloat("Length", &gLength, 0.5f, 12.0f, "%.2f m");
            ImGui::Checkbox("Dashed", &gDashed);
            if (gDashed) {
                ImGui::SliderFloat("Dash Length", &gDashLength, 0.2f, 12.0f, "%.2f m");
                ImGui::SliderFloat("Gap Length", &gGapLength, 0.1f, 12.0f, "%.2f m");
            }
            ImGui::Checkbox("Auto Align", &gAutoAlign);
            if (!gAutoAlign) {
                ImGui::SliderFloat("Rotation", &gRotationDeg, -180.0f, 180.0f, "%.0f deg");
            }

            int mode = static_cast<int>(gPlacementMode);
            if (ImGui::RadioButton("Freehand", mode == static_cast<int>(PlacementMode::Freehand))) mode = static_cast<int>(PlacementMode::Freehand);
            ImGui::SameLine();
            if (ImGui::RadioButton("Two Point", mode == static_cast<int>(PlacementMode::TwoPoint))) mode = static_cast<int>(PlacementMode::TwoPoint);
            ImGui::SameLine();
            if (ImGui::RadioButton("Single", mode == static_cast<int>(PlacementMode::SingleClick))) mode = static_cast<int>(PlacementMode::SingleClick);
            gPlacementMode = static_cast<PlacementMode>(mode);

            ImGui::Separator();
            ImGui::Text("Layers");
            for (size_t i = 0; i < gRoadMarkupLayers.size(); ++i) {
                auto& layer = gRoadMarkupLayers[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Checkbox("##vis", &layer.visible);
                ImGui::SameLine();
                if (ImGui::Selectable(layer.name.c_str(), gActiveLayerIndex == static_cast<int>(i))) {
                    gActiveLayerIndex = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            if (ImGui::Button("New Layer")) {
                AddRoadMarkupLayer("Layer");
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete Layer")) {
                DeleteActiveRoadMarkupLayer();
            }

            ImGui::Separator();
            if (ImGui::Button("Undo")) {
                UndoLastRoadMarkupStroke();
                RebuildRoadDecalGeometry();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                ClearAllRoadMarkupStrokes();
                RebuildRoadDecalGeometry();
            }

            ImGui::InputText("File", gSavePath, sizeof(gSavePath));
            if (ImGui::Button("Save")) {
                SaveMarkupsToFile(gSavePath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                LoadMarkupsFromFile(gSavePath);
            }

            ImGui::Text("Markings: %u", static_cast<uint32_t>(GetTotalRoadMarkupStrokeCount()));
            ImGui::TextUnformatted("LMB: place/draw  RMB: finish/clear  ESC: cancel  Ctrl+Z: undo");

            SyncToolSettings();
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
            LOG_WARN("RoadMarkup: ImGui service not available");
            return true;
        }

        auto* panel = new RoadDecalPanel();
        const ImGuiPanelDesc desc = ImGuiPanelAdapter<RoadDecalPanel>::MakeDesc(panel, kRoadDecalPanelId, 120, true);
        if (!imguiService_->RegisterPanel(desc)) {
            delete panel;
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }
        panelRegistered_ = true;
        gImGuiServiceForD3DOverlay.store(imguiService_, std::memory_order_release);

        if (!mpFrameWork->GetSystemService(kDrawServiceID,
                                           GZIID_cIGZDrawService,
                                           reinterpret_cast<void**>(&drawService_))) {
            LOG_WARN("RoadMarkup: Draw service not available");
            return true;
        }

        drawService_->RegisterDrawPassCallback(DrawServicePass::PreDynamic,
                                               &DrawPassRoadDecalCallback,
                                               nullptr,
                                               &drawPassCallbackToken_);
        return true;
    }

    bool PostAppShutdown() override
    {
        if (drawService_) {
            if (drawPassCallbackToken_ != 0) {
                drawService_->UnregisterDrawPassCallback(drawPassCallbackToken_);
                drawPassCallbackToken_ = 0;
            }
            drawService_->Release();
            drawService_ = nullptr;
        }

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
    cIGZDrawService* drawService_ = nullptr;
    uint32_t drawPassCallbackToken_ = 0;
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
