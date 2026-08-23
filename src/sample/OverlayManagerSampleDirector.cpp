#include "cRZCOMDllDirector.h"

#include "cIGZFrameWork.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "GZServPtrs.h"
#include "cISTEOverlayManager.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "cS3DVector2.h"
#include "cRZBaseString.h"
#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

#include <cmath>
#include <cinttypes>
#include <cstdio>

#include "GraphicsInterfaceHook.h"

namespace {
    constexpr auto kOverlaySampleDirectorID = 0xB4A6E2F1;
    constexpr uint32_t kOverlaySamplePanelId = 0x4D7C91AA;

    class OverlayManagerPanel final : public ImGuiPanel {
    public:
        OverlayManagerPanel() = default;

        void OnInit() override {
            LOG_INFO("OverlayManagerSample: panel initialized");
        }

        void OnShutdown() override {
            LOG_INFO("OverlayManagerSample: panel shutdown");
            delete this;
        }

        void OnRender() override {
            cISTEOverlayManager* overlay = ResolveOverlay();

            ImGui::Begin("Overlay Manager Sample", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            if (!overlay) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "Overlay manager unavailable (enter city view).");
            }
            else {
                ImGui::SeparatorText("Add Decal");
                ImGui::InputScalar("Texture IID (hex)", ImGuiDataType_U32, &textureIID_, nullptr, nullptr, "%08X");
                ImGui::InputFloat2("Center (x,z)", &decalCenter_[0]);
                ImGui::InputFloat("Base size", &decalBaseSize_, 0.1f, 1.0f, "%.2f");
                ImGui::InputFloat("Rotation (turns)", &decalRotationTurns_, 0.01f, 0.1f, "%.3f");
                if (ImGui::Button("Add Decal")) {
                    lastOverlayId_ = overlay->AddDecal(
                        textureIID_,
                        cS3DVector2{decalCenter_[0], decalCenter_[1]},
                        decalBaseSize_,
                        decalRotationTurns_);
                    SetStatus("AddDecal called");
                }
                ImGui::SameLine();
                if (ImGui::Button("Add Ring Decal")) {
                    lastOverlayId_ = overlay->AddRingDecal(
                        textureIID_,
                        cS3DVector2{decalCenter_[0], decalCenter_[1]},
                        decalBaseSize_,
                        decalRotationTurns_);
                    SetStatus("AddRingDecal called");
                }

                ImGui::SeparatorText("Overlay Controls");
                ImGui::InputScalar("Overlay ID", ImGuiDataType_U32, &overlayId_, nullptr, nullptr, "%u");
                ImGui::SameLine();
                if (ImGui::Button("Use Last Created")) {
                    overlayId_ = lastOverlayId_;
                }

                ImGui::InputFloat2("Move to center (x,z)", &moveCenter_[0]);
                ImGui::Checkbox("Snap center to tile center (16m)", &snapCenterToTile_);
                ImGui::SameLine();
                if (ImGui::Button("Snap Move Center")) {
                    moveCenter_[0] = SnapToTileCenter(moveCenter_[0]);
                    moveCenter_[1] = SnapToTileCenter(moveCenter_[1]);
                }
                if (ImGui::Button("Move Decal")) {
                    const float targetX = snapCenterToTile_ ? SnapToTileCenter(moveCenter_[0]) : moveCenter_[0];
                    const float targetY = snapCenterToTile_ ? SnapToTileCenter(moveCenter_[1]) : moveCenter_[1];
                    moveCenter_[0] = targetX;
                    moveCenter_[1] = targetY;
                    overlay->MoveDecal(overlayId_, cS3DVector2{targetX, targetY});
                    SetStatus("MoveDecal called");
                }

                ImGui::InputFloat("Alpha", &alpha_, 0.05f, 0.5f, "%.2f");
                ImGui::SameLine();
                if (ImGui::Button("Set Alpha")) {
                    overlay->SetOverlayAlpha(overlayId_, alpha_);
                    SetStatus("SetOverlayAlpha called");
                }

                ImGui::Checkbox("Enabled", &enabled_);
                ImGui::SameLine();
                if (ImGui::Button("Apply Enabled")) {
                    overlay->SetOverlayEnabled(overlayId_, enabled_);
                    SetStatus("SetOverlayEnabled called");
                }

                if (ImGui::Button("Remove Overlay")) {
                    overlay->RemoveOverlay(overlayId_);
                    SetStatus("RemoveOverlay called");
                }

                ImGui::SeparatorText("Decal Info");
                ImGui::InputFloat2("Center (x,z)##info", &infoCenter_[0]);
                ImGui::InputFloat("Base size##info", &infoBaseSize_, 0.1f, 1.0f, "%.2f");
                ImGui::InputFloat("Rotation turns##info", &infoRotationTurns_, 0.01f, 0.1f, "%.3f");
                ImGui::InputFloat("Aspect multiplier##info", &infoAspectMultiplier_, 0.01f, 0.1f, "%.3f");
                ImGui::InputFloat("UV scale U##info", &infoUvScaleU_, 0.01f, 0.1f, "%.3f");
                ImGui::InputFloat("UV scale V##info", &infoUvScaleV_, 0.01f, 0.1f, "%.3f");
                ImGui::InputFloat("UV offset (uniform)##info", &infoUvOffset_, 0.01f, 0.1f, "%.3f");
                ImGui::InputFloat("Unknown8##info", &infoUnknown8_, 0.01f, 0.1f, "%.3f");
                ImGui::Checkbox("Apply center via MoveDecal##info", &applyCenterViaMoveDecal_);
                ImGui::SameLine();
                if (ImGui::Button("Snap Info Center")) {
                    infoCenter_[0] = SnapToTileCenter(infoCenter_[0]);
                    infoCenter_[1] = SnapToTileCenter(infoCenter_[1]);
                }

                if (ImGui::Button("Fetch DecalInfo")) {
                    cISTEOverlayManager::cDecalInfo fetched{};
                    overlay->DecalInfo(overlayId_, &fetched);
                    infoCenter_[0] = fetched.center.fX;
                    infoCenter_[1] = fetched.center.fY;
                    infoBaseSize_ = fetched.baseSize;
                    infoRotationTurns_ = fetched.rotationTurns;
                    infoAspectMultiplier_ = fetched.aspectMultiplier;
                    infoUvScaleU_ = fetched.uvScaleU;
                    infoUvScaleV_ = fetched.uvScaleV;
                    infoUvOffset_ = fetched.uvOffset;
                    infoUnknown8_ = fetched.unknown8;
                    SetStatus("DecalInfo fetched");
                }
                ImGui::SameLine();
                if (ImGui::Button("Apply DecalInfo")) {
                    const float targetInfoX = snapCenterToTile_ ? SnapToTileCenter(infoCenter_[0]) : infoCenter_[0];
                    const float targetInfoY = snapCenterToTile_ ? SnapToTileCenter(infoCenter_[1]) : infoCenter_[1];
                    infoCenter_[0] = targetInfoX;
                    infoCenter_[1] = targetInfoY;

                    cISTEOverlayManager::cDecalInfo info{};
                    if (applyCenterViaMoveDecal_) {
                        cISTEOverlayManager::cDecalInfo current{};
                        overlay->DecalInfo(overlayId_, &current);
                        info.center = current.center;
                    }
                    else {
                        info.center = cS3DVector2{targetInfoX, targetInfoY};
                    }
                    info.baseSize = infoBaseSize_;
                    info.rotationTurns = infoRotationTurns_;
                    info.aspectMultiplier = infoAspectMultiplier_;
                    info.uvScaleU = infoUvScaleU_;
                    info.uvScaleV = infoUvScaleV_;
                    info.uvOffset = infoUvOffset_;
                    info.unknown8 = infoUnknown8_;
                    overlay->UpdateDecalInfo(overlayId_, info);
                    if (applyCenterViaMoveDecal_) {
                        overlay->MoveDecal(overlayId_, cS3DVector2{targetInfoX, targetInfoY});
                        SetStatus("UpdateDecalInfo + MoveDecal called");
                    }
                    else {
                        SetStatus("UpdateDecalInfo called");
                    }
                }

                ImGui::SeparatorText("Query");
                if (ImGui::Button("Refresh Stats")) {
                    cRZBaseString stats;
                    overlay->GetStatsString(stats);
                    const char* text = stats.ToChar();
                    std::snprintf(statsBuffer_, sizeof(statsBuffer_), "%s", text ? text : "");
                    SetStatus("GetStatsString called");
                }
                ImGui::InputTextMultiline("Stats", statsBuffer_, IM_ARRAYSIZE(statsBuffer_), ImVec2(360, 80),
                                          ImGuiInputTextFlags_ReadOnly);
            }

