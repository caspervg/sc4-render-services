#pragma once

#include "cIGZUnknown.h"
#include "cIS3DModelInstance.h"
#include "cS3DVector2.h"
#include "cS3DVector3.h"

/// Opaque handle returned by the camera service. Version tag guards cross-build use.
struct S3DCameraHandle {
    void* ptr;
    uint16_t version;
    bool owned; // true if service created the camera and may destroy it
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
/// S3D camera service interface.
class cIGZS3DCameraService : public cIGZUnknown {
public:
    /// Returns the service ID (kS3DCameraServiceID).
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;

    /// Not supported by the built-in service; returns a null handle.
    virtual S3DCameraHandle CreateCamera() = 0;
    /// Destroys a camera created by the service. No-op for wrapped cameras.
    virtual void DestroyCamera(S3DCameraHandle handle) = 0;
    /// Wrap an existing camera allocated by the game/renderer.
    /// The service will not destroy wrapped cameras.
    virtual S3DCameraHandle WrapCamera(void* existingCameraPtr) = 0;
    /// Convenience: wraps the active renderer camera (if available).
    virtual S3DCameraHandle WrapActiveRendererCamera() = 0;

    /// Projects a world position into screen space.
    virtual bool Project(S3DCameraHandle handle, const cS3DVector3& worldPos, cS3DVector3& screenPos) = 0;
    /// Unprojects a screen position into world space.
    virtual bool UnProject(S3DCameraHandle handle, const cS3DVector3& screenPos, cS3DVector3& worldPos) = 0;
    /// Convenience: world to screen with float inputs and optional depth.
    virtual bool WorldToScreen(S3DCameraHandle handle, float worldX, float worldY, float worldZ,
                               float& screenX, float& screenY, float* depthOut = nullptr) = 0;
    /// Returns the projection type for the camera.
    virtual int GetProjectionType(S3DCameraHandle handle) = 0;
    /// Returns the projection state for the camera.
    virtual int GetProjectionState(S3DCameraHandle handle) = 0;

    /// Returns the view transform for the camera.
    virtual const cS3DTransform* GetViewTransform(S3DCameraHandle handle) = 0;
    /// Returns a pointer to the projection matrix.
    virtual intptr_t GetProjectionMatrix(S3DCameraHandle handle) = 0;

    /// Sets the viewport size.
    virtual void SetViewport(S3DCameraHandle handle, float width, float height) = 0;
    /// Sets the viewport offset.
    virtual void SetViewportOffset(S3DCameraHandle handle, const cS3DVector2& offset) = 0;
    /// Sets a sub-viewport rectangle.
    virtual void SetSubview(S3DCameraHandle handle, float left, float top, float right, float bottom) = 0;
    /// Clears the sub-viewport rectangle.
    virtual void ClearSubview(S3DCameraHandle handle) = 0;
    /// Sets an orthographic projection.
    virtual void SetOrtho(S3DCameraHandle handle, float left, float right, float bottom, float top, float nearPlane,
                          float farPlane) = 0;
    /// Gets the view volume.
    virtual void GetViewVolume(S3DCameraHandle handle, float* left, float* right, float* bottom, float* top,
                               int* nearPlane, int* farPlane) = 0;

    /// Resets the camera transform to identity.
    virtual void MakeIdentity(S3DCameraHandle handle) = 0;
    /// Sets the camera look-at orientation.
    virtual void LookAt(S3DCameraHandle handle, const cS3DVector3& eye, const cS3DVector3& up) = 0;
    /// Gets the camera position.
    virtual void GetPosition(S3DCameraHandle handle, cS3DVector3& outPos) = 0;
    /// Sets the camera position.
    virtual void SetPosition(S3DCameraHandle handle, const cS3DVector3& pos) = 0;
    /// Gets the camera look-at vector.
    virtual void GetLookAt(S3DCameraHandle handle, cS3DVector3& outLookAt) = 0;
    /// Returns the eye ray for a screen position.
    virtual void GetEyeRay(S3DCameraHandle handle, const cS3DVector2& screenPos, cS3DVector3& outRayOrigin,
                           cS3DVector3& outRayDirection) = 0;
    /// Sets the camera depth offset.
    virtual void SetDepthOffset(S3DCameraHandle handle, float offset) = 0;
    /// Returns the view state for the camera.
    virtual int GetViewState(S3DCameraHandle handle) = 0;
};
