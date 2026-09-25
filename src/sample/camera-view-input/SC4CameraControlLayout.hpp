#pragma once

#include "SC4UI.h"
#include "cIGZWin.h"
#include "cISC43DRender.h"
#include "cS3DVector2.h"
#include "cS3DVector3.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

struct SC4Float4
{
    float fX;
    float fY;
    float fZ;
    float fW;
};

// Reconstructed from the Windows x86 641 binary and cross-checked against the
// symbolized Mac x86 binary. The names are intended for practical DLL-side use.
struct SC4CameraControlLayout
{
    void* vtable;                               // 0x000

    uint8_t unk04;                              // 0x004
    uint8_t unk05;                              // 0x005
    uint8_t pad06[2];                           // 0x006

    int32_t refCount;                           // 0x008
    void* camera;                               // 0x00C cS3DCamera*

    cS3DVector3 cameraPlaneOrigin;              // 0x010
    cS3DVector3 baseTargetForRotation;          // 0x01C

    SC4Float4 unk28;                            // 0x028

    cS3DVector3 cameraPosition;                 // 0x038
    cS3DVector3 viewTargetPosition;             // 0x044
    cS3DVector3 viewTargetVelocity;             // 0x050
    cS3DVector3 groundRayDirection;             // 0x05C

    cS3DVector3 cachedViewXformA;               // 0x068
    cS3DVector3 cachedViewXformB;               // 0x074

    float groundPlaneEq[4];                     // 0x080

    float projectedCameraPlaneX;                // 0x090
    float projectedCameraPlaneY;                // 0x094
    float projectedScreenishX;                  // 0x098
    float projectedScreenishY;                  // 0x09C

    float prevProjectedCameraPlaneX;            // 0x0A0
    float prevProjectedCameraPlaneY;            // 0x0A4
    float prevProjectedScreenishX;              // 0x0A8
    float prevProjectedScreenishY;              // 0x0AC

    float viewportOffsetX;                      // 0x0B0
    float viewportOffsetY;                      // 0x0B4

    float zoomStepWorld[5];                     // 0x0B8

    uint32_t unkCC;                             // 0x0CC
    uint32_t unkD0;                             // 0x0D0
    uint32_t unkD4;                             // 0x0D4

    cS3DVector3 boundsA;                        // 0x0D8
    cS3DVector3 boundsB;                        // 0x0E4

    float customMagnification;                  // 0x0F0
    float minZoom;                              // 0x0F4
    float maxZoomLevelWithoutCustom;            // 0x0F8
    float maxZoom;                              // 0x0FC

    int32_t prevZoom;                           // 0x100
    int32_t prevRotation;                       // 0x104
    int32_t zoom;                               // 0x108
    int32_t rotation;                           // 0x10C
    int32_t postZoom;                           // 0x110
    int32_t postRotation;                       // 0x114

    float yaw;                                  // 0x118
    float pitch;                                // 0x11C
    float cameraDistance;                       // 0x120
    float nearClip;                             // 0x124
    float farClip;                              // 0x128

    int32_t viewportWidth;                      // 0x12C
    int32_t viewportHeight;                     // 0x130

    float orthoScale;                           // 0x134
    float invOrthoScale;                        // 0x138

    cS3DVector3 cameraOffset;                   // 0x13C

    uint8_t scrollBoundsEnabled;                // 0x148
    uint8_t pad149[3];                          // 0x149

    cS3DVector2* scrollBoundsBegin;             // 0x14C
    cS3DVector2* scrollBoundsEnd;               // 0x150
    cS3DVector2* scrollBoundsCapacityEnd;       // 0x154

    float citySizeX;                            // 0x158
    float citySizeZ;                            // 0x15C
};

static_assert(offsetof(SC4CameraControlLayout, viewTargetPosition) == 0x044);
static_assert(offsetof(SC4CameraControlLayout, viewTargetVelocity) == 0x050);
static_assert(offsetof(SC4CameraControlLayout, groundRayDirection) == 0x05C);
static_assert(offsetof(SC4CameraControlLayout, customMagnification) == 0x0F0);
static_assert(offsetof(SC4CameraControlLayout, zoom) == 0x108);
static_assert(offsetof(SC4CameraControlLayout, rotation) == 0x10C);
static_assert(offsetof(SC4CameraControlLayout, yaw) == 0x118);
static_assert(offsetof(SC4CameraControlLayout, pitch) == 0x11C);
static_assert(offsetof(SC4CameraControlLayout, cameraOffset) == 0x13C);
static_assert(offsetof(SC4CameraControlLayout, scrollBoundsBegin) == 0x14C);
static_assert(offsetof(SC4CameraControlLayout, citySizeX) == 0x158);
static_assert(sizeof(SC4CameraControlLayout) == 0x160);

