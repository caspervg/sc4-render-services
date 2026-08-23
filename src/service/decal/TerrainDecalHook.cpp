#include "TerrainDecalHook.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <d3d.h>
#include <ddraw.h>

#include "GraphicsInterfaceHook.h"
#include "GZServPtrs.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

namespace TerrainDecal
{
    TerrainDecalHook* TerrainDecalHook::sActiveHook_ = nullptr;

    namespace
    {
        constexpr std::ptrdiff_t kOverlayManagerDecalDrawCountOffset = 0xD8;

        [[nodiscard]] bool IsRingDecalSlot(const std::byte* const slotBase) noexcept
        {
            if (!slotBase) {
                return false;
            }

            uint32_t flags = 0;
            std::memcpy(&flags, slotBase + 4, sizeof(flags));
            return (flags & 0x80u) == 0u;
        }
    }

    TerrainDecalHook::TerrainDecalHook(const Options options)
        : options_(options)
        , renderer_(RendererOptions{
              .enableClippedRendering = options.enableCustomRenderer,
              .customDefaultDepthOffset = options.customDefaultDepthOffset,
              .shadowRecoveryOpacityScale = options.shadowRecoveryOpacityScale,
          })
    {
    }

    TerrainDecalHook::~TerrainDecalHook()
    {
        Uninstall();
    }

    bool TerrainDecalHook::Install()
    {
        if (callSitePatch_.IsInstalled() &&
            setTexTransformCallSitePatch_.IsInstalled() &&
            drawShadowsCallSitePatch_.IsInstalled() &&
            drawShadowsRoughCallSitePatch_.IsInstalled()) {
            return true;
        }

        if (!options_.installEnabled) {
            SetLastError_("terrain decal hook disabled by configuration");
            return false;
        }

        const auto gameVersion = VersionDetection::GetInstance().GetGameVersion();
        addresses_ = ResolveHookAddresses(gameVersion);
        if (!addresses_) {
            SetLastError_(std::string("unsupported game version: ") + std::to_string(gameVersion));
            LOG_INFO("TerrainDecalHook: {}. No patch installed.", lastError_);
            return false;
        }

        if (sActiveHook_ && sActiveHook_ != this) {
            SetLastError_("another terrain decal hook instance is already active");
            LOG_WARN("TerrainDecalHook: {}", lastError_);
            return false;
        }

        sActiveHook_ = this;
        callSitePatch_.Configure("cSTEOverlayManager::DrawDecals->DrawRect call site",
                                 addresses_->drawRectCallSite,
                                 reinterpret_cast<void*>(&DrawRectCallThunk));
        setTexTransformCallSitePatch_.Configure("cSTEOverlayManager::DrawDecals->SetTexTransform4 call site",
                                                addresses_->setTexTransform4CallSite,
                                                reinterpret_cast<void*>(&SetTexTransform4CallThunk));
        drawShadowsCallSitePatch_.Configure("cSTEOverlayManager::DrawOverlays->DrawShadows call site",
                                            addresses_->drawShadowsCallSite,
                                            reinterpret_cast<void*>(&DrawShadowsCallThunk));
        drawShadowsRoughCallSitePatch_.Configure("cSTEOverlayManager::DrawOverlays->DrawShadowsRough call site",
                                                 addresses_->drawShadowsRoughCallSite,
                                                 reinterpret_cast<void*>(&DrawShadowsRoughCallThunk));

        if (!callSitePatch_.Install()) {
            sActiveHook_ = nullptr;
            SetLastError_("failed to install draw-rect call-site patch");
            return false;
        }
        if (!setTexTransformCallSitePatch_.Install()) {
            callSitePatch_.Uninstall();
            sActiveHook_ = nullptr;
            SetLastError_("failed to install set-tex-transform call-site patch");
            return false;
        }
        if (!drawShadowsCallSitePatch_.Install()) {
            setTexTransformCallSitePatch_.Uninstall();
            callSitePatch_.Uninstall();
            sActiveHook_ = nullptr;
            SetLastError_("failed to install draw-shadows call-site patch");
            return false;
        }
        if (!drawShadowsRoughCallSitePatch_.Install()) {
            drawShadowsCallSitePatch_.Uninstall();
            setTexTransformCallSitePatch_.Uninstall();
            callSitePatch_.Uninstall();
            sActiveHook_ = nullptr;
            SetLastError_("failed to install draw-shadows-rough call-site patch");
            return false;
        }

        lastError_.clear();
        LOG_INFO("TerrainDecalHook: installed at 0x{:08X} / 0x{:08X}; shadow replay at 0x{:08X} / 0x{:08X} for {}",
                 static_cast<uint32_t>(addresses_->drawRectCallSite),
                 static_cast<uint32_t>(addresses_->setTexTransform4CallSite),
                 static_cast<uint32_t>(addresses_->drawShadowsCallSite),
                 static_cast<uint32_t>(addresses_->drawShadowsRoughCallSite),
                 DescribeKnownAddressSet(addresses_->gameVersion));
        return true;
    }

