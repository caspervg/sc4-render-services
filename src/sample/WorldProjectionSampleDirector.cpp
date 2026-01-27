// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAConstantFunctionResult
// ReSharper disable CppDFAUnreachableCode
#include "cIGZFrameWork.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC43DRender.h"
#include "cISC4View3DWin.h"
#include "cRZCOMDllDirector.h"
#include "cS3DCamera.h"
#include "GZServPtrs.h"
#include "imgui.h"
#include "public/cIGZImGuiService.h"
#include "public/ImGuiServiceIds.h"
#include "SC4UI.h"
#include "utils/Logger.h"
#include <cmath>

namespace {
    constexpr auto kWorldProjectionSampleDirectorID = 0xB7E4F2A9;
    constexpr uint32_t kWorldProjectionPanelId = 0x3D9C8B1F;

    // Function pointer type for SC4's camera Project function
    typedef bool (__thiscall *ProjectFunc)(void* camera, float* worldPos, float* screenPos);
    const auto ProjectFn = reinterpret_cast<ProjectFunc>(0x007fff10);

    struct GridConfig {
        bool enabled = true;
        int gridSpacing = 64;    // World units between grid lines
        int gridExtent = 512;    // How far from center to draw
        float centerX = 512.0f;       // Center world position X
        float centerY = 0.0f;         // Center world position Y (height)
        float centerZ = 512.0f;       // Center world position Z
        ImVec4 gridColor = ImVec4(0.0f, 1.0f, 0.0f, 0.8f);  // RGBA
        float lineThickness = 2.0f;
        bool drawCenterMarker = true;
        float markerSize = 10.0f;
    };

    bool IsCityView() {
        const cISC4AppPtr app;
        if (!app) {
            return false;
        }
        return app->GetCity() != nullptr;
    }

    bool WorldToScreen(cS3DCamera* camera, float worldX, float worldY, float worldZ,
                       float& screenX, float& screenY) {
        if (!camera) {
            return false;
        }

        float worldPos[3] = {worldX, worldY, worldZ};
        float screenPos[3] = {0.0f, 0.0f, 0.0f};

        if (ProjectFn(camera, worldPos, screenPos)) {
            screenX = screenPos[0];
            screenY = screenPos[1];
            return true;
        }

        return false;
    }

    void DrawWorldGrid(cS3DCamera* camera, const GridConfig& config) {
        if (!camera || !config.enabled) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(config.gridColor);

        // Draw grid lines parallel to X axis (running along X direction)
        for (float z = -config.gridExtent; z <= config.gridExtent; z += config.gridSpacing) {
            float worldZ = config.centerZ + z;

            // Start and end points along X axis
            float startX = config.centerX - config.gridExtent;
            float endX = config.centerX + config.gridExtent;

            float screenX1, screenY1, screenX2, screenY2;
            bool visible1 = WorldToScreen(camera, startX, config.centerY, worldZ, screenX1, screenY1);
            bool visible2 = WorldToScreen(camera, endX, config.centerY, worldZ, screenX2, screenY2);

            if (visible1 && visible2) {
                drawList->AddLine(
                    ImVec2(screenX1, screenY1),
                    ImVec2(screenX2, screenY2),
                    color,
                    config.lineThickness
                );
            }
        }

        // Draw grid lines parallel to Z axis (running along Z direction)
        for (float x = -config.gridExtent; x <= config.gridExtent; x += config.gridSpacing) {
            float worldX = config.centerX + x;

            // Start and end points along Z axis
            float startZ = config.centerZ - config.gridExtent;
            float endZ = config.centerZ + config.gridExtent;

            float screenX1, screenY1, screenX2, screenY2;
            bool visible1 = WorldToScreen(camera, worldX, config.centerY, startZ, screenX1, screenY1);
            bool visible2 = WorldToScreen(camera, worldX, config.centerY, endZ, screenX2, screenY2);

            if (visible1 && visible2) {
                drawList->AddLine(
                    ImVec2(screenX1, screenY1),
                    ImVec2(screenX2, screenY2),
                    color,
                    config.lineThickness
                );
            }
        }

        // Draw center marker
        if (config.drawCenterMarker) {
            float screenX, screenY;
            if (WorldToScreen(camera, config.centerX, config.centerY, config.centerZ, screenX, screenY)) {
                // Draw crosshair at center
                ImU32 markerColor = IM_COL32(255, 0, 0, 255);  // Red marker
                drawList->AddCircleFilled(
                    ImVec2(screenX, screenY),
                    config.markerSize * 0.5f,
                    markerColor
                );

                // Draw crosshair lines
                drawList->AddLine(
                    ImVec2(screenX - config.markerSize, screenY),
                    ImVec2(screenX + config.markerSize, screenY),
                    markerColor,
                    config.lineThickness
                );
                drawList->AddLine(
                    ImVec2(screenX, screenY - config.markerSize),
                    ImVec2(screenX, screenY + config.markerSize),
                    markerColor,
                    config.lineThickness
                );
            }
        }
    }

