#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/S3DCameraServiceIds.h"
#include "public/cIGZImGuiService.h"
#include "public/cIGZS3DCameraService.h"
#include "utils/Logger.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace {
    constexpr auto kS3DCameraDebugDirectorID = 0xB6F4D7E1;
    constexpr uint32_t kS3DCameraDebugPanelId = 0x2C7E1A35;

    enum class CameraSource : int {
        Renderer = 0,
        Managed = 1
    };

    struct Float4 {
        float a;
        float b;
        float c;
        float d;
    };

    class S3DCameraDebugPanel final : public ImGuiPanel {
    public:
        explicit S3DCameraDebugPanel(cIGZS3DCameraService* cameraService)
            : cameraService_(cameraService) {
            std::snprintf(status_, sizeof(status_), "Idle");
            std::snprintf(wrapPtrInput_, sizeof(wrapPtrInput_), "0");
        }

        void OnInit() override {
            LOG_INFO("S3DCameraDebug: panel initialized");
            RefreshRendererCamera();
        }

        void OnShutdown() override {
            DestroyManagedCamera();
            LOG_INFO("S3DCameraDebug: panel shutdown");
            delete this;
        }

        void OnUnregister() override {
            LOG_INFO("S3DCameraDebug: panel unregistered");
        }

        void OnRender() override {
            ImGui::Begin("S3D Camera Service Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::Text("Service pointer: %p", cameraService_);
            if (!cameraService_) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Camera service unavailable (check SC4RenderServices load/version)");
                ImGui::End();
                return;
            }

            ImGui::SeparatorText("Handle Management");

            if (ImGui::Button("Wrap Active Renderer Camera")) {
                RefreshRendererCamera();
                SetStatus("Wrapped current renderer camera");
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Managed Camera")) {
                managedCamera_ = cameraService_->CreateCamera();
                activeSource_ = managedCamera_.ptr ? CameraSource::Managed : activeSource_;
                SetStatus(managedCamera_.ptr ? "Created managed camera" : "CreateCamera returned null (version mismatch?)");
            }
            ImGui::SameLine();
            if (ImGui::Button("Destroy Managed")) {
                DestroyManagedCamera();
                SetStatus("Destroyed managed camera");
            }

            ImGuiInputTextFlags hexFlags = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_AutoSelectAll;
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputText("Wrap Raw Pointer (hex)", wrapPtrInput_, IM_ARRAYSIZE(wrapPtrInput_), hexFlags);
            ImGui::SameLine();
            if (ImGui::Button("Wrap Pointer")) {
                const auto ptr = ParsePointer(wrapPtrInput_);
                if (ptr) {
                    rendererCamera_ = cameraService_->WrapCamera(ptr);
                    activeSource_ = CameraSource::Renderer;
                    SetStatus("Wrapped manual pointer");
                }
                else {
                    SetStatus("Invalid pointer text");
                }
            }

            DrawHandleInfo();

            const S3DCameraHandle handle = GetActiveHandle();
            if (!handle.ptr) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "No active handle selected.");
                ImGui::TextUnformatted(status_);
                ImGui::End();
                return;
            }

            ImGui::SeparatorText("Projection Queries");
            if (ImGui::Button("Refresh Projection Info")) {
                projectionType_ = cameraService_->GetProjectionType(handle);
                projectionState_ = cameraService_->GetProjectionState(handle);
                projectionMatrix_ = cameraService_->GetProjectionMatrix(handle);
                viewTransform_ = cameraService_->GetViewTransform(handle);
                viewState_ = cameraService_->GetViewState(handle);
                SetStatus("Updated projection info");
            }
            ImGui::Text("ProjectionType: %d | ProjectionState: %d | ViewState: %d", projectionType_, projectionState_, viewState_);
            ImGui::Text("ProjectionMatrix*: 0x%08" PRIxPTR " | ViewTransform*: %p", projectionMatrix_, viewTransform_);

            ImGui::SeparatorText("Project / UnProject");
            ImGui::InputFloat3("World (for Project/WorldToScreen)", worldPos_);
            if (ImGui::Button("Project World -> Screen")) {
                cS3DVector3 world{worldPos_[0], worldPos_[1], worldPos_[2]};
                cS3DVector3 screen{};
                projectOk_ = cameraService_->Project(handle, world, screen);
                if (projectOk_) {
                    screenPos_[0] = screen.fX;
                    screenPos_[1] = screen.fY;
                    screenPos_[2] = screen.fZ;
                    SetStatus("Project succeeded");
                } else {
                    SetStatus("Project failed");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("WorldToScreen")) {
                worldToScreenOk_ = cameraService_->WorldToScreen(handle, worldPos_[0], worldPos_[1], worldPos_[2],
                                                                 screenPos_[0], screenPos_[1], &worldToScreenDepth_);
                if (worldToScreenOk_) {
                    screenPos_[2] = worldToScreenDepth_;
                    SetStatus("WorldToScreen succeeded");
                } else {
                    SetStatus("WorldToScreen failed");
                }
            }
            ImGui::InputFloat3("Screen (x,y,depth) for UnProject", screenPos_);
            if (ImGui::Button("UnProject Screen -> World")) {
                const cS3DVector3 screen{screenPos_[0], screenPos_[1], screenPos_[2]};
                cS3DVector3 world{};
                unprojectOk_ = cameraService_->UnProject(handle, screen, world);
                if (unprojectOk_) {
                    worldFromScreen_[0] = world.fX;
                    worldFromScreen_[1] = world.fY;
                    worldFromScreen_[2] = world.fZ;
                    SetStatus("UnProject succeeded");
                } else {
                    SetStatus("UnProject failed");
                }
            }
            ImGui::InputFloat3("UnProject result", worldFromScreen_, "%.3f", ImGuiInputTextFlags_ReadOnly);

            ImGui::SeparatorText("View / Look");
            if (ImGui::Button("Make Identity")) {
                cameraService_->MakeIdentity(handle);
                SetStatus("MakeIdentity applied");
            }
            ImGui::InputFloat3("Eye (LookAt)", eye_);
            ImGui::InputFloat3("Up (LookAt)", up_);
            if (ImGui::Button("Apply LookAt")) {
                const cS3DVector3 eye{eye_[0], eye_[1], eye_[2]};
                const cS3DVector3 up{up_[0], up_[1], up_[2]};
                cameraService_->LookAt(handle, eye, up);
                SetStatus("LookAt applied");
            }
            if (ImGui::Button("Get Position")) {
                cS3DVector3 pos{};
                cameraService_->GetPosition(handle, pos);
                position_[0] = pos.fX;
                position_[1] = pos.fY;
                position_[2] = pos.fZ;
                SetStatus("Position refreshed");
            }
            ImGui::SameLine();
            if (ImGui::Button("Set Position")) {
                const cS3DVector3 pos{position_[0], position_[1], position_[2]};
                cameraService_->SetPosition(handle, pos);
                SetStatus("Position updated");
            }
            ImGui::InputFloat3("Position", position_);

            if (ImGui::Button("Get LookAt")) {
                cS3DVector3 look{};
                cameraService_->GetLookAt(handle, look);
                lookAt_[0] = look.fX;
                lookAt_[1] = look.fY;
                lookAt_[2] = look.fZ;
                SetStatus("LookAt vector refreshed");
            }
            ImGui::InputFloat3("LookAt (read-only)", lookAt_, "%.3f", ImGuiInputTextFlags_ReadOnly);

            ImGui::SeparatorText("Viewport / Volume");
            ImGui::InputFloat2("Viewport (width,height)", viewport_);
            ImGui::SameLine();
            if (ImGui::Button("Set Viewport")) {
                cameraService_->SetViewport(handle, viewport_[0], viewport_[1]);
                SetStatus("Viewport applied");
            }
            ImGui::InputFloat2("Viewport Offset", viewportOffset_);
            ImGui::SameLine();
            if (ImGui::Button("Set Offset")) {
                const cS3DVector2 offset{viewportOffset_[0], viewportOffset_[1]};
                cameraService_->SetViewportOffset(handle, offset);
                SetStatus("Viewport offset applied");
            }
            ImGui::InputFloat4("Subview (l,t,r,b)", &subview_.a);
            if (ImGui::Button("Set Subview")) {
                cameraService_->SetSubview(handle, subview_.a, subview_.b, subview_.c, subview_.d);
                SetStatus("Subview applied");
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Subview")) {
                cameraService_->ClearSubview(handle);
                SetStatus("Subview cleared");
            }

            ImGui::InputFloat4("Ortho (l,r,b,t)", &ortho_[0]);
            ImGui::InputFloat2("Ortho (near,far)", &ortho_[4]);
            if (ImGui::Button("Set Ortho")) {
                cameraService_->SetOrtho(handle, ortho_[0], ortho_[1], ortho_[2], ortho_[3], ortho_[4], ortho_[5]);
                SetStatus("Ortho applied");
            }

            if (ImGui::Button("Get View Volume")) {
                cameraService_->GetViewVolume(handle,
                                              &viewVolume_[0], &viewVolume_[1], &viewVolume_[2], &viewVolume_[3],
                                              &viewVolumeNear_, &viewVolumeFar_);
                SetStatus("View volume refreshed");
            }
            ImGui::Text("Volume: L=%.3f R=%.3f B=%.3f T=%.3f Near=%d Far=%d",
                        viewVolume_[0], viewVolume_[1], viewVolume_[2], viewVolume_[3], viewVolumeNear_, viewVolumeFar_);

            ImGui::SeparatorText("Rays / Depth");
            ImGui::InputFloat2("Screen pos (EyeRay)", eyeRayScreen_);
            if (ImGui::Button("Get Eye Ray")) {
                const cS3DVector2 screen{eyeRayScreen_[0], eyeRayScreen_[1]};
                cameraService_->GetEyeRay(handle, screen, eyeRayOrigin_, eyeRayDirection_);
                eyeRayOk_ = true;
                SetStatus("Eye ray computed");
            }
            if (eyeRayOk_) {
                ImGui::Text("Ray origin: (%.3f, %.3f, %.3f)", eyeRayOrigin_.fX, eyeRayOrigin_.fY, eyeRayOrigin_.fZ);
                ImGui::Text("Ray direction: (%.3f, %.3f, %.3f)", eyeRayDirection_.fX, eyeRayDirection_.fY, eyeRayDirection_.fZ);
            }

            ImGui::InputFloat("Depth offset", &depthOffset_, 0.01f, 1.0f, "%.3f");
            ImGui::SameLine();
            if (ImGui::Button("Set Depth Offset")) {
                cameraService_->SetDepthOffset(handle, depthOffset_);
                SetStatus("Depth offset applied");
            }

            ImGui::Text("Results: Project=%s | UnProject=%s | WorldToScreen=%s | EyeRay=%s",
                        BoolLabel(projectOk_), BoolLabel(unprojectOk_), BoolLabel(worldToScreenOk_), BoolLabel(eyeRayOk_));

            ImGui::Separator();
            ImGui::TextWrapped("%s", status_);
            ImGui::End();
        }

    private:
        void RefreshRendererCamera() {
            rendererCamera_ = cameraService_->WrapActiveRendererCamera();
        }

        void DestroyManagedCamera() {
            if (cameraService_ && managedCamera_.ptr) {
                cameraService_->DestroyCamera(managedCamera_);
            }
            managedCamera_ = {nullptr, 0, false};
            if (activeSource_ == CameraSource::Managed) {
                activeSource_ = CameraSource::Renderer;
            }
        }

        S3DCameraHandle GetActiveHandle() const {
            if (activeSource_ == CameraSource::Managed) {
                return managedCamera_;
            }
            return rendererCamera_;
        }

        void DrawHandleInfo() {
            ImGui::Text("Renderer camera: ptr=%p ver=%u owned=%s",
                        rendererCamera_.ptr, rendererCamera_.version, rendererCamera_.owned ? "true" : "false");
            ImGui::Text("Managed camera : ptr=%p ver=%u owned=%s",
                        managedCamera_.ptr, managedCamera_.version, managedCamera_.owned ? "true" : "false");

            int selected = static_cast<int>(activeSource_);
            const bool rendererAvailable = rendererCamera_.ptr != nullptr;
            const bool managedAvailable = managedCamera_.ptr != nullptr;

            if (!rendererAvailable && selected == static_cast<int>(CameraSource::Renderer) && managedAvailable) {
                selected = static_cast<int>(CameraSource::Managed);
            }

            if (rendererAvailable) {
                ImGui::RadioButton("Use renderer camera", &selected, static_cast<int>(CameraSource::Renderer));
            }
            if (managedAvailable) {
                ImGui::RadioButton("Use managed camera", &selected, static_cast<int>(CameraSource::Managed));
            }

            activeSource_ = static_cast<CameraSource>(selected);
        }

        void SetStatus(const char* text) {
            std::snprintf(status_, sizeof(status_), "%s", text);
        }

        static void* ParsePointer(const char* text) {
            if (!text || *text == '\0') {
                return nullptr;
            }
            char* end = nullptr;
            const auto value = std::strtoull(text, &end, 16);
            if (end == text) {
                return nullptr;
            }
            return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
        }

        static const char* BoolLabel(const bool value) {
            return value ? "OK" : "--";
        }

    private:
        cIGZS3DCameraService* cameraService_;
        S3DCameraHandle rendererCamera_{nullptr, 0, false};
        S3DCameraHandle managedCamera_{nullptr, 0, false};
        CameraSource activeSource_ = CameraSource::Renderer;

        char wrapPtrInput_[64]{};
        char status_[160]{};

        float worldPos_[3] = {512.0f, 32.0f, 512.0f};
        float screenPos_[3] = {0.0f, 0.0f, 0.0f};
        float worldFromScreen_[3] = {0.0f, 0.0f, 0.0f};
        float viewport_[2] = {1024.0f, 768.0f};
        float viewportOffset_[2] = {0.0f, 0.0f};
        Float4 subview_{0.0f, 0.0f, 1.0f, 1.0f};
        float ortho_[6] = {-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 2000.0f};
        float eye_[3] = {512.0f, 200.0f, 512.0f};
        float up_[3] = {0.0f, 1.0f, 0.0f};
        float position_[3] = {0.0f, 0.0f, 0.0f};
        float lookAt_[3] = {0.0f, 0.0f, 0.0f};
        float eyeRayScreen_[2] = {512.0f, 384.0f};
        float depthOffset_ = 0.0f;
        float viewVolume_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float worldToScreenDepth_ = 0.0f;
        int viewVolumeNear_ = 0;
        int viewVolumeFar_ = 0;
        int projectionType_ = 0;
        int projectionState_ = 0;
        int viewState_ = 0;
        intptr_t projectionMatrix_ = 0;
        const cS3DTransform* viewTransform_ = nullptr;
        cS3DVector3 eyeRayOrigin_{};
        cS3DVector3 eyeRayDirection_{};

        bool projectOk_ = false;
        bool unprojectOk_ = false;
        bool worldToScreenOk_ = false;
        bool eyeRayOk_ = false;
    };
}

