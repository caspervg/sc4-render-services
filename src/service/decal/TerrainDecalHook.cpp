#include "TerrainDecalHook.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <d3d.h>
#include <ddraw.h>

#include "DX7InterfaceHook.h"
#include "SC4DrawContextAccess.h"
#include "cIGZGDriver.h"
#include "sGDMode.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

namespace TerrainDecal
{
    TerrainDecalHook* TerrainDecalHook::sActiveHook_ = nullptr;

    namespace
    {
        constexpr std::ptrdiff_t kOverlayManagerDecalDrawCountOffset = 0xD8;
        // Set on the water overlay managers. The game's DrawRect (0x00735720) then draws the
        // decal on the water surface mesh instead of the terrain.
        constexpr std::ptrdiff_t kOverlayManagerWaterFlagOffset = 0x16;

        [[nodiscard]] bool IsWaterOverlayManager(const void* const overlayManager) noexcept
        {
            return overlayManager &&
                   static_cast<const std::byte*>(overlayManager)[kOverlayManagerWaterFlagOffset] != std::byte{0};
        }

        // Renderer -> options -> a flag that keeps alpha testing off whatever the state asks
        // for. SC4DrawContext::SetRenderState (0x007D4530) checks it the same way.
        constexpr std::ptrdiff_t kRendererOptionsOffset = 0x0C;
        constexpr std::ptrdiff_t kOptionsAlphaTestOffOffset = 0x11C;

        // The draw context's packed render state, as SC4DrawContext::SetRenderState applies it.
        constexpr uint32_t kPackedColorWrite = 0x1;
        constexpr uint32_t kPackedDepthWrite = 0x2;
        constexpr uint32_t kPackedAlphaTest = 0x8;
        constexpr uint32_t kPackedAlphaFuncShift = 10;
        constexpr uint32_t kPackedAlphaRefShift = 21;
        constexpr float kPackedAlphaRefScale = 1.0f / 15.0f;

        // cIGZGDriver values.
        constexpr uint32_t kDriverStateAlphaTest = 0;
        constexpr uint32_t kDriverStateStencilTest = 2;
        // Bit 0x80 belongs to SCD3D11's live shadows.
        constexpr uint32_t kRecoveryStencilMask = 0x7F;
        constexpr uint32_t kDriverTestEqual = 2;
        constexpr uint32_t kDriverTestGreater = 4;
        constexpr uint32_t kDriverTestAlways = 7;
        // Stencil ops run zero, keep, replace, incr, decr, invert. Both built-in drivers'
        // StencilOp index a table in that order (OpenGL 0x0087C5F0 reads 0x00B120BC, DirectX
        // 0x00882E50 reads 0x00AC450C), and SCD3D11 maps them the same way. SCGL's table
        // leaves zero out, so its codes are one lower.
        constexpr uint32_t kDriverStencilKeep = 1;
        constexpr uint32_t kDriverStencilReplace = 2;

        enum RecoveryMethod : int
        {
            kRecoveryComposite = 0,
            kRecoveryStencil = 1,
            kRecoveryReplay = 2,
            kRecoveryOff = 3,
            kRecoveryZeroOpacity = 4,
        };

        using DrawContextAccess::ReadField;

        [[nodiscard]] bool AlphaTestForcedOff(SC4DrawContext* const drawContext) noexcept
        {
            const auto* const renderer = ReadField<const std::byte*>(drawContext, DrawContextAccess::kRendererOffset);
            const auto* const options = renderer ? ReadField<const std::byte*>(renderer, kRendererOptionsOffset) : nullptr;
            return options && options[kOptionsAlphaTestOffOffset] != std::byte{0};
        }

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

    std::array<RelativeCallPatch*, 5> TerrainDecalHook::Patches_() noexcept
    {
        return {&drawDecalsCallSitePatch_, &callSitePatch_, &setTexTransformCallSitePatch_,
                &drawShadowsCallSitePatch_, &drawShadowsRoughCallSitePatch_};
    }

    bool TerrainDecalHook::Install()
    {
        if (IsInstalled()) {
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
        drawDecalsCallSitePatch_.Configure("cSTEOverlayManager::DrawOverlays->DrawDecals call site",
                                           addresses_->drawDecalsCallSite,
                                           reinterpret_cast<void*>(&DrawDecalsCallThunk));
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

        for (RelativeCallPatch* const patch : Patches_()) {
            if (!patch->Install()) {
                Uninstall();
                SetLastError_("failed to install a call-site patch, see the log");
                return false;
            }
        }

        lastError_.clear();
        LOG_INFO("TerrainDecalHook: installed at 0x{:08X} (order), 0x{:08X} / 0x{:08X} (draw); shadow replay at 0x{:08X} / 0x{:08X} for {}",
                 static_cast<uint32_t>(addresses_->drawDecalsCallSite),
                 static_cast<uint32_t>(addresses_->drawRectCallSite),
                 static_cast<uint32_t>(addresses_->setTexTransform4CallSite),
                 static_cast<uint32_t>(addresses_->drawShadowsCallSite),
                 static_cast<uint32_t>(addresses_->drawShadowsRoughCallSite),
                 DescribeKnownAddressSet(addresses_->gameVersion));
        return true;
    }

    void TerrainDecalHook::Uninstall()
    {
        auto patches = Patches_();
        for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
            (*it)->Uninstall();
        }
        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;
        shadowRecoveryActive_ = false;
        shadowRecoveryCaptureActive_ = false;
        ReleaseRecoveryTarget_();

        if (sActiveHook_ == this) {
            sActiveHook_ = nullptr;
        }
    }