namespace SC4CameraControl
{
    constexpr uint32_t kUpdateReasonGeneric = 2;
    constexpr uint32_t kUpdateReasonTarget = 3;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kAngleToleranceDegrees = 0.001f;
    constexpr float kPitchMinDegrees = 1.0f;
    constexpr float kPitchMaxDegrees = 90.0f - kAngleToleranceDegrees;
    constexpr float kPitchMinRadians = kPitchMinDegrees * (kPi / 180.0f);
    constexpr float kPitchMaxRadians = kPitchMaxDegrees * (kPi / 180.0f);
    constexpr size_t kZoomCount = 5;
    constexpr float kDefaultYawRadians = -0.39269909f;
    constexpr float kDefaultPitchRadians[kZoomCount] = {
        0.52359879f,
        0.61086524f,
        0.69813168f,
        0.78539819f,
        0.78539819f
    };

    constexpr uintptr_t kYawAddress1 = 0x00ABCFC4;
    constexpr uintptr_t kYawAddress2 = 0x00ABACB8;
    constexpr uintptr_t kPitchAddress1 = 0x00ABCFD8;
    constexpr uintptr_t kPitchAddress2 = 0x00ABACCC;

    inline std::atomic<bool> gHasQueuedYawPitch{false};
    inline float gQueuedYaw = 0.0f;
    inline float gQueuedPitch = kPitchMinRadians;

    using SetViewTargetPositionFn = bool(__thiscall*)(SC4CameraControlLayout*, const cS3DVector3*);
    using SetViewTargetVelocityFn = bool(__thiscall*)(SC4CameraControlLayout*, const cS3DVector3*);
    using SetCustomMagnificationFn = bool(__thiscall*)(SC4CameraControlLayout*, float);
    using UpdateCameraPositionFn = bool(__thiscall*)(SC4CameraControlLayout*, uint32_t);

    inline float RadToDeg(const float radians)
    {
        return radians * (180.0f / kPi);
    }

    inline float DegToRad(const float degrees)
    {
        return degrees * (kPi / 180.0f);
    }

    // Mirrors the original DLL's deg2rad helper to avoid singular values at 0 and 90 degrees.
    inline float SanitizeAngleDegrees(float degrees)
    {
        if (-kAngleToleranceDegrees < degrees && degrees < kAngleToleranceDegrees) {
            degrees = kAngleToleranceDegrees;
        }
        else if ((90.0f - kAngleToleranceDegrees) < degrees && degrees < (90.0f + kAngleToleranceDegrees)) {
            degrees = 90.0f - kAngleToleranceDegrees;
        }

        return degrees;
    }

    inline float SanitizeYawRadians(const float yaw)
    {
        float normalizedYaw = std::fmod(yaw, 2.0f * kPi);
        if (normalizedYaw <= -kPi) {
            normalizedYaw += 2.0f * kPi;
        }
        else if (normalizedYaw > kPi) {
            normalizedYaw -= 2.0f * kPi;
        }

        return normalizedYaw;
    }

    inline float SanitizePitchRadians(const float pitch)
    {
        const float clampedDegrees = std::clamp(RadToDeg(pitch), kPitchMinDegrees, kPitchMaxDegrees);
        return DegToRad(SanitizeAngleDegrees(clampedDegrees));
    }

    inline int32_t WrapRotation(int32_t rotation)
    {
        rotation %= 4;
        if (rotation < 0) {
            rotation += 4;
        }
        return rotation;
    }

    inline void RebalanceYawAndRotation(float& yaw, int32_t& rotation)
    {
        constexpr float kQuarterTurn = kPi * 0.5f;
        constexpr float kNativeYawAnchor = -kPi * 0.125f;

        const auto quarterTurns = static_cast<int32_t>(std::lround((yaw - kNativeYawAnchor) / kQuarterTurn));
        yaw = SanitizeYawRadians(yaw - (static_cast<float>(quarterTurns) * kQuarterTurn));
        rotation = WrapRotation(rotation + quarterTurns);
    }

    inline void SanitizeAngles(SC4CameraControlLayout& cameraControl)
    {
        cameraControl.yaw = SanitizeYawRadians(cameraControl.yaw);
        cameraControl.pitch = SanitizePitchRadians(cameraControl.pitch);
    }

    inline cISC43DRender* GetActiveRenderer()
    {
        const auto view3DWin = SC4UI::GetView3DWin();
        if (!view3DWin) {
            return nullptr;
        }
        return view3DWin->GetRenderer();
    }

    inline SC4CameraControlLayout* GetActiveCameraControl()
    {
        auto* renderer = GetActiveRenderer();
        if (!renderer) {
            return nullptr;
        }

        return reinterpret_cast<SC4CameraControlLayout*>(renderer->GetCameraControl());
    }

    inline SetViewTargetPositionFn GetSetViewTargetPosition()
    {
        return reinterpret_cast<SetViewTargetPositionFn>(0x007CD810);
    }

