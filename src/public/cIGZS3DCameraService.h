#pragma once

#include "cIGZUnknown.h"
#include "cIS3DModelInstance.h"
#include "cS3DVector2.h"
#include "cS3DVector3.h"

// Opaque handle returned by the camera service. Version tag guards cross-build use.
struct S3DCameraHandle {
    void* ptr;
    uint16_t version;
    bool owned; // true if service created the camera and may destroy it
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZS3DCameraService : public cIGZUnknown {
public:
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0; // kS3DCameraServiceID

    virtual S3DCameraHandle CreateCamera() = 0;
    virtual void DestroyCamera(S3DCameraHandle handle) = 0;
    // Wrap an existing camera allocated by the game/renderer.
    // The service will not destroy wrapped cameras.
    virtual S3DCameraHandle WrapCamera(void* existingCameraPtr) = 0;
    // Convenience: wraps the active renderer camera (if available).
    virtual S3DCameraHandle WrapActiveRendererCamera() = 0;

    virtual bool Project(S3DCameraHandle handle, const cS3DVector3& worldPos, cS3DVector3& screenPos) = 0;
    virtual bool UnProject(S3DCameraHandle handle, const cS3DVector3& screenPos, cS3DVector3& worldPos) = 0;
    // Convenience: world to screen with float inputs and optional depth.
    virtual bool WorldToScreen(S3DCameraHandle handle, float worldX, float worldY, float worldZ,
                               float& screenX, float& screenY, float* depthOut = nullptr) = 0;
    virtual int GetProjectionType(S3DCameraHandle handle) = 0;
    virtual int GetProjectionState(S3DCameraHandle handle) = 0;

    virtual const cS3DTransform* GetViewTransform(S3DCameraHandle handle) = 0;
    virtual intptr_t GetProjectionMatrix(S3DCameraHandle handle) = 0;

    virtual void SetViewport(S3DCameraHandle handle, float width, float height) = 0;
    virtual void SetViewportOffset(S3DCameraHandle handle, const cS3DVector2& offset) = 0;
    virtual void SetSubview(S3DCameraHandle handle, float left, float top, float right, float bottom) = 0;
    virtual void ClearSubview(S3DCameraHandle handle) = 0;
    virtual void SetOrtho(S3DCameraHandle handle, float left, float right, float bottom, float top, float nearPlane,
                          float farPlane) = 0;
    virtual void GetViewVolume(S3DCameraHandle handle, float* left, float* right, float* bottom, float* top,
                               int* nearPlane, int* farPlane) = 0;

    virtual void MakeIdentity(S3DCameraHandle handle) = 0;
    virtual void LookAt(S3DCameraHandle handle, const cS3DVector3& eye, const cS3DVector3& up) = 0;
    virtual void GetPosition(S3DCameraHandle handle, cS3DVector3& outPos) = 0;
    virtual void SetPosition(S3DCameraHandle handle, const cS3DVector3& pos) = 0;
    virtual void GetLookAt(S3DCameraHandle handle, cS3DVector3& outLookAt) = 0;
    virtual void GetEyeRay(S3DCameraHandle handle, const cS3DVector2& screenPos, cS3DVector3& outRayOrigin,
                           cS3DVector3& outRayDirection) = 0;
    virtual void SetDepthOffset(S3DCameraHandle handle, float offset) = 0;
    virtual int GetViewState(S3DCameraHandle handle) = 0;
};