    void TerrainDecalHook::Uninstall()
    {
        drawShadowsRoughCallSitePatch_.Uninstall();
        drawShadowsCallSitePatch_.Uninstall();
        setTexTransformCallSitePatch_.Uninstall();
        callSitePatch_.Uninstall();
        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;
        shadowRecoveryActive_ = false;
        shadowRecoveryCaptureActive_ = false;
        ReleaseRecoveryTarget_();
        renderer_.ClearOverlayUvWindows();

        if (sActiveHook_ == this) {
            sActiveHook_ = nullptr;
        }
    }

    bool TerrainDecalHook::IsInstalled() const noexcept
    {
        return callSitePatch_.IsInstalled() &&
               setTexTransformCallSitePatch_.IsInstalled() &&
               drawShadowsCallSitePatch_.IsInstalled() &&
               drawShadowsRoughCallSitePatch_.IsInstalled();
    }

    std::string_view TerrainDecalHook::GetLastError() const noexcept
    {
        return lastError_;
    }

    void TerrainDecalHook::SetOverlayUvWindow(const uint32_t overlayId, const TerrainDecalUvWindow& uvWindow)
    {
        renderer_.SetOverlayUvWindow(overlayId, uvWindow);
    }

    bool TerrainDecalHook::RemoveOverlayUvWindow(const uint32_t overlayId) noexcept
    {
        return renderer_.RemoveOverlayUvWindow(overlayId);
    }

    void TerrainDecalHook::ClearOverlayUvWindows() noexcept
    {
        renderer_.ClearOverlayUvWindows();
    }

    bool TerrainDecalHook::TryGetOverlayUvWindow(const uint32_t overlayId, TerrainDecalUvWindow& uvWindow) const noexcept
    {
        return renderer_.TryGetOverlayUvWindow(overlayId, uvWindow);
    }

    void TerrainDecalHook::SetOverlayOverridesResolver(const OverlayOverridesResolver resolver, void* const userData) noexcept
    {
        renderer_.SetOverlayOverridesResolver(resolver, userData);
    }