    inline SetViewTargetVelocityFn GetSetViewTargetVelocity()
    {
        return reinterpret_cast<SetViewTargetVelocityFn>(0x007CBC60);
    }

    inline SetCustomMagnificationFn GetSetCustomMagnification()
    {
        return reinterpret_cast<SetCustomMagnificationFn>(0x007CD6E0);
    }

    inline UpdateCameraPositionFn GetUpdateCameraPosition()
    {
        return reinterpret_cast<UpdateCameraPositionFn>(0x007CCF80);
    }

    inline size_t GetScrollBoundsPointCount(const SC4CameraControlLayout& cameraControl)
    {
        if (!cameraControl.scrollBoundsBegin || !cameraControl.scrollBoundsEnd ||
            cameraControl.scrollBoundsEnd < cameraControl.scrollBoundsBegin) {
            return 0;
        }

        return static_cast<size_t>(cameraControl.scrollBoundsEnd - cameraControl.scrollBoundsBegin);
    }

    inline bool Refresh(SC4CameraControlLayout& cameraControl, const uint32_t reason = kUpdateReasonGeneric)
    {
        auto thunk = GetUpdateCameraPosition();
        if (!thunk) {
            return false;
        }

        SanitizeAngles(cameraControl);
        return thunk(&cameraControl, reason);
    }

    inline void RequestViewRedraw()
    {
        auto view3DWin = SC4UI::GetView3DWin();
        if (!view3DWin) {
            return;
        }

        if (auto* window = view3DWin->AsIGZWin()) {
            window->InvalidateSelfAndParents();
        }
    }