    bool TerrainDecalHook::IsInstalled() const noexcept
    {
        return drawDecalsCallSitePatch_.IsInstalled() &&
               callSitePatch_.IsInstalled() &&
               setTexTransformCallSitePatch_.IsInstalled() &&
               drawShadowsCallSitePatch_.IsInstalled() &&
               drawShadowsRoughCallSitePatch_.IsInstalled();
    }

    std::string_view TerrainDecalHook::GetLastError() const noexcept
    {
        return lastError_;
    }

    void TerrainDecalHook::SetOverlaySource(ManagedOverlaySource* const source) noexcept
    {
        overlaySource_ = source;
        renderer_.SetOverlaySource(source);
    }

    void __fastcall TerrainDecalHook::DrawDecalsCallThunk(void* overlayManager,
                                                         void*,
                                                         const float* worldToScreenMatrix,
                                                         SC4DrawContext* drawContext,
                                                         int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        ++sActiveHook_->overlayPass_;
        sActiveHook_->SortByDrawOrder_(overlayManager, decalIds);
        sActiveHook_->CallOriginalOverlayPass_(sActiveHook_->drawDecalsCallSitePatch_,
                                               overlayManager,
                                               worldToScreenMatrix,
                                               drawContext,
                                               decalIds);
    }

    void TerrainDecalHook::SortByDrawOrder_(const void* const overlayManager, int* const decalIds) const
    {
        // decalIds is the std::vector<uint32_t> of overlay slots SDSFindOverlaysToDraw picked,
        // in slot order. The shadow passes get the same vector, so they follow this order too.
        auto* const range = reinterpret_cast<uint32_t* const*>(decalIds);
        if (!overlaySource_ || !range || !range[0] || range[1] - range[0] < 2) {
            return;
        }

        struct Keyed
        {
            std::pair<int32_t, uint32_t> order;
            uint32_t overlayId;
        };
        thread_local std::vector<Keyed> keyed;
        keyed.clear();
        for (const uint32_t* id = range[0]; id != range[1]; ++id) {
            keyed.push_back(Keyed{overlaySource_->OverlayDrawOrder(overlayManager, *id), *id});
        }

        const auto byOrder = [](const Keyed& a, const Keyed& b) { return a.order < b.order; };
        if (std::is_sorted(keyed.begin(), keyed.end(), byOrder)) {
            return;
        }
        std::stable_sort(keyed.begin(), keyed.end(), byOrder);
        uint32_t* out = range[0];
        for (const Keyed& entry : keyed) {
            *out++ = entry.overlayId;
        }
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

    void TerrainDecalHook::AroundStencilRecoveryDraw_(void* const userData,
                                                      SC4DrawContext* const drawContext,
                                                      const bool before)
    {
        const auto* const self = static_cast<TerrainDecalHook*>(userData);
        cIGZGDriver* const driver = DrawContextAccess::Driver(drawContext);
        if (!self || !driver) {
            return;
        }

        // Numbers 1 to 127. Decals 127 apart share a number, which only matters where they overlap.
        const int number = static_cast<int>(self->stencilDecalNumber_ % kRecoveryStencilMask) + 1;

        if (self->stencilPass_ == StencilPass::Draw) {
            if (before) {
                driver->StencilFunc(kDriverTestEqual, number, kRecoveryStencilMask);
                driver->StencilOp(kDriverStencilKeep, kDriverStencilKeep, kDriverStencilKeep);
            }
            return;
        }

        if (self->stencilPass_ != StencilPass::Claim) {
            return;
        }

        if (before) {
            // Mark the pixels this decal covers, without drawing anything. Fully transparent
            // texels do not count, so decals below still show through them.
            driver->ColorMask(false);
            driver->DepthMask(false);
            driver->AlphaFunc(kDriverTestGreater, 0.0f);
            driver->Enable(kDriverStateAlphaTest);
            driver->StencilFunc(kDriverTestAlways, number, kRecoveryStencilMask);
            driver->StencilOp(kDriverStencilKeep, kDriverStencilKeep, kDriverStencilReplace);
            return;
        }

        // Put back what the draw context's state cache says is set, so the cache stays true.
        const auto state = ReadField<uint32_t>(drawContext, DrawContextAccess::kRenderStateOffset);
        driver->ColorMask((state & kPackedColorWrite) != 0);
        driver->DepthMask((state & kPackedDepthWrite) != 0);
        driver->AlphaFunc((state >> kPackedAlphaFuncShift) & 7u,
                          static_cast<float>((state >> kPackedAlphaRefShift) & 0xFu) * kPackedAlphaRefScale);
        if ((state & kPackedAlphaTest) != 0 && !AlphaTestForcedOff(drawContext)) {
            driver->Enable(kDriverStateAlphaTest);
        }
        else {
            driver->Disable(kDriverStateAlphaTest);
        }
    }

    void TerrainDecalHook::HandleDrawRectCall_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect)
    {
        if (!options_.enableCustomRenderer) {
            // The game draws every decal itself. Shadow recovery is off too, it needs the
            // custom renderer.
            CallOriginalDrawRect_(overlayManager, drawContext, rect);
            return;
        }

        if (IsWaterOverlayManager(overlayManager)) {
            // The custom renderer drapes decals over the terrain, which would put water decals
            // such as water pollution on dry land. The game draws them on the water itself.
            if (!shadowRecoveryActive_) {
                CallOriginalDrawRect_(overlayManager, drawContext, rect);
            }
            return;
        }

        DrawRequest request{
            .overlayManager = overlayManager,
            .drawContext = drawContext,
            .rect = rect,
            .overlaySlotBase = nullptr,
            .activeTexTransform = currentTexTransformValid_ ? currentTexTransform_.data() : nullptr,
            .activeTexTransformStage = currentTexTransformValid_ ? currentTexTransformStage_ : -1,
            .addresses = addresses_ ? &*addresses_ : nullptr,
            .mode = shadowRecoveryActive_
                        ? (shadowRecoveryCaptureActive_ ? DrawMode::ShadowRecoveryCapture : DrawMode::ShadowRecovery)
                        : DrawMode::Normal,
            .pass = overlayPass_,
        };

        if (addresses_ && rect && addresses_->overlayRectOffset > 0) {
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

        if (shadowRecoveryActive_ && stencilPass_ != StencilPass::None) {
            // Both passes see the same decals in the same order, so the numbers match up.
            ++stencilDecalNumber_;
            request.aroundDraw = &AroundStencilRecoveryDraw_;
            request.aroundDrawUserData = this;
        }

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
        // What the decal pass laid is not to be drawn again by a later pass.
        ++overlayPass_;
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
        if (!options_.enableCustomRenderer) {
            LogRecoveryMethod_(kRecoveryOff);
            return;
        }
        if (!(options_.shadowRecoveryOpacityScale > 0.0f)) {
            LogRecoveryMethod_(kRecoveryZeroOpacity);
            return;
        }
        if (shadowRecoveryActive_ ||
            !addresses_ ||
            !addresses_->drawDecals ||
            !overlayManager ||
            !worldToScreenMatrix ||
            !drawContext ||
            !decalIds) {
            return;
        }

        if (CompositeManagedDecalsAfterShadows_(overlayManager, worldToScreenMatrix, drawContext, decalIds)) {
            LogRecoveryMethod_(kRecoveryComposite);
            return;
        }

        cIGZGDriver* const driver = DrawContextAccess::Driver(drawContext);
        const bool useStencil = options_.shadowRecoveryUseStencil && DriverHasStencil_(driver);
        LogRecoveryMethod_(useStencil ? kRecoveryStencil : kRecoveryReplay);

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
        if (useStencil) {
            // No clear: the claim pass renumbers every pixel a decal covers, and a clear would
            // wipe the driver's bits.
            driver->StencilMask(kRecoveryStencilMask);
            driver->Enable(kDriverStateStencilTest);

            for (const StencilPass pass : {StencilPass::Claim, StencilPass::Draw}) {
                stencilPass_ = pass;
                stencilDecalNumber_ = 0;
                drawDecals(overlayManager, worldToScreenMatrix, drawContext, decalIds);
                currentTexTransformValid_ = false;
                currentTexTransformStage_ = -1;
            }
            stencilPass_ = StencilPass::None;

            driver->StencilFunc(kDriverTestAlways, 0, 0xFF);
            driver->StencilOp(kDriverStencilKeep, kDriverStencilKeep, kDriverStencilKeep);
            driver->StencilMask(0xFF);
            driver->Disable(kDriverStateStencilTest);
        }
        else {
            drawDecals(overlayManager, worldToScreenMatrix, drawContext, decalIds);
        }
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
        auto* const d3dx = DX7InterfaceHook::GetD3DXInterface();
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
        auto* const d3dx = DX7InterfaceHook::GetD3DXInterface();
        IDirect3DDevice7* const device = d3dx ? d3dx->GetD3DDevice() : nullptr;
        if (!device) {
            return false;
        }

        IDirectDrawSurface7* originalTarget = nullptr;
        DWORD stateBlock = 0;
        DWORD replayStateBlock = 0;
        if (FAILED(device->GetRenderTarget(&originalTarget)) || !originalTarget ||
            FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) ||
            !EnsureRecoveryTarget_(device, originalTarget)) {
            if (stateBlock) device->DeleteStateBlock(stateBlock);
            if (originalTarget) originalTarget->Release();
            return false;
        }

        bool targetChanged = false;
        bool captureStarted = false;
        bool success = false;
        do {
            if (FAILED(device->SetRenderTarget(recoveryTarget_, 0))) break;
            targetChanged = true;
            if (FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0))) break;

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
            device->CreateStateBlock(D3DSBT_ALL, &replayStateBlock);