            ImGui::Separator();
            ImGui::TextWrapped("%s", status_);
            ImGui::End();

            // Separate window for height map tuning
            ImGui::Begin("Overlay HeightMap", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("Overlay type: %s",
                        OverlayTypeLabel(static_cast<cISTETerrainView::tOverlayManagerType>(overlayType_)));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::Combo("##overlayType", &overlayType_, kOverlayTypeNames, IM_ARRAYSIZE(kOverlayTypeNames));
            ImGui::SameLine();
            if (ImGui::Button("Refresh Manager")) {
                overlay = ResolveOverlay(true);
            }

            if (!overlay) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "Overlay manager unavailable (enter city view).");
                ImGui::End();
                return;
            }
            ImGui::Text("Overlay ID: %u", overlayId_);
            ImGui::InputScalar("Overlay ID##height", ImGuiDataType_U32, &overlayId_, nullptr, nullptr, "%u");
            ImGui::InputFloat("Contour interval (m)", &heightParamA_, 0.1f, 2.0f, "%.1f");
            ImGui::InputScalar("Texture ID##text", ImGuiDataType_U32, &textureIID_, nullptr, nullptr, "%08X");
            if (ImGui::Button("Add HeightMap")) {
                overlay->SetHeightMapParams(lastOverlayId_, -1.0, -1.0); // Remove the last overlay by setting ANY parameters
                lastOverlayId_ = CreateCustomContours(overlay, textureIID_, heightParamA_);
                LOG_INFO("OverlayManagerSample: Added HeightMap with texture IID 0x{:08X}, got overlay ID {}",
                         textureIID_, lastOverlayId_);
                overlayId_ = lastOverlayId_;
                SetStatus("AddHeightMap called");
            }
            ImGui::End();
        }

    private:
        cISTEOverlayManager* ResolveOverlay(bool forceRefresh = false) {
            if (!forceRefresh && cachedOverlay_ && cachedType_ == overlayType_) {
                return cachedOverlay_;
            }

            cachedOverlay_ = nullptr;
            cachedType_ = overlayType_;

            cISC4AppPtr app;
            cISC4City* city = app ? app->GetCity() : nullptr;
            cISTETerrain* terrain = city ? city->GetTerrain() : nullptr;
            cISTETerrainView* view = terrain ? terrain->GetView() : nullptr;
            if (view) {
                cachedOverlay_ = view->GetOverlayManager(
                    static_cast<cISTETerrainView::tOverlayManagerType>(overlayType_));
            }
            return cachedOverlay_;
        }

        static float MetersToConstant(float desiredIntervalMeters) {
            int terrainHeight = 256;  // Or read from *(int*)((char*)overlay + 0x60)
            return static_cast<float>(terrainHeight) / desiredIntervalMeters;
        }

        static float SnapToTileCenter(const float value) {
            return std::floor(value / 16.0f) * 16.0f + 8.0f;
        }

        static uint32_t CreateCustomContours(cISTEOverlayManager* overlay, const uint32_t textureIID,
                                             const float intervalMeters = 128.0f) {

            // Address of the 2.0 constant used by AddHeightMap
            auto pConstant = reinterpret_cast<float*>(0x00a8825c);

            // Save the original value (2.0)
            float savedValue = *pConstant;

            // Make the memory writable
            DWORD oldProtect;
            VirtualProtect(pConstant, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect);

            // Set your custom interval (higher = more contour lines)
            *pConstant = MetersToConstant(intervalMeters);

            // Restore original protection
            VirtualProtect(pConstant, sizeof(float), oldProtect, &oldProtect);

            // Call AddHeightMap - it will use our custom value
            const auto overlayId = overlay->AddHeightMap(textureIID);

            // IMMEDIATELY restore the original constant
            VirtualProtect(pConstant, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect);
            *pConstant = savedValue;
            VirtualProtect(pConstant, sizeof(float), oldProtect, &oldProtect);

            return overlayId;
        }

        static const char* OverlayTypeLabel(const cISTETerrainView::tOverlayManagerType type) {
            switch (type) {
            case cISTETerrainView::tOverlayManagerType::StaticLand: return "StaticLand";
            case cISTETerrainView::tOverlayManagerType::StaticWater: return "StaticWater";
            case cISTETerrainView::tOverlayManagerType::DynamicLand: return "DynamicLand";
            case cISTETerrainView::tOverlayManagerType::DynamicWater: return "DynamicWater";
            default: return "Unknown";
            }
        }

        void SetStatus(const char* text) {
            std::snprintf(status_, sizeof(status_), "%s", text);
            LOG_INFO("OverlayManagerSample: {}", text);
        }

    private:
        static constexpr const char* kOverlayTypeNames[] = {"StaticLand", "StaticWater", "DynamicLand", "DynamicWater"};

        int overlayType_ = static_cast<int>(cISTETerrainView::tOverlayManagerType::DynamicLand);
        cISTEOverlayManager* cachedOverlay_ = nullptr;
        int cachedType_ = -1;

        uint32_t textureIID_ = 0x5DA69704; // Default overlay test texture
        float decalCenter_[2] = {512.0f, 512.0f};
        float decalBaseSize_ = 1.0f;
        float decalRotationTurns_ = 0.0f;

        uint32_t overlayId_ = 0;
        uint32_t lastOverlayId_ = 0;
        float moveCenter_[2] = {512.0f, 512.0f};
        float alpha_ = 1.0f;
        bool enabled_ = true;
        float heightParamA_ = 0.0f;
        bool snapCenterToTile_ = true;
        bool applyCenterViaMoveDecal_ = true;

        // DecalInfo exploration
        float infoCenter_[2] = {512.0f, 512.0f};
        float infoBaseSize_ = 1.0f;
        float infoRotationTurns_ = 0.0f;
        float infoAspectMultiplier_ = 1.0f;
        float infoUvScaleU_ = 1.0f;
        float infoUvScaleV_ = 1.0f;
        float infoUvOffset_ = 0.0f;
        float infoUnknown8_ = 0.0f;

        char status_[128] = "Idle";
        char statsBuffer_[256] = "";
    };
}

