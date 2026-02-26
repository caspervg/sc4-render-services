#pragma once

#include <cstdint>

#include "cIGZUnknown.h"

// Unique IDs for the Draw service and its interface.
static constexpr auto kDrawServiceID = 0xD6A70C11;
static constexpr auto GZIID_cIGZDrawService = 0xA43BF2E7;

/// Opaque handle returned by the draw service. Version tag guards cross-build use.
struct SC4DrawContextHandle {
    void* ptr;
    uint16_t version;
};

/// Render pass identifiers used by DrawService callbacks.
enum class DrawServicePass : uint8_t {
    PreStatic = 0,
    Static,
    PostStatic,
    PreDynamic,
    Dynamic,
    PostDynamic
};

/// Callback invoked before and after a render pass.
using DrawPassCallback = void (*)(DrawServicePass pass, bool begin, void* userData);

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
/// Draw service interface.
class cIGZDrawService : public cIGZUnknown {
public:
    /** @name Service Info */
    ///@{
    /// Returns the service ID (kDrawServiceID).
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;
    ///@}

    /** @name Draw Context Helpers */
    ///@{
    // Wrap an existing draw context pointer.
    virtual SC4DrawContextHandle WrapDrawContext(void* existingDrawContextPtr) = 0;
    // Convenience: wraps the active renderer draw context (if available).
    virtual SC4DrawContextHandle WrapActiveRendererDrawContext() = 0;
    // Convenience wrappers for active cSC43DRender passes.
    virtual uint32_t RendererDraw() = 0;
    virtual void RendererDrawPreStaticView() = 0;
    virtual void RendererDrawStaticView() = 0;
    virtual void RendererDrawPostStaticView() = 0;
    virtual void RendererDrawPreDynamicView() = 0;
    virtual void RendererDrawDynamicView() = 0;
    virtual void RendererDrawPostDynamicView() = 0;
    ///@}

    /** @name Render Pass Callbacks */
    ///@{
    /// Registers a callback for a render pass.
    /// The callback is invoked before (begin=true) and after (begin=false) the pass.
    virtual bool RegisterDrawPassCallback(DrawServicePass pass, DrawPassCallback callback,
                                          void* userData, uint32_t* outToken) = 0;
    /// Unregisters a previously registered callback token.
    virtual void UnregisterDrawPassCallback(uint32_t token) = 0;
    ///@}

    /** @name Render State and Drawing */
    ///@{
    virtual void SetHighlightColor(SC4DrawContextHandle handle, int highlightType,
                                   float r, float g, float b, float a) = 0;
    virtual void SetRenderStateHighlight(SC4DrawContextHandle handle, int highlightType) = 0;
    virtual void SetRenderStateHighlight(SC4DrawContextHandle handle, const void* material, const void* highlightDesc) = 0;

    virtual void SetModelTransform(SC4DrawContextHandle handle, const void* transform4x4) = 0;
    virtual void SetModelTransform(SC4DrawContextHandle handle, float* transform4x4) = 0;
    virtual void SetModelViewTransformChanged(SC4DrawContextHandle handle, int changed) = 0;
    virtual void ResetModelViewTransform(SC4DrawContextHandle handle) = 0;
    virtual void GetModelViewMatrix(SC4DrawContextHandle handle, void* outMatrix4x4) = 0;
    virtual void SetModelShade(SC4DrawContextHandle handle, void* modelInstance, const float* rgba) = 0;
    virtual void SetShade(SC4DrawContextHandle handle, const float* rgba) = 0;
    virtual void SetSelfLitShade(SC4DrawContextHandle handle, void* selfLitShade) = 0;
    virtual void ResetShade(SC4DrawContextHandle handle) = 0;
    virtual void SetRenderState(SC4DrawContextHandle handle, void* packedRenderState, void* materialState) = 0;
    virtual void SetRenderState(SC4DrawContextHandle handle, uint32_t* packedRenderState) = 0;
    virtual void SetDefaultRenderState(SC4DrawContextHandle handle) = 0;
    virtual void SetDefaultRenderStateUnilaterally(SC4DrawContextHandle handle) = 0;
    virtual void SetEmulatedSecondStageRenderState(SC4DrawContextHandle handle) = 0;
    virtual void RenderMesh(SC4DrawContextHandle handle, void* mesh) = 0;
    virtual void RenderModelInstance(SC4DrawContextHandle handle, int* modelCount,
                                     int* modelList, uint8_t* drawInfo, bool previewOnly) = 0;

