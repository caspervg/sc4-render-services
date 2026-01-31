#pragma once

#include <cstdint>

#include "cRZBaseSystemService.h"
#include "cS3DCamera.h"
#include "public/cIGZS3DCameraService.h"
#include "utils/VersionDetection.h"

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class S3DCameraService final : public cRZBaseSystemService, public cIGZS3DCameraService {
public:
    S3DCameraService();
    ~S3DCameraService() = default;

    // IUnknown
    uint32_t AddRef() override;
    uint32_t Release() override;
    bool QueryInterface(uint32_t riid, void** ppvObj) override;

    // cIGZS3DCameraService
    [[nodiscard]] uint32_t GetServiceID() const override;

    S3DCameraHandle CreateCamera() override;
    void DestroyCamera(S3DCameraHandle handle) override;
    S3DCameraHandle WrapCamera(void* existingCameraPtr) override;
    S3DCameraHandle WrapActiveRendererCamera() override;

    bool Project(S3DCameraHandle handle, const cS3DVector3& worldPos, cS3DVector3& screenPos) override;
    bool UnProject(S3DCameraHandle handle, const cS3DVector3& screenPos, cS3DVector3& worldPos) override;
    bool WorldToScreen(S3DCameraHandle handle, float worldX, float worldY, float worldZ,
                       float& screenX, float& screenY, float* depthOut = nullptr) override;
    intptr_t GetProjectionMatrix(S3DCameraHandle handle) override;
    int GetProjectionType(S3DCameraHandle handle) override;
    int GetProjectionState(S3DCameraHandle handle) override;

    const cS3DTransform* GetViewTransform(S3DCameraHandle handle) override;

    void SetViewport(S3DCameraHandle handle, float width, float height) override;
    void SetViewportOffset(S3DCameraHandle handle, const cS3DVector2& offset) override;
    void SetSubview(S3DCameraHandle handle, float left, float top, float right, float bottom) override;
    void ClearSubview(S3DCameraHandle handle) override;
    void SetOrtho(S3DCameraHandle handle, float left, float right, float bottom, float top, float nearPlane, float farPlane) override;
    void GetViewVolume(S3DCameraHandle handle, float* left, float* right, float* bottom, float* top, int* nearPlane, int* farPlane) override;

    void MakeIdentity(S3DCameraHandle handle) override;
    void LookAt(S3DCameraHandle handle, const cS3DVector3& eye, const cS3DVector3& up) override;
    void GetPosition(S3DCameraHandle handle, cS3DVector3& outPos) override;
    void SetPosition(S3DCameraHandle handle, const cS3DVector3& pos) override;
    void GetLookAt(S3DCameraHandle handle, cS3DVector3& outLookAt) override;
    void GetEyeRay(S3DCameraHandle handle, const cS3DVector2& screenPos, cS3DVector3& outRayOrigin,
    cS3DVector3& outRayDirection) override;
    void SetDepthOffset(S3DCameraHandle handle, float offset) override;
    int GetViewState(S3DCameraHandle handle) override;

    // Lifecycle
    bool Init();
    bool Shutdown();

private:
    struct Thunks {
        cS3DCamera* (__cdecl* create)();
        bool (__thiscall* project)(cS3DCamera*, const cS3DVector3&, cS3DVector3&);
        bool (__thiscall* unProject)(cS3DCamera*, const cS3DVector3&, cS3DVector3&);
        intptr_t (__thiscall* getProjectionMatrix)(cS3DCamera*);
        const cS3DTransform* (__thiscall* getViewTransform)(cS3DCamera*);
        void (__thiscall* updateProjection)(cS3DCamera*);
        void (__thiscall* updateViewTransform)(cS3DCamera*);
        void (__thiscall* setViewport)(cS3DCamera*, float, float);
        void (__thiscall* setViewportOffset)(cS3DCamera*, const cS3DVector2&);
        void (__thiscall* setSubview)(cS3DCamera*, float, float, float, float);
        void (__thiscall* clearSubview)(cS3DCamera*);
        void (__thiscall* setOrtho)(cS3DCamera*, float, float, float, float, float, float);
        void (__thiscall* getViewVolume)(cS3DCamera*, float*, float*, float*, float*, int*, int*);
        int (__thiscall* getProjectionType)(cS3DCamera*);
        int (__thiscall* getProjectionState)(cS3DCamera*);
        void (__thiscall* makeIdentity)(cS3DCamera*);
        void (__thiscall* lookAt)(cS3DCamera*, const cS3DVector3&, const cS3DVector3&);
        void (__thiscall* getPosition)(cS3DCamera*, cS3DVector3&);
        void (__thiscall* setPosition)(cS3DCamera*, const cS3DVector3&);
        void (__thiscall* getLookAt)(cS3DCamera*, cS3DVector3&);
        void (__thiscall* setDepthOffset)(cS3DCamera*, float);
        int (__thiscall* getViewState)(cS3DCamera*);
        void (__thiscall* getEyeRay)(cS3DCamera*, const cS3DVector2&, cS3DVector3&, cS3DVector3&);
        void (__thiscall* destroy)(cS3DCamera*); // Unknown address; may be null
    };

    [[nodiscard]] cS3DCamera* Validate(S3DCameraHandle handle) const;
    S3DCameraHandle WrapRendererCameraInternal();

private:
    Thunks thunks_{};
    uint16_t versionTag_{};
};
