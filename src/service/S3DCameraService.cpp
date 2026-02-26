#include "S3DCameraService.h"

#include <Windows.h>

#include "cISC43DRender.h"
#include "cISC4View3DWin.h"
#include "SC4UI.h"
#include "public/S3DCameraServiceIds.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

S3DCameraService::S3DCameraService()
    : cRZBaseSystemService(kS3DCameraServiceID, 0)
      , versionTag_(VersionDetection::GetInstance().GetGameVersion()) {}

uint32_t S3DCameraService::AddRef() {
    return cRZBaseSystemService::AddRef();
}

uint32_t S3DCameraService::Release() {
    return cRZBaseSystemService::Release();
}

bool S3DCameraService::QueryInterface(const uint32_t riid, void** ppvObj) {
    if (riid == GZIID_cIGZS3DCameraService) {
        *ppvObj = static_cast<cIGZS3DCameraService*>(this);
        AddRef();
        return true;
    }
    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

uint32_t S3DCameraService::GetServiceID() const {
    return kS3DCameraServiceID;
}

S3DCameraHandle S3DCameraService::CreateCamera() {
    static bool warned = false;
    if (!warned) {
        LOG_WARN("S3DCameraService: CreateCamera not supported; use WrapCamera/WrapActiveRendererCamera");
        warned = true;
    }
    return {nullptr, 0, false};
}

void S3DCameraService::DestroyCamera(const S3DCameraHandle handle) {
    cS3DCamera* cam = Validate(handle);
    if (!cam) {
        return;
    }
    if (handle.owned && thunks_.destroy) {
        thunks_.destroy(cam);
    }
    else {
        // Either not owned or destructor unknown; do nothing.
    }
}

S3DCameraHandle S3DCameraService::WrapCamera(void* existingCameraPtr) {
    if (!existingCameraPtr) {
        return {nullptr, 0, false};
    }
    return {existingCameraPtr, versionTag_, false};
}

S3DCameraHandle S3DCameraService::WrapActiveRendererCamera() {
    return WrapRendererCameraInternal();
}

bool S3DCameraService::Project(const S3DCameraHandle handle, const cS3DVector3& worldPos, cS3DVector3& screenPos) {
    auto* cam = Validate(handle);
    return cam && thunks_.project(cam, worldPos, screenPos);
}

bool S3DCameraService::UnProject(const S3DCameraHandle handle, const cS3DVector3& screenPos, cS3DVector3& worldPos) {
    auto* cam = Validate(handle);
    return cam && thunks_.unProject(cam, screenPos, worldPos);
}

bool S3DCameraService::WorldToScreen(const S3DCameraHandle handle, const float worldX, const float worldY,
                                     const float worldZ, float& screenX, float& screenY, float* depthOut) {
    const cS3DVector3 world{worldX, worldY, worldZ};
    cS3DVector3 screen{};
    if (!Project(handle, world, screen)) {
        return false;
    }
    screenX = screen.fX;
    screenY = screen.fY;
    if (depthOut) {
        *depthOut = screen.fZ;
    }
    return true;
}

intptr_t S3DCameraService::GetProjectionMatrix(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    return cam ? thunks_.getProjectionMatrix(cam) : reinterpret_cast<intptr_t>(nullptr);
}

int S3DCameraService::GetProjectionType(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    return cam ? thunks_.getProjectionType(cam) : 0;
}

int S3DCameraService::GetProjectionState(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    return cam ? thunks_.getProjectionState(cam) : 0;
}

const cS3DTransform* S3DCameraService::GetViewTransform(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    return cam ? thunks_.getViewTransform(cam) : nullptr;
}

void S3DCameraService::SetViewport(const S3DCameraHandle handle, const float width, const float height) {
    auto* cam = Validate(handle);
    if (cam) thunks_.setViewport(cam, width, height);
}

void S3DCameraService::SetViewportOffset(const S3DCameraHandle handle, const cS3DVector2& offset) {
    auto* cam = Validate(handle);
    if (cam) thunks_.setViewportOffset(cam, offset);
}

void S3DCameraService::SetSubview(const S3DCameraHandle handle, const float left, const float top, const float right,
                                  const float bottom) {
    auto* cam = Validate(handle);
    if (cam) thunks_.setSubview(cam, left, top, right, bottom);
}

void S3DCameraService::ClearSubview(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    if (cam) thunks_.clearSubview(cam);
}

void S3DCameraService::SetOrtho(const S3DCameraHandle handle, const float left, const float right, const float bottom,
                                const float top, const float nearPlane, const float farPlane) {
    auto* cam = Validate(handle);
    if (cam) thunks_.setOrtho(cam, left, right, bottom, top, nearPlane, farPlane);
}

void S3DCameraService::GetViewVolume(const S3DCameraHandle handle, float* left, float* right, float* bottom, float* top,
                                     int* nearPlane, int* farPlane) {
    auto* cam = Validate(handle);
    if (cam) thunks_.getViewVolume(cam, left, right, bottom, top, nearPlane, farPlane);
}

void S3DCameraService::MakeIdentity(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    if (cam) thunks_.makeIdentity(cam);
}

void S3DCameraService::LookAt(const S3DCameraHandle handle, const cS3DVector3& eye, const cS3DVector3& up) {
    auto* cam = Validate(handle);
    if (cam) thunks_.lookAt(cam, eye, up);
}

void S3DCameraService::GetPosition(const S3DCameraHandle handle, cS3DVector3& outPos) {
    auto* cam = Validate(handle);
    if (cam) thunks_.getPosition(cam, outPos);
}

void S3DCameraService::SetPosition(const S3DCameraHandle handle, const cS3DVector3& pos) {
    auto* cam = Validate(handle);
    if (cam) thunks_.setPosition(cam, pos);
}

void S3DCameraService::GetLookAt(const S3DCameraHandle handle, cS3DVector3& outLookAt) {
    auto* cam = Validate(handle);
    if (cam) thunks_.getLookAt(cam, outLookAt);
}

void S3DCameraService::GetEyeRay(const S3DCameraHandle handle, const cS3DVector2& screenPos, cS3DVector3& outRayOrigin,
                                 cS3DVector3& outRayDirection) {
    auto* cam = Validate(handle);
    if (cam) thunks_.getEyeRay(cam, screenPos, outRayOrigin, outRayDirection);
}

void S3DCameraService::SetDepthOffset(const S3DCameraHandle handle, const float offset) {
    auto* cam = Validate(handle);
    if (cam) thunks_.setDepthOffset(cam, offset);
}

int S3DCameraService::GetViewState(const S3DCameraHandle handle) {
    auto* cam = Validate(handle);
    return cam ? thunks_.getViewState(cam) : -1;
}

bool S3DCameraService::Init() {
    if (versionTag_ != 641) {
        LOG_WARN("S3DCameraService: not registering, game version {} != 641", versionTag_);
        return false;
    }

    thunks_.create = reinterpret_cast<cS3DCamera* (__cdecl*)()>(0x00800320);
    thunks_.project = reinterpret_cast<bool (__thiscall*)(cS3DCamera*, const cS3DVector3&, cS3DVector3&)>(0x007FFF10);
    thunks_.unProject = reinterpret_cast<bool (__thiscall*)(cS3DCamera*, const cS3DVector3&, cS3DVector3&)>(0x00800120);
    thunks_.getProjectionMatrix = reinterpret_cast<intptr_t (__thiscall*)(cS3DCamera*)>(0x007FFEF0);
    thunks_.getViewTransform = reinterpret_cast<const cS3DTransform* (__thiscall*)(cS3DCamera*)>(0x007FFED0);
    thunks_.updateProjection = reinterpret_cast<void (__thiscall*)(cS3DCamera*)>(0x007FF580);
    thunks_.updateViewTransform = reinterpret_cast<void (__thiscall*)(cS3DCamera*)>(0x007FF3D0);
    thunks_.setViewport = reinterpret_cast<void (__thiscall*)(cS3DCamera*, float, float)>(0x007FF150);
    thunks_.setViewportOffset = reinterpret_cast<void (__thiscall*)(cS3DCamera*, const cS3DVector2&)>(0x007FF2C0);
    thunks_.setSubview = reinterpret_cast<void (__thiscall*)(cS3DCamera*, float, float, float, float)>(0x007FF080);
    thunks_.clearSubview = reinterpret_cast<void (__thiscall*)(cS3DCamera*)>(0x007FF0B0);
    thunks_.setOrtho = reinterpret_cast<void (__thiscall*)(cS3DCamera*, float, float, float, float, float, float)>(
        0x007FF2E0);
    thunks_.getViewVolume = reinterpret_cast<void (__thiscall
        *)(cS3DCamera*, float*, float*, float*, float*, int*, int*)>(0x007FF340);
    thunks_.getProjectionType = reinterpret_cast<int (__thiscall*)(cS3DCamera*)>(0x007FF110);
    thunks_.getProjectionState = reinterpret_cast<int (__thiscall*)(cS3DCamera*)>(0x007FF120);
    thunks_.makeIdentity = reinterpret_cast<void (__thiscall*)(cS3DCamera*)>(0x007FF190);
    thunks_.lookAt = reinterpret_cast<void (__thiscall*)(cS3DCamera*, const cS3DVector3&, const cS3DVector3&)>(
        0x007FFD60);
    thunks_.getPosition = reinterpret_cast<void (__thiscall*)(cS3DCamera*, cS3DVector3&)>(0x007FF230);
    thunks_.setPosition = reinterpret_cast<void (__thiscall*)(cS3DCamera*, const cS3DVector3&)>(0x007FF250);
    thunks_.getLookAt = reinterpret_cast<void (__thiscall*)(cS3DCamera*, cS3DVector3&)>(0x007FF280);
    thunks_.getEyeRay = reinterpret_cast<void (__thiscall
        *)(cS3DCamera*, const cS3DVector2&, cS3DVector3&, cS3DVector3&)>(
        0x00800340);
    thunks_.setDepthOffset = reinterpret_cast<void (__thiscall*)(cS3DCamera*, float)>(0x007FF0F0);
    thunks_.getViewState = reinterpret_cast<int (__thiscall*)(cS3DCamera*)>(0x007FF0E0);
    thunks_.destroy = nullptr; // Unknown destructor address

    LOG_INFO("S3DCameraService: bound 641 camera thunks and registered");
    return true;
}

bool S3DCameraService::Shutdown() {
    return true;
}

cS3DCamera* S3DCameraService::Validate(const S3DCameraHandle handle) const {
    if (!handle.ptr || handle.version != versionTag_) {
        return nullptr;
    }
    return static_cast<cS3DCamera*>(handle.ptr);
}

S3DCameraHandle S3DCameraService::WrapRendererCameraInternal() {
    const auto view3DWin = SC4UI::GetView3DWin();
    if (!view3DWin) {
        LOG_WARN("S3DCameraService: WrapActiveRendererCamera failed, View3DWin missing");
        return {nullptr, 0, false};
    }
    cISC43DRender* renderer = view3DWin->GetRenderer();
    if (!renderer) {
        LOG_WARN("S3DCameraService: WrapActiveRendererCamera failed, renderer missing");
        return {nullptr, 0, false};
    }
    cS3DCamera* camera = renderer->GetCamera();
    if (!camera) {
        LOG_WARN("S3DCameraService: WrapActiveRendererCamera failed, camera missing");
        return {nullptr, 0, false};
    }
    return WrapCamera(camera);
}
