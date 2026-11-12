#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ClippedTerrainDecalRenderer.h"
#include "RelativeCallPatch.h"
#include "TerrainDecalSymbols.h"

class SC4DrawContext;
class cIGZGDriver;
struct IDirect3DDevice7;
struct IDirectDrawSurface7;

namespace TerrainDecal
{
    class TerrainDecalHook final
    {
    public:
        struct Options
        {
            bool installEnabled = true;
            bool enableCustomRenderer = true;
            // Default custom-rendered decal depth offset, used when a state has depthOffset == -1.
            int customDefaultDepthOffset = 2;
            float shadowRecoveryOpacityScale = 0.25f;
            // Limit the shadow recovery redraw to one layer per pixel with the stencil buffer,
            // on drivers that have one.
            bool shadowRecoveryUseStencil = true;
        };

        explicit TerrainDecalHook(Options options = {});
        ~TerrainDecalHook();

        [[nodiscard]] bool Install();
        void Uninstall();

        [[nodiscard]] bool IsInstalled() const noexcept;
        [[nodiscard]] std::string_view GetLastError() const noexcept;

        // The service whose overlays get custom drawing and a draw order.
        void SetOverlaySource(ManagedOverlaySource* source) noexcept;

        // Whether something calls DrawUnderLots after the terrain every time the static view is
        // drawn. Until then, overlays under lots are drawn with the others.
        void SetUnderLotsPassAvailable(bool available) noexcept;
        // Draws the overlays of the manager that go under lots and networks, and nothing else.
        // Call it after the terrain and before the static models.
        void DrawUnderLots(void* overlayManager, SC4DrawContext* drawContext);

    private:
        [[nodiscard]] std::array<RelativeCallPatch*, 5> Patches_() noexcept;

        using DrawRectFn = void(__thiscall*)(void*, SC4DrawContext*, const cRZRect*);
        using DrawOverlayPassFn = void(__thiscall*)(void*, const float*, SC4DrawContext*, int*);
        using DrawOverlaysFn = void(__thiscall*)(void*, SC4DrawContext*);
        using SetTexTransform4Fn = void(__thiscall*)(SC4DrawContext*, const float*, int);

        static void __fastcall DrawDecalsCallThunk(void* overlayManager, void*, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        static void __fastcall DrawRectCallThunk(void* overlayManager, void*, SC4DrawContext* drawContext, const cRZRect* rect);
        static void __fastcall DrawShadowsCallThunk(void* overlayManager, void*, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        static void __fastcall DrawShadowsRoughCallThunk(void* overlayManager, void*, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        static void __fastcall SetTexTransform4CallThunk(SC4DrawContext* drawContext, void*, const float* matrix, int stage);
        static void AroundStencilRecoveryDraw_(void* userData, SC4DrawContext* drawContext, bool before);

        void SortByDrawOrder_(const void* overlayManager, int* decalIds) const;
        // Keeps the overlays that belong in the pass being drawn: in the under-lots pass only
        // those under lots, otherwise all but those.
        void FilterByLayer_(const void* overlayManager, int* decalIds) const;
        void HandleDrawRectCall_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect);
        void HandleDrawShadowsCall_(const RelativeCallPatch& patch, void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        void HandleSetTexTransform4Call_(SC4DrawContext* drawContext, const float* matrix, int stage);
        void CallOriginalDrawRect_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect) const;
        void CallOriginalOverlayPass_(const RelativeCallPatch& patch, void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds) const;
        void CallOriginalSetTexTransform4_(SC4DrawContext* drawContext, const float* matrix, int stage) const;
        void ReplayManagedDecalsAfterShadows_(void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        bool CompositeManagedDecalsAfterShadows_(void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        [[nodiscard]] bool DriverHasStencil_(cIGZGDriver* driver);
        void LogRecoveryMethod_(int method);
        bool EnsureRecoveryTarget_(IDirect3DDevice7* device, IDirectDrawSurface7* originalTarget);
        void ReleaseRecoveryTarget_() noexcept;
        void SetLastError_(std::string message);

    private:
        Options options_;
        std::optional<HookAddresses> addresses_;
        ManagedOverlaySource* overlaySource_ = nullptr;
        RelativeCallPatch drawDecalsCallSitePatch_;
        RelativeCallPatch callSitePatch_;
        RelativeCallPatch drawShadowsCallSitePatch_;
        RelativeCallPatch drawShadowsRoughCallSitePatch_;
        RelativeCallPatch setTexTransformCallSitePatch_;
        ClippedTerrainDecalRenderer renderer_;
        std::string lastError_{};
        std::array<float, 16> currentTexTransform_{};
        int currentTexTransformStage_ = -1;
        bool currentTexTransformValid_ = false;
        bool shadowRecoveryActive_ = false;
        bool shadowRecoveryCaptureActive_ = false;
        // Numbers the decal passes; the shadow passes after one keep its number.
        uint32_t overlayPass_ = 0;
        bool underLotsPassAvailable_ = false;
        // DrawUnderLots is drawing: only overlays under lots, and no shadows.
        bool underLotsPassActive_ = false;

        // Stencil shadow recovery: the claim pass marks each pixel with the topmost decal's
        // number, the draw pass then draws each decal only where it holds the mark.
        enum class StencilPass : uint8_t { None, Claim, Draw };
        StencilPass stencilPass_ = StencilPass::None;
        uint32_t stencilDecalNumber_ = 0;
        cIGZGDriver* stencilCheckedDriver_ = nullptr;
        bool stencilCheckedResult_ = false;
        int loggedRecoveryMethod_ = -1;
        IDirect3DDevice7* recoveryDevice_ = nullptr;
        IDirectDrawSurface7* recoveryTarget_ = nullptr;
        IDirectDrawSurface7* recoveryDepth_ = nullptr;
        uint32_t recoveryWidth_ = 0;
        uint32_t recoveryHeight_ = 0;

        static TerrainDecalHook* sActiveHook_;
    };
}