            if (FAILED(device->SetRenderTarget(originalTarget, 0))) break;
            targetChanged = false;

            D3DVIEWPORT7 viewport{};
            if (FAILED(device->GetViewport(&viewport))) break;

            struct CompositeVertex { float x, y, z, rhw; DWORD color; float u, v; };
            const float left = static_cast<float>(viewport.dwX) - 0.5f;
            const float top = static_cast<float>(viewport.dwY) - 0.5f;
            const float right = static_cast<float>(viewport.dwX + viewport.dwWidth) - 0.5f;
            const float bottom = static_cast<float>(viewport.dwY + viewport.dwHeight) - 0.5f;
            const float u0 = static_cast<float>(viewport.dwX) / static_cast<float>(recoveryWidth_);
            const float v0 = static_cast<float>(viewport.dwY) / static_cast<float>(recoveryHeight_);
            const float u1 = static_cast<float>(viewport.dwX + viewport.dwWidth) / static_cast<float>(recoveryWidth_);
            const float v1 = static_cast<float>(viewport.dwY + viewport.dwHeight) / static_cast<float>(recoveryHeight_);
            const DWORD color = D3DRGBA(1.0f, 1.0f, 1.0f,
                                        std::clamp(options_.shadowRecoveryOpacityScale, 0.0f, 1.0f));
            const CompositeVertex vertices[] = {
                {left, top, 0.0f, 1.0f, color, u0, v0},
                {right, top, 0.0f, 1.0f, color, u1, v0},
                {left, bottom, 0.0f, 1.0f, color, u0, v1},
                {right, bottom, 0.0f, 1.0f, color, u1, v1},
            };