    void __fastcall TerrainDecalHook::DrawRectCallThunk(void* overlayManager,
                                                        void*,
                                                        SC4DrawContext* drawContext,
                                                        const cRZRect* rect)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawRectCall_(overlayManager, drawContext, rect);
    }

    void __fastcall TerrainDecalHook::DrawShadowsCallThunk(void* overlayManager,
                                                           void*,
                                                           const float* worldToScreenMatrix,
                                                           SC4DrawContext* drawContext,
                                                           int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawShadowsCall_(sActiveHook_->drawShadowsCallSitePatch_,
                                             overlayManager,
                                             worldToScreenMatrix,
                                             drawContext,
                                             decalIds);
    }

    void __fastcall TerrainDecalHook::DrawShadowsRoughCallThunk(void* overlayManager,
                                                                void*,
                                                                const float* worldToScreenMatrix,
                                                                SC4DrawContext* drawContext,
                                                                int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawShadowsCall_(sActiveHook_->drawShadowsRoughCallSitePatch_,
                                             overlayManager,
                                             worldToScreenMatrix,
                                             drawContext,
                                             decalIds);
    }

    void __fastcall TerrainDecalHook::SetTexTransform4CallThunk(SC4DrawContext* drawContext,
                                                                void*,
                                                                const float* matrix,
                                                                int stage)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleSetTexTransform4Call_(drawContext, matrix, stage);
    }

    void TerrainDecalHook::HandleDrawRectCall_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect)
    {
        DrawRequest request{
            .overlayManager = overlayManager,
            .drawContext = drawContext,
            .rect = rect,
            .overlaySlotBase = nullptr,
            .activeTexTransform = currentTexTransformValid_ ? currentTexTransform_.data() : nullptr,
            .activeTexTransformStage = currentTexTransformValid_ ? currentTexTransformStage_ : -1,
            .overlayRectOffset = 0,
            .addresses = addresses_ ? &*addresses_ : nullptr,
            .terrain = nullptr,
            .terrainView = nullptr,
            .mode = shadowRecoveryActive_
                        ? (shadowRecoveryCaptureActive_ ? DrawMode::ShadowRecoveryCapture : DrawMode::ShadowRecovery)
                        : DrawMode::Normal,
        };

        if (addresses_ && rect && addresses_->overlayRectOffset > 0) {
            request.overlayRectOffset = addresses_->overlayRectOffset;
            request.overlaySlotBase = reinterpret_cast<const std::byte*>(rect) - addresses_->overlayRectOffset;
        }

        if (request.overlaySlotBase && IsRingDecalSlot(request.overlaySlotBase)) {
            currentTexTransformValid_ = false;
            currentTexTransformStage_ = -1;
            if (!shadowRecoveryActive_) {
                CallOriginalDrawRect_(overlayManager, drawContext, rect);
            }
            return;
        }

        const cISC4AppPtr app;
        cISC4City* city = app ? app->GetCity() : nullptr;
        request.terrain = city ? city->GetTerrain() : nullptr;
        request.terrainView = request.terrain ? request.terrain->GetView() : nullptr;

        const auto result = renderer_.Draw(request);
        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;

        if (result == DrawResult::Handled) {
            return;
        }

        if (shadowRecoveryActive_) {
            return;
        }

        CallOriginalDrawRect_(overlayManager, drawContext, rect);
    }

    void TerrainDecalHook::HandleDrawShadowsCall_(const RelativeCallPatch& patch,
                                                  void* overlayManager,
                                                  const float* worldToScreenMatrix,
                                                  SC4DrawContext* drawContext,
                                                  int* decalIds)
    {
        CallOriginalOverlayPass_(patch, overlayManager, worldToScreenMatrix, drawContext, decalIds);
        ReplayManagedDecalsAfterShadows_(overlayManager, worldToScreenMatrix, drawContext, decalIds);
    }

    void TerrainDecalHook::HandleSetTexTransform4Call_(SC4DrawContext* drawContext, const float* matrix, const int stage)
    {
        if (matrix) {
            std::copy_n(matrix, currentTexTransform_.size(), currentTexTransform_.begin());
            currentTexTransformStage_ = stage;
            currentTexTransformValid_ = true;
        }
        else {
            currentTexTransformValid_ = false;
            currentTexTransformStage_ = -1;
        }

        CallOriginalSetTexTransform4_(drawContext, matrix, stage);
    }

    void TerrainDecalHook::CallOriginalDrawRect_(void* overlayManager,
                                                 SC4DrawContext* drawContext,
                                                 const cRZRect* rect) const
    {
        const auto originalTarget = callSitePatch_.GetOriginalTarget();
        if (!originalTarget) {
            return;
        }

        const auto original = reinterpret_cast<DrawRectFn>(originalTarget);
        original(overlayManager, drawContext, rect);
    }

    void TerrainDecalHook::CallOriginalOverlayPass_(const RelativeCallPatch& patch,
                                                    void* overlayManager,
                                                    const float* worldToScreenMatrix,
                                                    SC4DrawContext* drawContext,
                                                    int* decalIds) const
    {
        const auto originalTarget = patch.GetOriginalTarget();
        if (!originalTarget) {
            return;
        }

        const auto original = reinterpret_cast<DrawOverlayPassFn>(originalTarget);
        original(overlayManager, worldToScreenMatrix, drawContext, decalIds);
    }

    void TerrainDecalHook::CallOriginalSetTexTransform4_(SC4DrawContext* drawContext,
                                                         const float* matrix,
                                                         const int stage) const
    {
        const auto originalTarget = setTexTransformCallSitePatch_.GetOriginalTarget();
        if (!originalTarget) {
            return;
        }

        const auto original = reinterpret_cast<SetTexTransform4Fn>(originalTarget);
        original(drawContext, matrix, stage);
    }

    void TerrainDecalHook::ReplayManagedDecalsAfterShadows_(void* overlayManager,
                                                            const float* worldToScreenMatrix,
                                                            SC4DrawContext* drawContext,
                                                            int* decalIds)
    {
        if (!options_.enableCustomRenderer ||
            shadowRecoveryActive_ ||
            !addresses_ ||
            !addresses_->drawDecals ||
            !overlayManager ||
            !worldToScreenMatrix ||
            !drawContext ||
            !decalIds) {
            return;
        }

        if (CompositeManagedDecalsAfterShadows_(overlayManager, worldToScreenMatrix, drawContext, decalIds)) {
            return;
        }

        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;

        auto* const overlayManagerBytes = reinterpret_cast<std::byte*>(overlayManager);
        int savedDecalDrawCount = 0;
        std::memcpy(&savedDecalDrawCount,
                    overlayManagerBytes + kOverlayManagerDecalDrawCountOffset,
                    sizeof(savedDecalDrawCount));

        const bool previousShadowRecoveryActive = shadowRecoveryActive_;
        shadowRecoveryActive_ = true;
        const auto drawDecals = reinterpret_cast<DrawOverlayPassFn>(addresses_->drawDecals);
        drawDecals(overlayManager, worldToScreenMatrix, drawContext, decalIds);
        shadowRecoveryActive_ = previousShadowRecoveryActive;

        std::memcpy(overlayManagerBytes + kOverlayManagerDecalDrawCountOffset,
                    &savedDecalDrawCount,
                    sizeof(savedDecalDrawCount));

        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;
    }

    bool TerrainDecalHook::EnsureRecoveryTarget_(IDirect3DDevice7* const device,
                                                 IDirectDrawSurface7* const originalTarget)
    {
        if (!device || !originalTarget) {
            return false;
        }

        DDSURFACEDESC2 originalDesc{.dwSize = sizeof(DDSURFACEDESC2)};
        if (FAILED(originalTarget->GetSurfaceDesc(&originalDesc)) ||
            originalDesc.dwWidth == 0 || originalDesc.dwHeight == 0) {
            return false;
        }

        if (recoveryDevice_ == device && recoveryTarget_ && recoveryDepth_ &&
            recoveryWidth_ == originalDesc.dwWidth && recoveryHeight_ == originalDesc.dwHeight &&
            recoveryTarget_->IsLost() == DD_OK && recoveryDepth_->IsLost() == DD_OK) {
            return true;
        }

        ReleaseRecoveryTarget_();
        auto* const d3dx = GraphicsInterfaceHook::GetD3DXInterface();
        IDirectDraw7* const dd = d3dx ? d3dx->GetDD() : nullptr;
        IDirectDrawSurface7* const gameDepth = d3dx ? d3dx->GetZBuffer() : nullptr;
        if (!dd || !gameDepth) {
            return false;
        }

        DDSURFACEDESC2 colorDesc{.dwSize = sizeof(DDSURFACEDESC2)};
        colorDesc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        colorDesc.dwWidth = originalDesc.dwWidth;
        colorDesc.dwHeight = originalDesc.dwHeight;
        colorDesc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        colorDesc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        colorDesc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        if (originalDesc.ddpfPixelFormat.dwRGBBitCount <= 16) {
            colorDesc.ddpfPixelFormat.dwRGBBitCount = 16;
            colorDesc.ddpfPixelFormat.dwRBitMask = 0x0F00;
            colorDesc.ddpfPixelFormat.dwGBitMask = 0x00F0;
            colorDesc.ddpfPixelFormat.dwBBitMask = 0x000F;
            colorDesc.ddpfPixelFormat.dwRGBAlphaBitMask = 0xF000;
        }
        else {
            colorDesc.ddpfPixelFormat.dwRGBBitCount = 32;
            colorDesc.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
            colorDesc.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
            colorDesc.ddpfPixelFormat.dwBBitMask = 0x000000FF;
            colorDesc.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
        }

        if (FAILED(dd->CreateSurface(&colorDesc, &recoveryTarget_, nullptr))) {
            ReleaseRecoveryTarget_();
            return false;
        }

        DDSURFACEDESC2 depthDesc{.dwSize = sizeof(DDSURFACEDESC2)};
        if (FAILED(gameDepth->GetSurfaceDesc(&depthDesc))) {
            ReleaseRecoveryTarget_();
            return false;
        }
        depthDesc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        depthDesc.dwWidth = originalDesc.dwWidth;
        depthDesc.dwHeight = originalDesc.dwHeight;
        depthDesc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        if (FAILED(dd->CreateSurface(&depthDesc, &recoveryDepth_, nullptr)) ||
            FAILED(recoveryTarget_->AddAttachedSurface(recoveryDepth_))) {
            ReleaseRecoveryTarget_();
            return false;
        }

        recoveryDevice_ = device;
        recoveryDevice_->AddRef();
        recoveryWidth_ = originalDesc.dwWidth;
        recoveryHeight_ = originalDesc.dwHeight;
        LOG_INFO("TerrainDecalHook: created {}x{} shadow recovery render target", recoveryWidth_, recoveryHeight_);
        return true;
    }

    void TerrainDecalHook::ReleaseRecoveryTarget_() noexcept
    {
        if (recoveryDepth_) {
            recoveryDepth_->Release();
            recoveryDepth_ = nullptr;
        }
        if (recoveryTarget_) {
            recoveryTarget_->Release();
            recoveryTarget_ = nullptr;
        }
        if (recoveryDevice_) {
            recoveryDevice_->Release();
            recoveryDevice_ = nullptr;
        }
        recoveryWidth_ = 0;
        recoveryHeight_ = 0;
    }

    bool TerrainDecalHook::CompositeManagedDecalsAfterShadows_(void* const overlayManager,
                                                               const float* const worldToScreenMatrix,
                                                               SC4DrawContext* const drawContext,
                                                               int* const decalIds)
    {
        auto* const d3dx = GraphicsInterfaceHook::GetD3DXInterface();
        IDirect3DDevice7* const device = d3dx ? d3dx->GetD3DDevice() : nullptr;
        IDirectDrawSurface7* const gameDepth = d3dx ? d3dx->GetZBuffer() : nullptr;
        if (!device || !gameDepth) {
            return false;
        }

        IDirectDrawSurface7* originalTarget = nullptr;
        DWORD stateBlock = 0;
        DWORD replayStateBlock = 0;
        D3DVIEWPORT7 viewport{};
        if (FAILED(device->GetRenderTarget(&originalTarget)) || !originalTarget ||
            FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) ||
            FAILED(device->GetViewport(&viewport)) ||
            !EnsureRecoveryTarget_(device, originalTarget)) {
            if (stateBlock) device->DeleteStateBlock(stateBlock);
            if (originalTarget) originalTarget->Release();
            return false;
        }

        bool targetChanged = false;
        bool captureStarted = false;
        bool success = false;
        do {
            // Preserve scene occlusion in the private target. Drivers that cannot
            // copy their Z surface fall back to the direct replay path below.
            if (FAILED(recoveryDepth_->Blt(nullptr, gameDepth, nullptr, DDBLT_WAIT, nullptr))) break;
            if (FAILED(device->SetRenderTarget(recoveryTarget_, 0))) break;
            targetChanged = true;
            // SetRenderTarget may reset the device viewport while SC4's draw-context
            // cache still believes the city viewport is active.
            if (FAILED(device->SetViewport(&viewport))) break;
            if (FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0))) break;

            currentTexTransformValid_ = false;
            currentTexTransformStage_ = -1;
            shadowRecoveryActive_ = true;
            shadowRecoveryCaptureActive_ = true;
            captureStarted = true;
            reinterpret_cast<DrawOverlayPassFn>(addresses_->drawDecals)(overlayManager,
                                                                         worldToScreenMatrix,
                                                                         drawContext,
                                                                         decalIds);
            shadowRecoveryCaptureActive_ = false;
            shadowRecoveryActive_ = false;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &replayStateBlock))) break;

            if (FAILED(device->SetRenderTarget(originalTarget, 0))) break;
            targetChanged = false;
            if (FAILED(device->SetViewport(&viewport))) break;

            struct CompositeVertex { float x, y, z, rhw; DWORD color; float u, v; };
            const float left = static_cast<float>(viewport.dwX) - 0.5f;
            const float top = static_cast<float>(viewport.dwY) - 0.5f;
            const float right = static_cast<float>(viewport.dwX + viewport.dwWidth) - 0.5f;
            const float bottom = static_cast<float>(viewport.dwY + viewport.dwHeight) - 0.5f;
            const float u0 = static_cast<float>(viewport.dwX) / static_cast<float>(recoveryWidth_);
            const float v0 = static_cast<float>(viewport.dwY) / static_cast<float>(recoveryHeight_);
            const float u1 = static_cast<float>(viewport.dwX + viewport.dwWidth) / static_cast<float>(recoveryWidth_);
            const float v1 = static_cast<float>(viewport.dwY + viewport.dwHeight) / static_cast<float>(recoveryHeight_);
            const float opacity = std::clamp(options_.shadowRecoveryOpacityScale, 0.0f, 1.0f);
            const DWORD color = D3DRGBA(opacity, opacity, opacity, opacity);
            const CompositeVertex vertices[] = {
                {left, top, 0.0f, 1.0f, color, u0, v0},
                {right, top, 0.0f, 1.0f, color, u1, v0},
                {left, bottom, 0.0f, 1.0f, color, u0, v1},
                {right, bottom, 0.0f, 1.0f, color, u1, v1},
            };

            // The capture is already alpha-blended into transparent black, so its
            // RGB is premultiplied. Scale RGB and alpha together and do not apply
            // source alpha a second time.
            const bool stateReady =
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA)) &&
                SUCCEEDED(device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE)) &&
                SUCCEEDED(device->SetTexture(0, recoveryTarget_)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_POINT)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0)) &&
                SUCCEEDED(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE)) &&
                SUCCEEDED(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE)) &&
                SUCCEEDED(device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE)) &&
                SUCCEEDED(device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0)) &&
                SUCCEEDED(device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
            if (!stateReady) break;
            success = SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,
                                                       D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                                                       const_cast<CompositeVertex*>(vertices),
                                                       4,
                                                       D3DDP_WAIT));
        } while (false);

        shadowRecoveryCaptureActive_ = false;
        shadowRecoveryActive_ = false;
        if (targetChanged) {
            device->SetRenderTarget(originalTarget, 0);
            device->SetViewport(&viewport);
        }
        if (replayStateBlock) {
            device->ApplyStateBlock(replayStateBlock);
        }
        else if (!captureStarted) {
            device->ApplyStateBlock(stateBlock);
        }
        if (replayStateBlock) device->DeleteStateBlock(replayStateBlock);
        device->DeleteStateBlock(stateBlock);
        originalTarget->Release();
        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;
        if (!success) {
            ReleaseRecoveryTarget_();
        }
        // Once DrawDecals touched SC4's internal draw-context cache, do not run the
        // legacy replay in the same frame after restoring only the native D3D state.
        return success || captureStarted;
    }

    void TerrainDecalHook::SetLastError_(std::string message)
    {
        lastError_ = std::move(message);
    }
}