class S3DCameraDebugSampleDirector final : public cRZCOMDllDirector {
public:
    S3DCameraDebugSampleDirector()
        : imguiService_(nullptr), cameraService_(nullptr), panelRegistered_(false) {}

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kS3DCameraDebugDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4S3DCameraDebug", "");
        LOG_INFO("S3DCameraDebug: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("S3DCameraDebug: framework hook added");
        } else {
            LOG_WARN("S3DCameraDebug: mpFrameWork not available on start");
        }
        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("S3DCameraDebug: PostAppInit");
        if (!mpFrameWork || panelRegistered_) {
            if (!mpFrameWork) {
                LOG_WARN("S3DCameraDebug: mpFrameWork not available in PostAppInit");
            }
            if (panelRegistered_) {
                LOG_WARN("S3DCameraDebug: panel already registered");
            }
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("S3DCameraDebug: ImGui service not available");
            return true;
        }

        if (!mpFrameWork->GetSystemService(kS3DCameraServiceID, GZIID_cIGZS3DCameraService,
                                           reinterpret_cast<void**>(&cameraService_))) {
            LOG_WARN("S3DCameraDebug: S3D camera service not available");
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        auto* panel = new S3DCameraDebugPanel(cameraService_);
        ImGuiPanelDesc desc = ImGuiPanelAdapter<S3DCameraDebugPanel>::MakeDesc(panel, kS3DCameraDebugPanelId, 130, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("S3DCameraDebug: failed to register panel");
            delete panel;
            cameraService_->Release();
            imguiService_->Release();
            cameraService_ = nullptr;
            imguiService_ = nullptr;
            return true;
        }

        LOG_INFO("S3DCameraDebug: registered panel {}", kS3DCameraDebugPanelId);
        panelRegistered_ = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (imguiService_) {
            imguiService_->UnregisterPanel(kS3DCameraDebugPanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }
        if (cameraService_) {
            cameraService_->Release();
            cameraService_ = nullptr;
        }
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_;
    cIGZS3DCameraService* cameraService_;
    bool panelRegistered_;
};

static S3DCameraDebugSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
