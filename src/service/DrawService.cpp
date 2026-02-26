#include "DrawService.h"

#include "cISC43DRender.h"
#include "cISC4View3DWin.h"
#include "SC4UI.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <windows.h>

namespace {
    bool IsValidPass(const DrawServicePass pass) {
        switch (pass) {
        case DrawServicePass::PreStatic:
        case DrawServicePass::Static:
        case DrawServicePass::PostStatic:
        case DrawServicePass::PreDynamic:
        case DrawServicePass::Dynamic:
        case DrawServicePass::PostDynamic:
            return true;
        default:
            return false;
        }
    }

    struct cS3DVector4 {
        float x;
        float y;
        float z;
        float w;
    };
}

DrawService* DrawService::activeInstance_ = nullptr;

DrawService::DrawService()
    : cRZBaseSystemService(kDrawServiceID, 0)
      , versionTag_(VersionDetection::GetInstance().GetGameVersion()) {
    callSitePatches_ = {{
        {"Draw::DrawPreStaticView_ [A]", DrawServicePass::PreStatic, 0x007CB770, 0, 0, reinterpret_cast<void*>(&HookPreStatic), false},
        {"Draw::DrawStaticView_ [A]", DrawServicePass::Static, 0x007CB777, 0, 0, reinterpret_cast<void*>(&HookStatic), false},
        {"Draw::DrawPostStaticView_ [A]", DrawServicePass::PostStatic, 0x007CB77E, 0, 0, reinterpret_cast<void*>(&HookPostStatic), false},
        {"Draw::DrawPreStaticView_ [B]", DrawServicePass::PreStatic, 0x007CB82A, 0, 0, reinterpret_cast<void*>(&HookPreStatic), false},
        {"Draw::DrawStaticView_ [B]", DrawServicePass::Static, 0x007CB831, 0, 0, reinterpret_cast<void*>(&HookStatic), false},
        {"Draw::DrawPostStaticView_ [B]", DrawServicePass::PostStatic, 0x007CB838, 0, 0, reinterpret_cast<void*>(&HookPostStatic), false},
        {"Draw::DrawPreDynamicView_", DrawServicePass::PreDynamic, 0x007CB84C, 0, 0, reinterpret_cast<void*>(&HookPreDynamic), false},
        {"Draw::DrawDynamicView_", DrawServicePass::Dynamic, 0x007CB853, 0, 0, reinterpret_cast<void*>(&HookDynamic), false},
        {"Draw::DrawPostDynamicView_", DrawServicePass::PostDynamic, 0x007CB85A, 0, 0, reinterpret_cast<void*>(&HookPostDynamic), false},
        {"DoTranslatedViewUpdate::DrawPreDynamicView_", DrawServicePass::PreDynamic, 0x007CA964, 0, 0, reinterpret_cast<void*>(&HookPreDynamic), false},
        {"DoTranslatedViewUpdate::DrawDynamicView_", DrawServicePass::Dynamic, 0x007CA96B, 0, 0, reinterpret_cast<void*>(&HookDynamic), false},
        {"DoTranslatedViewUpdate::DrawPostDynamicView_", DrawServicePass::PostDynamic, 0x007CA972, 0, 0, reinterpret_cast<void*>(&HookPostDynamic), false},
        {"UpdateViewportRect::DrawPreStaticView_", DrawServicePass::PreStatic, 0x007C9522, 0, 0, reinterpret_cast<void*>(&HookPreStatic), false},
        {"UpdateViewportRect::DrawStaticView_", DrawServicePass::Static, 0x007C9529, 0, 0, reinterpret_cast<void*>(&HookStatic), false},
        {"UpdateViewportRect::DrawPostStaticView_", DrawServicePass::PostStatic, 0x007C9530, 0, 0, reinterpret_cast<void*>(&HookPostStatic), false},
    }};
    activeInstance_ = this;
}

uint32_t DrawService::AddRef() {
    return cRZBaseSystemService::AddRef();
}

uint32_t DrawService::Release() {
    return cRZBaseSystemService::Release();
}