    inline void OverwriteMemoryFloat(uintptr_t address, float value)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *reinterpret_cast<float*>(address) = value;
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), oldProtect, &ignored);
        }
    }

    inline void ApplyYawOverride(float yaw)
    {
        yaw = SanitizeYawRadians(yaw);

        for (size_t i = 0; i < kZoomCount; i++) {
            OverwriteMemoryFloat(kYawAddress1 + (i * sizeof(float)), yaw);
            OverwriteMemoryFloat(kYawAddress2 + (i * sizeof(float)), yaw);
        }
    }

    inline void ApplyPitchOverride(float pitch)
    {
        pitch = SanitizePitchRadians(pitch);

        for (size_t i = 0; i < kZoomCount; i++) {
            OverwriteMemoryFloat(kPitchAddress1 + (i * sizeof(float)), pitch);
            OverwriteMemoryFloat(kPitchAddress2 + (i * sizeof(float)), pitch);
        }
    }

    inline void ApplyDefaultPitchOverrides()
    {
        for (size_t i = 0; i < kZoomCount; i++) {
            const float pitch = SanitizePitchRadians(kDefaultPitchRadians[i]);
            OverwriteMemoryFloat(kPitchAddress1 + (i * sizeof(float)), pitch);
            OverwriteMemoryFloat(kPitchAddress2 + (i * sizeof(float)), pitch);
        }
    }

    inline float GetDefaultPitchForZoom(const int32_t zoom)
    {
        const auto index = static_cast<size_t>(std::clamp(zoom, 0, static_cast<int32_t>(kZoomCount - 1)));
        return SanitizePitchRadians(kDefaultPitchRadians[index]);
    }

    inline bool SetViewTargetPosition(const cS3DVector3& value)
    {
        auto* cameraControl = GetActiveCameraControl();
        auto thunk = GetSetViewTargetPosition();
        return cameraControl && thunk && thunk(cameraControl, &value);
    }

    inline bool SetViewTargetVelocity(const cS3DVector3& value)
    {
        auto* cameraControl = GetActiveCameraControl();
        auto thunk = GetSetViewTargetVelocity();
        return cameraControl && thunk && thunk(cameraControl, &value);
    }

    inline bool SetCustomMagnification(const float value)
    {
        auto* cameraControl = GetActiveCameraControl();
        if (!cameraControl) {
            return false;
        }

        float magnification = std::clamp(value, 0.001f, 10.0f);
        int32_t zoom = cameraControl->zoom;
        const int32_t minZoom = static_cast<int32_t>(std::floor(cameraControl->minZoom));
        const int32_t maxZoom = static_cast<int32_t>(std::floor(cameraControl->maxZoomLevelWithoutCustom));

        while (magnification > 2.0f && zoom < maxZoom) {
            magnification *= 0.5f;
            ++zoom;
        }

        while (magnification < 0.5f && zoom > minZoom) {
            magnification *= 2.0f;
            --zoom;
        }

        if (zoom != cameraControl->zoom) {
            auto* renderer = GetActiveRenderer();
            if (!renderer || !renderer->SetZoom(zoom)) {
                return false;
            }
        }

        cameraControl = GetActiveCameraControl();
        auto thunk = GetSetCustomMagnification();
        return cameraControl && thunk && thunk(cameraControl, std::clamp(magnification, 0.001f, 10.0f));
    }

    inline bool SetYawPitch(const float yaw, const float pitch)
    {
        auto* renderer = GetActiveRenderer();
        auto* cameraControl = GetActiveCameraControl();
        if (!renderer || !cameraControl) {
            return false;
        }

        float sanitizedYaw = SanitizeYawRadians(yaw);
        const float sanitizedPitch = SanitizePitchRadians(pitch);
        int32_t targetRotation = cameraControl->rotation;

        RebalanceYawAndRotation(sanitizedYaw, targetRotation);

        if (targetRotation != cameraControl->rotation) {
            if (!renderer->SetRotation(targetRotation)) {
                return false;
            }

            cameraControl = GetActiveCameraControl();
            if (!cameraControl) {
                return false;
            }
        }

        ApplyYawOverride(sanitizedYaw);
        ApplyPitchOverride(sanitizedPitch);

        cameraControl->yaw = sanitizedYaw;
        cameraControl->pitch = sanitizedPitch;

        return Refresh(*cameraControl, kUpdateReasonGeneric);
    }

    inline void QueueYawPitch(const float yaw, const float pitch)
    {
        gQueuedYaw = SanitizeYawRadians(yaw);
        gQueuedPitch = SanitizePitchRadians(pitch);
        gHasQueuedYawPitch.store(true, std::memory_order_release);
    }

    inline bool FlushQueuedYawPitch()
    {
        if (!gHasQueuedYawPitch.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }

        return SetYawPitch(gQueuedYaw, gQueuedPitch);
    }

    inline void ClearQueuedYawPitch()
    {
        gQueuedYaw = 0.0f;
        gQueuedPitch = kPitchMinRadians;
        gHasQueuedYawPitch.store(false, std::memory_order_release);
    }

    inline bool ResetToDefaults()
    {
        auto* cameraControl = GetActiveCameraControl();
        if (!cameraControl) {
            ClearQueuedYawPitch();
            return false;
        }

        ClearQueuedYawPitch();

        ApplyYawOverride(kDefaultYawRadians);
        ApplyDefaultPitchOverrides();

        cameraControl->yaw = SanitizeYawRadians(kDefaultYawRadians);
        cameraControl->pitch = GetDefaultPitchForZoom(cameraControl->zoom);

        if (!SetCustomMagnification(1.0f)) {
            return false;
        }

        cameraControl = GetActiveCameraControl();
        return cameraControl && Refresh(*cameraControl, kUpdateReasonGeneric);
    }

    inline bool SetCameraDistance(const float value)
    {
        auto* cameraControl = GetActiveCameraControl();
        if (!cameraControl) {
            return false;
        }

        cameraControl->cameraDistance = (std::max)(1.0f, value);
        return Refresh(*cameraControl, kUpdateReasonGeneric);
    }

    inline bool SetCameraOffset(const cS3DVector3& value)
    {
        auto* cameraControl = GetActiveCameraControl();
        if (!cameraControl) {
            return false;
        }

        cameraControl->cameraOffset = value;
        return Refresh(*cameraControl);
    }

    inline bool SetCitySize(const float x, const float z)
    {
        auto* cameraControl = GetActiveCameraControl();
        if (!cameraControl) {
            return false;
        }

        cameraControl->citySizeX = (std::max)(1.0f, x);
        cameraControl->citySizeZ = (std::max)(1.0f, z);
        return Refresh(*cameraControl);
    }

    inline bool SetScrollBoundsEnabled(const bool enabled)
    {
        auto* cameraControl = GetActiveCameraControl();
        if (!cameraControl) {
            return false;
        }

        cameraControl->scrollBoundsEnabled = enabled ? 1 : 0;
        return Refresh(*cameraControl);
    }

    inline bool SetZoom(const int32_t zoom)
    {
        auto* renderer = GetActiveRenderer();
        return renderer && renderer->SetZoom(zoom);
    }

    inline bool SetRotation(const int32_t rotation)
    {
        auto* renderer = GetActiveRenderer();
        return renderer && renderer->SetRotation(rotation);
    }

    inline bool SetZoomAndRotation(const int32_t zoom, const int32_t rotation)
    {
        auto* renderer = GetActiveRenderer();
        return renderer && renderer->SetZoomAndRotation(zoom, rotation);
    }

    inline bool SetViewportSize(const int32_t width, const int32_t height)
    {
        auto* renderer = GetActiveRenderer();
        if (!renderer) {
            return false;
        }

        const uint32_t safeWidth = static_cast<uint32_t>((std::max)(1, width));
        const uint32_t safeHeight = static_cast<uint32_t>((std::max)(1, height));
        return renderer->SetViewportSize(safeWidth, safeHeight);
    }
}