    void RenderWorldProjectionPanel(void* userData) {
        if (!IsCityView()) {
            return;
        }

        auto* config = static_cast<GridConfig*>(userData);
        if (!config) {
            return;
        }

        // Get camera for rendering
        auto view3DWin = SC4UI::GetView3DWin();
        if (view3DWin) {
            cISC43DRender* renderer = view3DWin->GetRenderer();
            if (renderer) {
                cS3DCamera* camera = renderer->GetCamera();
                if (camera) {
                    DrawWorldGrid(camera, *config);
                }
            }
        }

        // Control panel
        ImGui::Begin("World space", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Separator();

        ImGui::Checkbox("Enable grid", &config->enabled);

        if (config->enabled) {
            ImGui::Spacing();
            ImGui::Text("Grid Configuration");
            ImGui::Separator();

            ImGui::SliderInt("Spacing", &config->gridSpacing, 8, 256);
            ImGui::SliderInt("Extent", &config->gridExtent, 64, 2048);
            ImGui::SliderFloat("Line thickness", &config->lineThickness, 1.0f, 5.0f, "%.1f");

            ImGui::Spacing();
            ImGui::Text("Grid center");
            ImGui::Separator();

            ImGui::DragFloat("Center X", &config->centerX, 1.0f, 0.0f, 4096.0f, "%.1f");
            ImGui::DragFloat("Center Y (Height)", &config->centerY, 0.5f, -100.0f, 500.0f, "%.1f");
            ImGui::DragFloat("Center Z", &config->centerZ, 1.0f, 0.0f, 4096.0f, "%.1f");

            ImGui::Spacing();
            ImGui::Text("Appearance");
            ImGui::Separator();

            ImGui::ColorEdit4("Color", reinterpret_cast<float*>(&config->gridColor));

            ImGui::Spacing();
            ImGui::Checkbox("Draw marker", &config->drawCenterMarker);
            if (config->drawCenterMarker) {
                ImGui::SliderFloat("Marker size", &config->markerSize, 5.0f, 30.0f, "%.1f");
            }
        }

        ImGui::End();
    }

    void ShutdownWorldProjection(void* userData) {
        delete static_cast<GridConfig*>(userData);
    }
}

class WorldProjectionSampleDirector final : public cRZCOMDllDirector {
public:
    WorldProjectionSampleDirector()
        : service(nullptr),
          panelRegistered(false) {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kWorldProjectionSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4WorldProjectionSample", "");
        LOG_INFO("WorldProjectionSample: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("WorldProjectionSample: framework hook added");
        }
        else {
            LOG_WARN("WorldProjectionSample: mpFrameWork not available on start");
        }

        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("WorldProjectionSample: PostAppInit");

        if (!mpFrameWork || panelRegistered) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&service))) {
            LOG_WARN("WorldProjectionSample: ImGui service not available");
            return true;
        }

        LOG_INFO("WorldProjectionSample: obtained ImGui service (api={})", service->GetApiVersion());

        auto* config = new GridConfig();
        ImGuiPanelDesc desc{};
        desc.id = kWorldProjectionPanelId;
        desc.order = 200;  // Render after other panels
        desc.visible = true;
        desc.on_render = &RenderWorldProjectionPanel;
        desc.on_shutdown = &ShutdownWorldProjection;
        desc.data = config;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("WorldProjectionSample: failed to register panel");
            ShutdownWorldProjection(config);
            service->Release();
            service = nullptr;
            return true;
        }

        LOG_INFO("WorldProjectionSample: registered panel {}", kWorldProjectionPanelId);
        panelRegistered = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (service) {
            service->UnregisterPanel(kWorldProjectionPanelId);
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

static WorldProjectionSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