bool DrawService::QueryInterface(const uint32_t riid, void** ppvObj) {
    if (riid == GZIID_cIGZDrawService) {
        *ppvObj = static_cast<cIGZDrawService*>(this);
        AddRef();
        return true;
    }
    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

uint32_t DrawService::GetServiceID() const {
    return kDrawServiceID;
}

SC4DrawContextHandle DrawService::WrapDrawContext(void* existingDrawContextPtr) {
    if (!existingDrawContextPtr) {
        return {nullptr, 0};
    }
    return {existingDrawContextPtr, versionTag_};
}

SC4DrawContextHandle DrawService::WrapActiveRendererDrawContext() {
    return WrapRendererDrawContextInternal();
}

uint32_t DrawService::RendererDraw() {
    void* renderer = GetActiveRendererInternal();
    return renderer && thunks_.rendererDraw ? thunks_.rendererDraw(renderer) : 0;
}

void DrawService::RendererDrawPreStaticView() {
    void* renderer = GetActiveRendererInternal();
    if (renderer && thunks_.drawPreStaticView) {
        thunks_.drawPreStaticView(renderer);
    }
}

void DrawService::RendererDrawStaticView() {
    void* renderer = GetActiveRendererInternal();
    if (renderer && thunks_.drawStaticView) {
        thunks_.drawStaticView(renderer);
    }
}

void DrawService::RendererDrawPostStaticView() {
    void* renderer = GetActiveRendererInternal();
    if (renderer && thunks_.drawPostStaticView) {
        thunks_.drawPostStaticView(renderer);
    }
}

void DrawService::RendererDrawPreDynamicView() {
    void* renderer = GetActiveRendererInternal();
    if (renderer && thunks_.drawPreDynamicView) {
        thunks_.drawPreDynamicView(renderer);
    }
}

void DrawService::RendererDrawDynamicView() {
    void* renderer = GetActiveRendererInternal();
    if (renderer && thunks_.drawDynamicView) {
        thunks_.drawDynamicView(renderer);
    }
}

void DrawService::RendererDrawPostDynamicView() {
    void* renderer = GetActiveRendererInternal();
    if (renderer && thunks_.drawPostDynamicView) {
        thunks_.drawPostDynamicView(renderer);
    }
}

bool DrawService::RegisterDrawPassCallback(const DrawServicePass pass, DrawPassCallback callback,
                                           void* userData, uint32_t* outToken) {
    if (!callback || !outToken || versionTag_ != 641 || !IsValidPass(pass)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsPassInstalledLocked_(pass) && !InstallPassCallSitePatchesLocked_(pass)) {
        LOG_WARN("DrawService: failed to install hook for draw pass {}", static_cast<int>(pass));
        return false;
    }

    uint32_t token = nextCallbackToken_++;
    if (token == 0) {
        token = nextCallbackToken_++;
    }
    passCallbacks_.push_back({token, pass, callback, userData});
    *outToken = token;
    return true;
}

void DrawService::UnregisterDrawPassCallback(const uint32_t token) {
    if (!token) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(passCallbacks_.begin(), passCallbacks_.end(),
                                 [token](const DrawPassCallbackRegistration& reg) {
                                     return reg.token == token;
                                 });
    if (it == passCallbacks_.end()) {
        return;
    }

    const DrawServicePass pass = it->pass;
    passCallbacks_.erase(it);

    const bool passStillInUse = std::any_of(passCallbacks_.begin(), passCallbacks_.end(),
                                            [pass](const DrawPassCallbackRegistration& reg) {
                                                return reg.pass == pass;
                                            });
    if (!passStillInUse) {
        UninstallPassCallSitePatchesLocked_(pass);
    }
}

void DrawService::SetHighlightColor(const SC4DrawContextHandle handle, const int highlightType,
                                    const float r, const float g, const float b, const float a) {
    auto* drawContext = Validate(handle);
    if (!drawContext || !thunks_.setHighlightColor) {
        return;
    }
    const cS3DVector4 color{r, g, b, a};
    thunks_.setHighlightColor(drawContext, highlightType, &color);
}

void DrawService::SetRenderStateHighlight(const SC4DrawContextHandle handle, const int highlightType) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setRenderStateHighlightSimple) {
        thunks_.setRenderStateHighlightSimple(drawContext, highlightType);
    }
}

void DrawService::SetRenderStateHighlight(const SC4DrawContextHandle handle,
                                          const void* material, const void* highlightDesc) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setRenderStateHighlightMaterial) {
        thunks_.setRenderStateHighlightMaterial(drawContext, material, highlightDesc);
    }
}

void DrawService::SetModelTransform(const SC4DrawContextHandle handle, const void* transform4x4) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setModelTransform) {
        thunks_.setModelTransform(drawContext, transform4x4);
    }
}

void DrawService::SetModelTransform(const SC4DrawContextHandle handle, float* transform4x4) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setModelTransformFloats) {
        thunks_.setModelTransformFloats(drawContext, transform4x4);
    }
}