class OverlayManagerSampleDirector final : public cRZCOMDllDirector {
public:
    OverlayManagerSampleDirector()
        : imguiService_(nullptr), panelRegistered_(false) {}

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kOverlaySampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4OverlayManagerSample", "");
        LOG_INFO("OverlayManagerSample: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("OverlayManagerSample: framework hook added");
        }
        else {
            LOG_WARN("OverlayManagerSample: mpFrameWork not available on start");
        }
        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("OverlayManagerSample: PostAppInit");
        if (!mpFrameWork || panelRegistered_) {
            if (!mpFrameWork) {
                LOG_WARN("OverlayManagerSample: mpFrameWork not available in PostAppInit");
            }
            if (panelRegistered_) {
                LOG_WARN("OverlayManagerSample: panel already registered");
            }
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("OverlayManagerSample: ImGui service not available");
            return true;
        }

        auto* panel = new OverlayManagerPanel();
        ImGuiPanelDesc desc = ImGuiPanelAdapter<OverlayManagerPanel>::MakeDesc(panel, kOverlaySamplePanelId, 140, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("OverlayManagerSample: failed to register panel");
            delete panel;
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        LOG_INFO("OverlayManagerSample: registered panel {}", kOverlaySamplePanelId);
        panelRegistered_ = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (imguiService_) {
            imguiService_->UnregisterPanel(kOverlaySamplePanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_;
    bool panelRegistered_;
};

static OverlayManagerSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