            device->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
            device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
            device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
            device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            device->SetTexture(0, recoveryTarget_);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
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
        }
        device->ApplyStateBlock(replayStateBlock ? replayStateBlock : stateBlock);
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

    bool TerrainDecalHook::DriverHasStencil_(cIGZGDriver* const driver)
    {
        if (!driver) {
            return false;
        }
        if (driver != stencilCheckedDriver_) {
            // The current video mode says whether the depth buffer has stencil. scvk reports
            // a stencil buffer while its stencil functions do nothing yet, which is harmless:
            // the claim pass draws nothing and the draw pass then draws every decal.
            // The driver copies its whole mode record. Leave room in case a driver's record is
            // longer than the header's (the game's own may be 56 bytes).
            struct PaddedMode
            {
                sGDMode mode;
                std::byte spare[64];
            } info{};
            driver->GetVideoModeInfo(info.mode);
            stencilCheckedDriver_ = driver;
            stencilCheckedResult_ = info.mode.supportsStencilBuffer;
        }
        return stencilCheckedResult_;
    }

    void TerrainDecalHook::LogRecoveryMethod_(const int method)
    {
        if (method == loggedRecoveryMethod_) {
            return;
        }
        loggedRecoveryMethod_ = method;
        switch (method) {
        case kRecoveryComposite:
            LOG_INFO("TerrainDecalHook: decals are drawn again over shadows as one blended layer");
            break;
        case kRecoveryStencil:
            LOG_INFO("TerrainDecalHook: decals are drawn again over shadows once per pixel, using the stencil buffer");
            break;
        case kRecoveryReplay:
            LOG_INFO("TerrainDecalHook: decals are drawn again over shadows one by one, overlapping decals come out a little darker");
            break;
        case kRecoveryZeroOpacity:
            LOG_INFO("TerrainDecalHook: decals are not drawn again over shadows, because TerrainDecalShadowRecoveryOpacityScale is 0");
            break;
        default:
            LOG_INFO("TerrainDecalHook: decals are not drawn again over shadows, because EnableCustomTerrainDecalRenderer is false");
            break;
        }
    }

    void TerrainDecalHook::SetLastError_(std::string message)
    {
        lastError_ = std::move(message);
    }
}