void DrawService::SetModelViewTransformChanged(const SC4DrawContextHandle handle, const int changed) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setModelViewTransformChanged) {
        thunks_.setModelViewTransformChanged(drawContext, changed);
    }
}

void DrawService::ResetModelViewTransform(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.resetModelViewTransform) {
        thunks_.resetModelViewTransform(drawContext);
    }
}

void DrawService::GetModelViewMatrix(const SC4DrawContextHandle handle, void* outMatrix4x4) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.getModelViewMatrix) {
        thunks_.getModelViewMatrix(drawContext, outMatrix4x4);
    }
}

void DrawService::SetModelShade(const SC4DrawContextHandle handle, void* modelInstance, const float* rgba) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setModelShade) {
        thunks_.setModelShade(drawContext, modelInstance, rgba);
    }
}

void DrawService::SetShade(const SC4DrawContextHandle handle, const float* rgba) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setShade) {
        thunks_.setShade(drawContext, rgba);
    }
}

void DrawService::SetSelfLitShade(const SC4DrawContextHandle handle, void* selfLitShade) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setSelfLitShade) {
        thunks_.setSelfLitShade(drawContext, selfLitShade);
    }
}

void DrawService::ResetShade(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.resetShade) {
        thunks_.resetShade(drawContext);
    }
}

void DrawService::SetRenderState(const SC4DrawContextHandle handle, void* packedRenderState, void* materialState) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setRenderState) {
        thunks_.setRenderState(drawContext, packedRenderState, materialState);
    }
}

void DrawService::SetRenderState(const SC4DrawContextHandle handle, uint32_t* packedRenderState) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setRenderStatePacked) {
        thunks_.setRenderStatePacked(drawContext, packedRenderState);
    }
}

void DrawService::SetDefaultRenderState(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setDefaultRenderState) {
        thunks_.setDefaultRenderState(drawContext);
    }
}

void DrawService::SetDefaultRenderStateUnilaterally(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setDefaultRenderStateUnilaterally) {
        thunks_.setDefaultRenderStateUnilaterally(drawContext);
    }
}

void DrawService::SetEmulatedSecondStageRenderState(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setEmulatedSecondStageRenderState) {
        thunks_.setEmulatedSecondStageRenderState(drawContext);
    }
}

void DrawService::RenderMesh(const SC4DrawContextHandle handle, void* mesh) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.renderMesh) {
        thunks_.renderMesh(drawContext, mesh);
    }
}

void DrawService::RenderModelInstance(const SC4DrawContextHandle handle, int* modelCount,
                                      int* modelList, uint8_t* drawInfo, const bool previewOnly) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.renderModelInstance) {
        thunks_.renderModelInstance(drawContext, modelCount, modelList, drawInfo, previewOnly);
    }
}

void DrawService::SetTexWrapModes(const SC4DrawContextHandle handle, const int uMode, const int vMode, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexWrapModes) {
        thunks_.setTexWrapModes(drawContext, uMode, vMode, stage);
    }
}

void DrawService::SetTexFiltering(const SC4DrawContextHandle handle, const int minFilter, const int magFilter, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexFiltering) {
        thunks_.setTexFiltering(drawContext, minFilter, magFilter, stage);
    }
}

void DrawService::SetTexture(const SC4DrawContextHandle handle, const uint32_t texture, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexture) {
        thunks_.setTexture(drawContext, texture, stage);
    }
}

void DrawService::EnableTextureStateFlag(const SC4DrawContextHandle handle, const bool enable, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableTextureStateFlag) {
        thunks_.enableTextureStateFlag(drawContext, enable ? 1 : 0, stage);
    }
}

void DrawService::SetTexColor(const SC4DrawContextHandle handle, const float r, const float g, const float b, const float a) {
    auto* drawContext = Validate(handle);
    if (!drawContext || !thunks_.setTexColor) {
        return;
    }
    const cS3DVector4 color{r, g, b, a};
    thunks_.setTexColor(drawContext, &color);
}

void DrawService::SetTexCombiner(const SC4DrawContextHandle handle, void* combinerState, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexCombiner) {
        thunks_.setTexCombiner(drawContext, combinerState, stage);
    }
}

void DrawService::SetTexEnvMode(const SC4DrawContextHandle handle, const uint32_t envMode, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexEnvMode) {
        thunks_.setTexEnvMode(drawContext, envMode, stage);
    }
}

