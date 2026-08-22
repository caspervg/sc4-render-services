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
        };

        explicit TerrainDecalHook(Options options = {});
        ~TerrainDecalHook();

        [[nodiscard]] bool Install();
        void Uninstall();

        [[nodiscard]] bool IsInstalled() const noexcept;
        [[nodiscard]] std::string_view GetLastError() const noexcept;

        void SetOverlayUvWindow(uint32_t overlayId, const TerrainDecalUvWindow& uvWindow);
        [[nodiscard]] bool RemoveOverlayUvWindow(uint32_t overlayId) noexcept;
        void ClearOverlayUvWindows() noexcept;
        [[nodiscard]] bool TryGetOverlayUvWindow(uint32_t overlayId, TerrainDecalUvWindow& uvWindow) const noexcept;
        void SetOverlayOverridesResolver(OverlayOverridesResolver resolver, void* userData) noexcept;

    private:
        using DrawRectFn = void(__thiscall*)(void*, SC4DrawContext*, const cRZRect*);
        using DrawOverlayPassFn = void(__thiscall*)(void*, const float*, SC4DrawContext*, int*);
        using SetTexTransform4Fn = void(__thiscall*)(SC4DrawContext*, const float*, int);

        static void __fastcall DrawRectCallThunk(void* overlayManager, void*, SC4DrawContext* drawContext, const cRZRect* rect);
        static void __fastcall DrawShadowsCallThunk(void* overlayManager, void*, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        static void __fastcall DrawShadowsRoughCallThunk(void* overlayManager, void*, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        static void __fastcall SetTexTransform4CallThunk(SC4DrawContext* drawContext, void*, const float* matrix, int stage);

        void HandleDrawRectCall_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect);
        void HandleDrawShadowsCall_(const RelativeCallPatch& patch, void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        void HandleSetTexTransform4Call_(SC4DrawContext* drawContext, const float* matrix, int stage);
        void CallOriginalDrawRect_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect) const;
        void CallOriginalOverlayPass_(const RelativeCallPatch& patch, void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds) const;
        void CallOriginalSetTexTransform4_(SC4DrawContext* drawContext, const float* matrix, int stage) const;
        void ReplayManagedDecalsAfterShadows_(void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        bool CompositeManagedDecalsAfterShadows_(void* overlayManager, const float* worldToScreenMatrix, SC4DrawContext* drawContext, int* decalIds);
        bool EnsureRecoveryTarget_(IDirect3DDevice7* device, IDirectDrawSurface7* originalTarget);
        void ReleaseRecoveryTarget_() noexcept;
        void SetLastError_(std::string message);

    private:
        Options options_;
        std::optional<HookAddresses> addresses_;
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
        IDirect3DDevice7* recoveryDevice_ = nullptr;
        IDirectDrawSurface7* recoveryTarget_ = nullptr;
        IDirectDrawSurface7* recoveryDepth_ = nullptr;
        uint32_t recoveryWidth_ = 0;
        uint32_t recoveryHeight_ = 0;

        static TerrainDecalHook* sActiveHook_;
    };
}