    virtual void SetTexWrapModes(SC4DrawContextHandle handle, int uMode, int vMode, int stage) = 0;
    virtual void SetTexFiltering(SC4DrawContextHandle handle, int minFilter, int magFilter, int stage) = 0;
    virtual void SetTexture(SC4DrawContextHandle handle, uint32_t texture, int stage) = 0;
    virtual void EnableTextureStateFlag(SC4DrawContextHandle handle, bool enable, int stage) = 0;
    virtual void SetTexColor(SC4DrawContextHandle handle, float r, float g, float b, float a) = 0;
    virtual void SetTexCombiner(SC4DrawContextHandle handle, void* combinerState, int stage) = 0;
    virtual void SetTexEnvMode(SC4DrawContextHandle handle, uint32_t envMode, int stage) = 0;
    virtual void SetTexTransform4(SC4DrawContextHandle handle, void* transform4x4, int stage) = 0;
    virtual void ClearTexTransform(SC4DrawContextHandle handle, int stage) = 0;
    virtual void SetTexCoord(SC4DrawContextHandle handle, int texCoord, int stage) = 0;
    virtual void SetVertexBuffer(SC4DrawContextHandle handle) = 0;
    virtual void SetIndexBuffer(SC4DrawContextHandle handle, uint32_t indexBuffer, uint32_t indexFormat) = 0;
    virtual void EnableBlendStateFlag(SC4DrawContextHandle handle, bool enabled) = 0;
    virtual void EnableAlphaTestFlag(SC4DrawContextHandle handle, bool enabled) = 0;
    virtual void EnableColorMaskFlag(SC4DrawContextHandle handle, bool enabled) = 0;
    virtual void EnableCullFaceFlag(SC4DrawContextHandle handle, bool enabled) = 0;
    virtual void EnableDepthMaskFlag(SC4DrawContextHandle handle, bool enabled) = 0;
    virtual void EnableDepthTestFlag(SC4DrawContextHandle handle, bool enabled) = 0;
    virtual void SetBlendFunc(SC4DrawContextHandle handle, uint32_t srcFactor, uint32_t dstFactor) = 0;
    virtual void SetAlphaFunc(SC4DrawContextHandle handle, uint32_t alphaFunc, float alphaRef) = 0;
    virtual void SetDepthFunc(SC4DrawContextHandle handle, uint32_t depthFunc) = 0;
    virtual void SetDepthOffset(SC4DrawContextHandle handle, int depthOffset) = 0;
    virtual void SetTransparency(SC4DrawContextHandle handle) = 0;
    virtual void ResetTransparency(SC4DrawContextHandle handle) = 0;
    virtual bool GetLighting(SC4DrawContextHandle handle) = 0;
    virtual void SetLighting(SC4DrawContextHandle handle, bool enableLighting) = 0;
    virtual void SetFog(SC4DrawContextHandle handle, bool enableFog, float* fogColorRgb, float fogStart, float fogEnd) = 0;
    virtual void SetCamera(SC4DrawContextHandle handle, int camera) = 0;
    virtual void InitContext(SC4DrawContextHandle handle) = 0;
    virtual void ShutdownContext(SC4DrawContextHandle handle) = 0;

    virtual void DrawBoundingBox(SC4DrawContextHandle handle, float* bbox6,
                                 float r, float g, float b, float a) = 0;
    virtual void DrawPrims(SC4DrawContextHandle handle, uint32_t primType, uint32_t startVertex,
                           uint32_t primitiveCount, uint32_t flags) = 0;
    virtual void DrawPrimsIndexed(SC4DrawContextHandle handle, uint8_t primType, long indexStart, long indexCount) = 0;
    virtual void DrawPrimsIndexedRaw(SC4DrawContextHandle handle, uint32_t primType, uint32_t indexBuffer,
                                     uint32_t indexCount, uint32_t flags) = 0;
    virtual void DrawRect(SC4DrawContextHandle handle, void* drawTarget, int* rect) = 0;
    ///@}
};