void DrawService::SetTexTransform4(const SC4DrawContextHandle handle, void* transform4x4, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexTransform4) {
        thunks_.setTexTransform4(drawContext, transform4x4, stage);
    }
}

void DrawService::ClearTexTransform(const SC4DrawContextHandle handle, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.clearTexTransform) {
        thunks_.clearTexTransform(drawContext, stage);
    }
}

void DrawService::SetTexCoord(const SC4DrawContextHandle handle, const int texCoord, const int stage) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTexCoord) {
        thunks_.setTexCoord(drawContext, texCoord, stage);
    }
}

void DrawService::SetVertexBuffer(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setVertexBuffer) {
        thunks_.setVertexBuffer(drawContext);
    }
}

void DrawService::SetIndexBuffer(const SC4DrawContextHandle handle, const uint32_t indexBuffer, const uint32_t indexFormat) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setIndexBuffer) {
        thunks_.setIndexBuffer(drawContext, indexBuffer, indexFormat);
    }
}

void DrawService::EnableBlendStateFlag(const SC4DrawContextHandle handle, const bool enabled) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableBlendStateFlag) {
        thunks_.enableBlendStateFlag(drawContext, enabled ? 1 : 0);
    }
}

void DrawService::EnableAlphaTestFlag(const SC4DrawContextHandle handle, const bool enabled) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableAlphaTestFlag) {
        thunks_.enableAlphaTestFlag(drawContext, enabled);
    }
}

void DrawService::EnableColorMaskFlag(const SC4DrawContextHandle handle, const bool enabled) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableColorMaskFlag) {
        thunks_.enableColorMaskFlag(drawContext, enabled);
    }
}

void DrawService::EnableCullFaceFlag(const SC4DrawContextHandle handle, const bool enabled) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableCullFaceFlag) {
        thunks_.enableCullFaceFlag(drawContext, enabled ? 1 : 0);
    }
}

void DrawService::EnableDepthMaskFlag(const SC4DrawContextHandle handle, const bool enabled) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableDepthMaskFlag) {
        thunks_.enableDepthMaskFlag(drawContext, enabled);
    }
}

void DrawService::EnableDepthTestFlag(const SC4DrawContextHandle handle, const bool enabled) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.enableDepthTestFlag) {
        thunks_.enableDepthTestFlag(drawContext, enabled ? 1 : 0);
    }
}

void DrawService::SetBlendFunc(const SC4DrawContextHandle handle, const uint32_t srcFactor, const uint32_t dstFactor) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setBlendFunc) {
        thunks_.setBlendFunc(drawContext, srcFactor, dstFactor);
    }
}

void DrawService::SetAlphaFunc(const SC4DrawContextHandle handle, const uint32_t alphaFunc, const float alphaRef) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setAlphaFunc) {
        thunks_.setAlphaFunc(drawContext, alphaFunc, alphaRef);
    }
}

void DrawService::SetDepthFunc(const SC4DrawContextHandle handle, const uint32_t depthFunc) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setDepthFunc) {
        thunks_.setDepthFunc(drawContext, depthFunc);
    }
}

void DrawService::SetDepthOffset(const SC4DrawContextHandle handle, const int depthOffset) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setDepthOffset) {
        thunks_.setDepthOffset(drawContext, depthOffset);
    }
}

void DrawService::SetTransparency(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setTransparency) {
        thunks_.setTransparency(drawContext);
    }
}

void DrawService::ResetTransparency(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.resetTransparency) {
        thunks_.resetTransparency(drawContext);
    }
}

bool DrawService::GetLighting(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    return drawContext && thunks_.getLighting ? thunks_.getLighting(drawContext) != 0 : false;
}

void DrawService::SetLighting(const SC4DrawContextHandle handle, const bool enableLighting) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setLighting) {
        thunks_.setLighting(drawContext, enableLighting);
    }
}

void DrawService::SetFog(const SC4DrawContextHandle handle, const bool enableFog, float* fogColorRgb,
                         const float fogStart, const float fogEnd) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setFog) {
        thunks_.setFog(drawContext, enableFog, fogColorRgb, fogStart, fogEnd);
    }
}

void DrawService::SetCamera(const SC4DrawContextHandle handle, const int camera) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.setCamera) {
        thunks_.setCamera(drawContext, camera);
    }
}

void DrawService::InitContext(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.initContext) {
        thunks_.initContext(drawContext);
    }
}

void DrawService::ShutdownContext(const SC4DrawContextHandle handle) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.shutdownContext) {
        thunks_.shutdownContext(drawContext);
    }
}

