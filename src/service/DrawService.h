#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include "cRZBaseSystemService.h"
#include "public/cIGZDrawService.h"
#include "utils/VersionDetection.h"

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class DrawService final : public cRZBaseSystemService, public cIGZDrawService {
public:
    DrawService();
    ~DrawService() = default;

    // IUnknown
    uint32_t AddRef() override;
    uint32_t Release() override;
    bool QueryInterface(uint32_t riid, void** ppvObj) override;

    // cIGZDrawService
    [[nodiscard]] uint32_t GetServiceID() const override;
    SC4DrawContextHandle WrapDrawContext(void* existingDrawContextPtr) override;
    SC4DrawContextHandle WrapActiveRendererDrawContext() override;
    uint32_t RendererDraw() override;
    void RendererDrawPreStaticView() override;
    void RendererDrawStaticView() override;
    void RendererDrawPostStaticView() override;
    void RendererDrawPreDynamicView() override;
    void RendererDrawDynamicView() override;
    void RendererDrawPostDynamicView() override;
    bool RegisterDrawPassCallback(DrawServicePass pass, DrawPassCallback callback,
                                  void* userData, uint32_t* outToken) override;
    void UnregisterDrawPassCallback(uint32_t token) override;
    void SetHighlightColor(SC4DrawContextHandle handle, int highlightType,
                           float r, float g, float b, float a) override;
    void SetRenderStateHighlight(SC4DrawContextHandle handle, int highlightType) override;
    void SetRenderStateHighlight(SC4DrawContextHandle handle, const void* material, const void* highlightDesc) override;
    void SetModelTransform(SC4DrawContextHandle handle, const void* transform4x4) override;
    void SetModelTransform(SC4DrawContextHandle handle, float* transform4x4) override;
    void SetModelViewTransformChanged(SC4DrawContextHandle handle, int changed) override;
    void ResetModelViewTransform(SC4DrawContextHandle handle) override;
    void GetModelViewMatrix(SC4DrawContextHandle handle, void* outMatrix4x4) override;
    void SetModelShade(SC4DrawContextHandle handle, void* modelInstance, const float* rgba) override;
    void SetShade(SC4DrawContextHandle handle, const float* rgba) override;
    void SetSelfLitShade(SC4DrawContextHandle handle, void* selfLitShade) override;
    void ResetShade(SC4DrawContextHandle handle) override;
    void SetRenderState(SC4DrawContextHandle handle, void* packedRenderState, void* materialState) override;
    void SetRenderState(SC4DrawContextHandle handle, uint32_t* packedRenderState) override;
    void SetDefaultRenderState(SC4DrawContextHandle handle) override;
    void SetDefaultRenderStateUnilaterally(SC4DrawContextHandle handle) override;
    void SetEmulatedSecondStageRenderState(SC4DrawContextHandle handle) override;
    void RenderMesh(SC4DrawContextHandle handle, void* mesh) override;
    void RenderModelInstance(SC4DrawContextHandle handle, int* modelCount,
                             int* modelList, uint8_t* drawInfo, bool previewOnly) override;

    void SetTexWrapModes(SC4DrawContextHandle handle, int uMode, int vMode, int stage) override;
    void SetTexFiltering(SC4DrawContextHandle handle, int minFilter, int magFilter, int stage) override;
    void SetTexture(SC4DrawContextHandle handle, uint32_t texture, int stage) override;
    void EnableTextureStateFlag(SC4DrawContextHandle handle, bool enable, int stage) override;
    void SetTexColor(SC4DrawContextHandle handle, float r, float g, float b, float a) override;
    void SetTexCombiner(SC4DrawContextHandle handle, void* combinerState, int stage) override;
    void SetTexEnvMode(SC4DrawContextHandle handle, uint32_t envMode, int stage) override;
    void SetTexTransform4(SC4DrawContextHandle handle, void* transform4x4, int stage) override;
    void ClearTexTransform(SC4DrawContextHandle handle, int stage) override;
    void SetTexCoord(SC4DrawContextHandle handle, int texCoord, int stage) override;
    void SetVertexBuffer(SC4DrawContextHandle handle) override;
    void SetIndexBuffer(SC4DrawContextHandle handle, uint32_t indexBuffer, uint32_t indexFormat) override;
    void EnableBlendStateFlag(SC4DrawContextHandle handle, bool enabled) override;
    void EnableAlphaTestFlag(SC4DrawContextHandle handle, bool enabled) override;
    void EnableColorMaskFlag(SC4DrawContextHandle handle, bool enabled) override;
    void EnableCullFaceFlag(SC4DrawContextHandle handle, bool enabled) override;
    void EnableDepthMaskFlag(SC4DrawContextHandle handle, bool enabled) override;
    void EnableDepthTestFlag(SC4DrawContextHandle handle, bool enabled) override;
    void SetBlendFunc(SC4DrawContextHandle handle, uint32_t srcFactor, uint32_t dstFactor) override;
    void SetAlphaFunc(SC4DrawContextHandle handle, uint32_t alphaFunc, float alphaRef) override;
    void SetDepthFunc(SC4DrawContextHandle handle, uint32_t depthFunc) override;
    void SetDepthOffset(SC4DrawContextHandle handle, int depthOffset) override;
    void SetTransparency(SC4DrawContextHandle handle) override;
    void ResetTransparency(SC4DrawContextHandle handle) override;
    bool GetLighting(SC4DrawContextHandle handle) override;
    void SetLighting(SC4DrawContextHandle handle, bool enableLighting) override;
    void SetFog(SC4DrawContextHandle handle, bool enableFog, float* fogColorRgb, float fogStart, float fogEnd) override;
    void SetCamera(SC4DrawContextHandle handle, int camera) override;
    void InitContext(SC4DrawContextHandle handle) override;
    void ShutdownContext(SC4DrawContextHandle handle) override;

    void DrawBoundingBox(SC4DrawContextHandle handle, float* bbox6, float r, float g, float b, float a) override;
    void DrawPrims(SC4DrawContextHandle handle, uint32_t primType, uint32_t startVertex,
                   uint32_t primitiveCount, uint32_t flags) override;
    void DrawPrimsIndexed(SC4DrawContextHandle handle, uint8_t primType, long indexStart, long indexCount) override;
    void DrawPrimsIndexedRaw(SC4DrawContextHandle handle, uint32_t primType, uint32_t indexBuffer,
                             uint32_t indexCount, uint32_t flags) override;
    void DrawRect(SC4DrawContextHandle handle, void* drawTarget, int* rect) override;

    // Lifecycle
    bool Init();
    bool Shutdown();

private:
    static constexpr size_t kHookByteCount = 5;
    static constexpr size_t kCallSitePatchCount = 15;

    struct DrawPassCallbackRegistration {
        uint32_t token = 0;
        DrawServicePass pass = DrawServicePass::PreStatic;
        DrawPassCallback callback = nullptr;
        void* userData = nullptr;
    };

    struct CallSitePatch {
        const char* name = nullptr;
        DrawServicePass pass = DrawServicePass::PreStatic;
        uintptr_t callSiteAddress = 0;
        uintptr_t originalTarget = 0;
        int32_t originalRel = 0;
        void* hookFn = nullptr;
        bool installed = false;
    };

    static void __fastcall HookPreStatic(void* self, void*);
    static void __fastcall HookStatic(void* self, void*);
    static void __fastcall HookPostStatic(void* self, void*);
    static void __fastcall HookPreDynamic(void* self, void*);
    static void __fastcall HookDynamic(void* self, void*);
    static void __fastcall HookPostDynamic(void* self, void*);
    static DrawService* GetActiveInstance();

    static bool ComputeRelativeCallTarget(uintptr_t callSiteAddress, uintptr_t targetAddress, int32_t& relOut);
    bool InstallCallSitePatchLocked_(CallSitePatch& patch);
    void UninstallCallSitePatchLocked_(CallSitePatch& patch);
    bool InstallPassCallSitePatchesLocked_(DrawServicePass pass);
    void UninstallPassCallSitePatchesLocked_(DrawServicePass pass);
    bool IsPassInstalledLocked_(DrawServicePass pass) const;
    uintptr_t GetOriginalTargetForPassLocked_(DrawServicePass pass) const;
    void DispatchDrawPassCallbacksLocked_(DrawServicePass pass, bool begin);
    void OnPassHook(DrawServicePass pass, void* self);
    void UninstallAllPassHooksLocked_();

    struct Thunks {
        void* (__thiscall* getDrawContext)(void* renderer);
        uint32_t (__thiscall* rendererDraw)(void* renderer);
        void (__thiscall* drawPreStaticView)(void* renderer);
        void (__thiscall* drawStaticView)(void* renderer);
        void (__thiscall* drawPostStaticView)(void* renderer);
        void (__thiscall* drawPreDynamicView)(void* renderer);
        void (__thiscall* drawDynamicView)(void* renderer);
        void (__thiscall* drawPostDynamicView)(void* renderer);
        void (__thiscall* setHighlightColor)(void* drawContext, int highlightType, const void* highlightColor);
        void (__thiscall* setRenderStateHighlightSimple)(void* drawContext, int highlightType);
        void (__thiscall* setRenderStateHighlightMaterial)(void* drawContext, const void* material, const void* highlightDesc);
        void (__thiscall* setModelTransform)(void* drawContext, const void* transform4x4);
        void (__thiscall* setModelTransformFloats)(void* drawContext, float* transform4x4);
        void (__thiscall* setModelViewTransformChanged)(void* drawContext, int changed);
        void (__thiscall* resetModelViewTransform)(void* drawContext);
        void (__thiscall* getModelViewMatrix)(void* drawContext, void* outMatrix4x4);
        void (__thiscall* setModelShade)(void* drawContext, void* modelInstance, const float* rgba);
        void (__thiscall* setShade)(void* drawContext, const float* rgba);
        void (__thiscall* setSelfLitShade)(void* drawContext, void* selfLitShade);
        void (__thiscall* resetShade)(void* drawContext);
        void (__thiscall* setRenderState)(void* drawContext, void* packedRenderState, void* materialState);
        void (__thiscall* setRenderStatePacked)(void* drawContext, uint32_t* packedRenderState);
        void (__thiscall* setDefaultRenderState)(void* drawContext);
        void (__thiscall* setDefaultRenderStateUnilaterally)(void* drawContext);
        void (__thiscall* setEmulatedSecondStageRenderState)(void* drawContext);
        void (__thiscall* renderMesh)(void* drawContext, void* mesh);
        void (__thiscall* renderModelInstance)(void* drawContext, int* modelCount,
                                               int* modelList, uint8_t* drawInfo, bool previewOnly);
        void (__thiscall* setTexWrapModes)(void* drawContext, int uMode, int vMode, int stage);
        void (__thiscall* setTexFiltering)(void* drawContext, int minFilter, int magFilter, int stage);
        void (__thiscall* setTexture)(void* drawContext, uint32_t texture, int stage);
        void (__thiscall* enableTextureStateFlag)(void* drawContext, char enable, int stage);
        void (__thiscall* setTexColor)(void* drawContext, const void* color);
        void (__thiscall* setTexCombiner)(void* drawContext, void* combinerState, int stage);
        void (__thiscall* setTexEnvMode)(void* drawContext, uint32_t envMode, int stage);
        void (__thiscall* setTexTransform4)(void* drawContext, void* transform4x4, int stage);
        void (__thiscall* clearTexTransform)(void* drawContext, int stage);
        void (__thiscall* setTexCoord)(void* drawContext, int texCoord, int stage);
        void (__thiscall* setVertexBuffer)(void* drawContext);
        void (__thiscall* setIndexBuffer)(void* drawContext, uint32_t indexBuffer, uint32_t indexFormat);
        void (__thiscall* enableBlendStateFlag)(void* drawContext, char enabled);
        void (__thiscall* enableAlphaTestFlag)(void* drawContext, bool enabled);
        void (__thiscall* enableColorMaskFlag)(void* drawContext, bool enabled);
        void (__thiscall* enableCullFaceFlag)(void* drawContext, char enabled);
        void (__thiscall* enableDepthMaskFlag)(void* drawContext, bool enabled);
        void (__thiscall* enableDepthTestFlag)(void* drawContext, char enabled);
        void (__thiscall* setBlendFunc)(void* drawContext, uint32_t srcFactor, uint32_t dstFactor);
        void (__thiscall* setAlphaFunc)(void* drawContext, uint32_t alphaFunc, float alphaRef);
        void (__thiscall* setDepthFunc)(void* drawContext, uint32_t depthFunc);
        void (__thiscall* setDepthOffset)(void* drawContext, int depthOffset);
        void (__thiscall* setTransparency)(void* drawContext);
        void (__thiscall* resetTransparency)(void* drawContext);
        uint8_t (__thiscall* getLighting)(void* drawContext);
        void (__thiscall* setLighting)(void* drawContext, bool enableLighting);
        void (__thiscall* setFog)(void* drawContext, bool enableFog, float* fogColorRgb, float fogStart, float fogEnd);
        void (__thiscall* setCamera)(void* drawContext, int camera);
        void (__thiscall* initContext)(void* drawContext);
        void (__thiscall* shutdownContext)(void* drawContext);
        void (__thiscall* drawBoundingBox)(void* drawContext, float* bbox, const void* color);
        void (__thiscall* drawPrims)(void* drawContext, uint32_t primType, uint32_t startVertex, uint32_t primitiveCount, uint32_t flags);
        void (__thiscall* drawPrimsIndexed)(void* drawContext, uint8_t primType, long indexStart, long indexCount);
        void (__thiscall* drawPrimsIndexedRaw)(void* drawContext, uint32_t primType, uint32_t indexBuffer, uint32_t indexCount, uint32_t flags);
        void (__thiscall* drawRect)(void* drawContext, void* drawTarget, int* rect);
    };

    [[nodiscard]] void* Validate(SC4DrawContextHandle handle) const;
    [[nodiscard]] void* GetActiveRendererInternal() const;
    SC4DrawContextHandle WrapRendererDrawContextInternal();

private:
    Thunks thunks_{};
    uint16_t versionTag_{};
    static DrawService* activeInstance_;
    std::array<CallSitePatch, kCallSitePatchCount> callSitePatches_{};
    std::vector<DrawPassCallbackRegistration> passCallbacks_{};
    mutable std::mutex mutex_{};
    uint32_t nextCallbackToken_ = 1;
};