void DrawService::DrawBoundingBox(const SC4DrawContextHandle handle, float* bbox6,
                                  const float r, const float g, const float b, const float a) {
    auto* drawContext = Validate(handle);
    if (!drawContext || !thunks_.drawBoundingBox) {
        return;
    }
    const cS3DVector4 color{r, g, b, a};
    thunks_.drawBoundingBox(drawContext, bbox6, &color);
}

void DrawService::DrawPrims(const SC4DrawContextHandle handle, const uint32_t primType, const uint32_t startVertex,
                            const uint32_t primitiveCount, const uint32_t flags) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.drawPrims) {
        thunks_.drawPrims(drawContext, primType, startVertex, primitiveCount, flags);
    }
}

void DrawService::DrawPrimsIndexed(const SC4DrawContextHandle handle, const uint8_t primType,
                                   const long indexStart, const long indexCount) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.drawPrimsIndexed) {
        thunks_.drawPrimsIndexed(drawContext, primType, indexStart, indexCount);
    }
}

void DrawService::DrawPrimsIndexedRaw(const SC4DrawContextHandle handle, const uint32_t primType,
                                      const uint32_t indexBuffer, const uint32_t indexCount, const uint32_t flags) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.drawPrimsIndexedRaw) {
        thunks_.drawPrimsIndexedRaw(drawContext, primType, indexBuffer, indexCount, flags);
    }
}

void DrawService::DrawRect(const SC4DrawContextHandle handle, void* drawTarget, int* rect) {
    auto* drawContext = Validate(handle);
    if (drawContext && thunks_.drawRect) {
        thunks_.drawRect(drawContext, drawTarget, rect);
    }
}

void __fastcall DrawService::HookPreStatic(void* self, void*) {
    if (auto* service = GetActiveInstance()) {
        service->OnPassHook(DrawServicePass::PreStatic, self);
    }
}

void __fastcall DrawService::HookStatic(void* self, void*) {
    if (auto* service = GetActiveInstance()) {
        service->OnPassHook(DrawServicePass::Static, self);
    }
}

void __fastcall DrawService::HookPostStatic(void* self, void*) {
    if (auto* service = GetActiveInstance()) {
        service->OnPassHook(DrawServicePass::PostStatic, self);
    }
}

void __fastcall DrawService::HookPreDynamic(void* self, void*) {
    if (auto* service = GetActiveInstance()) {
        service->OnPassHook(DrawServicePass::PreDynamic, self);
    }
}

void __fastcall DrawService::HookDynamic(void* self, void*) {
    if (auto* service = GetActiveInstance()) {
        service->OnPassHook(DrawServicePass::Dynamic, self);
    }
}

void __fastcall DrawService::HookPostDynamic(void* self, void*) {
    if (auto* service = GetActiveInstance()) {
        service->OnPassHook(DrawServicePass::PostDynamic, self);
    }
}

DrawService* DrawService::GetActiveInstance() {
    return activeInstance_;
}

bool DrawService::ComputeRelativeCallTarget(const uintptr_t callSiteAddress, const uintptr_t targetAddress, int32_t& relOut) {
    const auto delta = static_cast<intptr_t>(targetAddress) - static_cast<intptr_t>(callSiteAddress + kHookByteCount);
    if (delta < static_cast<intptr_t>(INT32_MIN) || delta > static_cast<intptr_t>(INT32_MAX)) {
        return false;
    }

    relOut = static_cast<int32_t>(delta);
    return true;
}

bool DrawService::InstallCallSitePatchLocked_(CallSitePatch& patch) {
    if (patch.installed) {
        return true;
    }

    auto* site = reinterpret_cast<uint8_t*>(patch.callSiteAddress);
    if (site[0] != 0xE8) {
        LOG_ERROR("DrawService: expected CALL rel32 at 0x{:08X} for {}",
                  static_cast<uint32_t>(patch.callSiteAddress),
                  patch.name);
        return false;
    }

    std::memcpy(&patch.originalRel, site + 1, sizeof(patch.originalRel));
    patch.originalTarget = patch.callSiteAddress + kHookByteCount + patch.originalRel;

    int32_t newRel = 0;
    if (!ComputeRelativeCallTarget(patch.callSiteAddress, reinterpret_cast<uintptr_t>(patch.hookFn), newRel)) {
        LOG_ERROR("DrawService: rel32 range failure for {}", patch.name);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(site + 1, sizeof(newRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("DrawService: VirtualProtect failed for {}", patch.name);
        return false;
    }

    std::memcpy(site + 1, &newRel, sizeof(newRel));
    FlushInstructionCache(GetCurrentProcess(), site, kHookByteCount);
    VirtualProtect(site + 1, sizeof(newRel), oldProtect, &oldProtect);

    patch.installed = true;
    return true;
}

void DrawService::UninstallCallSitePatchLocked_(CallSitePatch& patch) {
    if (!patch.installed) {
        return;
    }

    auto* site = reinterpret_cast<uint8_t*>(patch.callSiteAddress);
    DWORD oldProtect = 0;
    if (!VirtualProtect(site + 1, sizeof(patch.originalRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("DrawService: VirtualProtect failed while uninstalling {}", patch.name);
        return;
    }

    std::memcpy(site + 1, &patch.originalRel, sizeof(patch.originalRel));
    FlushInstructionCache(GetCurrentProcess(), site, kHookByteCount);
    VirtualProtect(site + 1, sizeof(patch.originalRel), oldProtect, &oldProtect);

    patch.installed = false;
    patch.originalTarget = 0;
    patch.originalRel = 0;
}

bool DrawService::InstallPassCallSitePatchesLocked_(const DrawServicePass pass) {
    for (auto& patch : callSitePatches_) {
        if (patch.pass != pass) {
            continue;
        }
        if (!InstallCallSitePatchLocked_(patch)) {
            for (auto& rollbackPatch : callSitePatches_) {
                if (rollbackPatch.pass == pass) {
                    UninstallCallSitePatchLocked_(rollbackPatch);
                }
            }
            return false;
        }
    }
    return true;
}

void DrawService::UninstallPassCallSitePatchesLocked_(const DrawServicePass pass) {
    for (auto& patch : callSitePatches_) {
        if (patch.pass == pass) {
            UninstallCallSitePatchLocked_(patch);
        }
    }
}

bool DrawService::IsPassInstalledLocked_(const DrawServicePass pass) const {
    bool found = false;
    for (const auto& patch : callSitePatches_) {
        if (patch.pass != pass) {
            continue;
        }
        found = true;
        if (!patch.installed) {
            return false;
        }
    }
    return found;
}

uintptr_t DrawService::GetOriginalTargetForPassLocked_(const DrawServicePass pass) const {
    for (const auto& patch : callSitePatches_) {
        if (patch.pass == pass && patch.installed) {
            return patch.originalTarget;
        }
    }
    return 0;
}

void DrawService::DispatchDrawPassCallbacksLocked_(const DrawServicePass pass, const bool begin) {
    for (const auto& reg : passCallbacks_) {
        if (reg.pass == pass) {
            reg.callback(pass, begin, reg.userData);
        }
    }
}

void DrawService::OnPassHook(const DrawServicePass pass, void* self) {
    uintptr_t originalTarget = 0;
    std::vector<DrawPassCallbackRegistration> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        originalTarget = GetOriginalTargetForPassLocked_(pass);
        callbacks.reserve(passCallbacks_.size());
        for (const auto& reg : passCallbacks_) {
            if (reg.pass == pass && reg.callback) {
                callbacks.push_back(reg);
            }
        }
    }
    for (const auto& reg : callbacks) {
        reg.callback(pass, true, reg.userData);
    }
    if (originalTarget) {
        const auto fn = reinterpret_cast<void(__thiscall*)(void*)>(originalTarget);
        fn(self);
    }
    for (const auto& reg : callbacks) {
        reg.callback(pass, false, reg.userData);
    }
}

void DrawService::UninstallAllPassHooksLocked_() {
    for (auto& patch : callSitePatches_) {
        UninstallCallSitePatchLocked_(patch);
    }
}

bool DrawService::Init() {
    if (versionTag_ != 641) {
        LOG_WARN("DrawService: not registering, game version {} != 641", versionTag_);
        return false;
    }

    // Ghidra (SC4 Windows 641)
    thunks_.getDrawContext = reinterpret_cast<void* (__thiscall*)(void*)>(0x004E82A0);
    thunks_.rendererDraw = reinterpret_cast<uint32_t (__thiscall*)(void*)>(0x007CB530);
    thunks_.drawPreStaticView = reinterpret_cast<void (__thiscall*)(void*)>(0x007C3E90);
    thunks_.drawStaticView = reinterpret_cast<void (__thiscall*)(void*)>(0x007C7370);
    thunks_.drawPostStaticView = reinterpret_cast<void (__thiscall*)(void*)>(0x007C3ED0);
    thunks_.drawPreDynamicView = reinterpret_cast<void (__thiscall*)(void*)>(0x007C3E10);
    thunks_.drawDynamicView = reinterpret_cast<void (__thiscall*)(void*)>(0x007C7830);
    thunks_.drawPostDynamicView = reinterpret_cast<void (__thiscall*)(void*)>(0x007C3E50);
    thunks_.setHighlightColor = reinterpret_cast<void (__thiscall*)(void*, int, const void*)>(0x007D4F80);
    thunks_.setRenderStateHighlightSimple = reinterpret_cast<void (__thiscall*)(void*, int)>(0x007D4FB0);
    thunks_.setRenderStateHighlightMaterial = reinterpret_cast<void (__thiscall*)(void*, const void*, const void*)>(0x007D5670);
    thunks_.setModelTransform = reinterpret_cast<void (__thiscall*)(void*, const void*)>(0x007D3400);
    thunks_.setModelTransformFloats = reinterpret_cast<void (__thiscall*)(void*, float*)>(0x007D4B80);
    thunks_.setModelViewTransformChanged = reinterpret_cast<void (__thiscall*)(void*, int)>(0x007D2750);
    thunks_.resetModelViewTransform = reinterpret_cast<void (__fastcall*)(void*)>(0x007D2D20);
    thunks_.getModelViewMatrix = reinterpret_cast<void (__thiscall*)(void*, void*)>(0x007D2730);
    thunks_.setModelShade = reinterpret_cast<void (__thiscall*)(void*, void*, const float*)>(0x007D31E0);
    thunks_.setShade = reinterpret_cast<void (__thiscall*)(void*, const float*)>(0x007D3140);
    thunks_.setSelfLitShade = reinterpret_cast<void (__thiscall*)(void*, void*)>(0x007D31B0);
    thunks_.resetShade = reinterpret_cast<void (__fastcall*)(void*)>(0x007D3290);
    thunks_.setRenderState = reinterpret_cast<void (__thiscall*)(void*, void*, void*)>(0x007D5310);
    thunks_.setRenderStatePacked = reinterpret_cast<void (__thiscall*)(void*, uint32_t*)>(0x007D4530);
    thunks_.setDefaultRenderState = reinterpret_cast<void (__fastcall*)(void*)>(0x007D5200);
    thunks_.setDefaultRenderStateUnilaterally = reinterpret_cast<void (__fastcall*)(void*)>(0x007D5230);
    thunks_.setEmulatedSecondStageRenderState = reinterpret_cast<void (__thiscall*)(void*)>(0x007D47E0);
    thunks_.renderMesh = reinterpret_cast<void (__thiscall*)(void*, void*)>(0x007D4A80);
    thunks_.renderModelInstance = reinterpret_cast<void (__thiscall*)(void*, int*, int*, uint8_t*, bool)>(0x007D5710);
    thunks_.setTexWrapModes = reinterpret_cast<void (__thiscall*)(void*, int, int, int)>(0x007D41E0);
    thunks_.setTexFiltering = reinterpret_cast<void (__thiscall*)(void*, int, int, int)>(0x007D4280);
    thunks_.setTexture = reinterpret_cast<void (__thiscall*)(void*, uint32_t, int)>(0x007D4070);
    thunks_.enableTextureStateFlag = reinterpret_cast<void (__thiscall*)(void*, char, int)>(0x007D3EF0);
    thunks_.setTexColor = reinterpret_cast<void (__thiscall*)(void*, const void*)>(0x007D30B0);
    thunks_.setTexCombiner = reinterpret_cast<void (__thiscall*)(void*, void*, int)>(0x007D3030);
    thunks_.setTexEnvMode = reinterpret_cast<void (__thiscall*)(void*, uint32_t, int)>(0x007D40B0);
    thunks_.setTexTransform4 = reinterpret_cast<void (__thiscall*)(void*, void*, int)>(0x007D4340);
    thunks_.clearTexTransform = reinterpret_cast<void (__thiscall*)(void*, int)>(0x007D4420);
    thunks_.setTexCoord = reinterpret_cast<void (__thiscall*)(void*, int, int)>(0x007D4180);
    thunks_.setVertexBuffer = reinterpret_cast<void (__fastcall*)(void*)>(0x007D2970);
    thunks_.setIndexBuffer = reinterpret_cast<void (__thiscall*)(void*, uint32_t, uint32_t)>(0x007D2980);
    thunks_.enableBlendStateFlag = reinterpret_cast<void (__thiscall*)(void*, char)>(0x007D4010);
    thunks_.enableAlphaTestFlag = reinterpret_cast<void (__thiscall*)(void*, bool)>(0x007D3E90);
    thunks_.enableColorMaskFlag = reinterpret_cast<void (__thiscall*)(void*, bool)>(0x007D2760);
    thunks_.enableCullFaceFlag = reinterpret_cast<void (__thiscall*)(void*, char)>(0x007D2850);
    thunks_.enableDepthMaskFlag = reinterpret_cast<void (__thiscall*)(void*, bool)>(0x007D2800);
    thunks_.enableDepthTestFlag = reinterpret_cast<void (__thiscall*)(void*, char)>(0x007D27B0);
    thunks_.setBlendFunc = reinterpret_cast<void (__thiscall*)(void*, uint32_t, uint32_t)>(0x007D28F0);
    thunks_.setAlphaFunc = reinterpret_cast<void (__thiscall*)(void*, uint32_t, float)>(0x007D2F50);
    thunks_.setDepthFunc = reinterpret_cast<void (__thiscall*)(void*, uint32_t)>(0x007D28A0);
    thunks_.setDepthOffset = reinterpret_cast<void (__thiscall*)(void*, int)>(0x007D4480);
    thunks_.setTransparency = reinterpret_cast<void (__fastcall*)(void*)>(0x007D2A20);
    thunks_.resetTransparency = reinterpret_cast<void (__fastcall*)(void*)>(0x007D2A30);
    thunks_.getLighting = reinterpret_cast<uint8_t (__fastcall*)(void*)>(0x007D2A10);
    thunks_.setLighting = reinterpret_cast<void (__thiscall*)(void*, bool)>(0x007D30D0);
    thunks_.setFog = reinterpret_cast<void (__thiscall*)(void*, bool, float*, float, float)>(0x007D32D0);
    thunks_.setCamera = reinterpret_cast<void (__thiscall*)(void*, int)>(0x007D3D80);
    thunks_.initContext = reinterpret_cast<void (__fastcall*)(void*)>(0x007D6320);
    thunks_.shutdownContext = reinterpret_cast<void (__thiscall*)(void*)>(0x007D2720);
    thunks_.drawBoundingBox = reinterpret_cast<void (__thiscall*)(void*, float*, const void*)>(0x007D5030);
    thunks_.drawPrims = reinterpret_cast<void (__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(0x007D2990);
    thunks_.drawPrimsIndexed = reinterpret_cast<void (__thiscall*)(void*, uint8_t, long, long)>(0x007D29C0);
    thunks_.drawPrimsIndexedRaw = reinterpret_cast<void (__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(0x007D29F0);
    thunks_.drawRect = reinterpret_cast<void (__thiscall*)(void*, void*, int*)>(0x00735720);

    LOG_INFO("DrawService: bound 641 draw-context thunks and registered");
    return true;
}

bool DrawService::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        passCallbacks_.clear();
        UninstallAllPassHooksLocked_();
    }
    if (activeInstance_ == this) {
        activeInstance_ = nullptr;
    }
    return true;
}

void* DrawService::Validate(const SC4DrawContextHandle handle) const {
    if (!handle.ptr || handle.version != versionTag_) {
        return nullptr;
    }
    return handle.ptr;
}

void* DrawService::GetActiveRendererInternal() const {
    const auto view3DWin = SC4UI::GetView3DWin();
    if (!view3DWin) {
        return nullptr;
    }
    return view3DWin->GetRenderer();
}

SC4DrawContextHandle DrawService::WrapRendererDrawContextInternal() {
    const auto view3DWin = SC4UI::GetView3DWin();
    if (!view3DWin) {
        LOG_WARN("DrawService: WrapActiveRendererDrawContext failed, View3DWin missing");
        return {nullptr, 0};
    }

    cISC43DRender* renderer = view3DWin->GetRenderer();
    if (!renderer) {
        LOG_WARN("DrawService: WrapActiveRendererDrawContext failed, renderer missing");
        return {nullptr, 0};
    }

    void* drawContext = nullptr;
    if (thunks_.getDrawContext) {
        drawContext = thunks_.getDrawContext(renderer);
    }
    if (!drawContext) {
        LOG_WARN("DrawService: WrapActiveRendererDrawContext failed, draw context missing");
        return {nullptr, 0};
    }

    return WrapDrawContext(drawContext);
}
