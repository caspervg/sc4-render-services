#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "cRZRect.h"
#include "public/cIGZTerrainDecalService.h"
#include "TerrainDecalSymbols.h"

class SC4DrawContext;
class cISTETerrain;
class cISTETerrainView;

namespace TerrainDecal
{
    enum class DrawResult
    {
        FallThroughToVanilla,
        Handled,
    };

    enum class DrawMode
    {
        Normal,
        ShadowRecovery,
    };

    struct RendererOptions
    {
        bool enableClippedRendering = false;
        // Used when an overlay's overrides specify depthOffset == -1.
        // Vanilla decals = 2, shadows = 3, above-shadows = 4.
        int customDefaultDepthOffset = 2;
        int shadowRecoveryDepthOffset = 4;
        float shadowRecoveryOpacityScale = 0.25f;
    };

    struct DrawRequest
    {
        void* overlayManager = nullptr;
        SC4DrawContext* drawContext = nullptr;
        const cRZRect* rect = nullptr;
        const std::byte* overlaySlotBase = nullptr;
        const float* activeTexTransform = nullptr;
        int activeTexTransformStage = -1;
        std::ptrdiff_t overlayRectOffset = 0;
        const HookAddresses* addresses = nullptr;
        cISTETerrain* terrain = nullptr;
        cISTETerrainView* terrainView = nullptr;
        DrawMode mode = DrawMode::Normal;
    };

    struct TerrainDecalOverlayOverrides
    {
        bool hasUvWindow = false;
        TerrainDecalUvWindow uvWindow{};
        float aspectMultiplier = 1.0f;
        float uvScaleU = 1.0f;
        float uvScaleV = 1.0f;
        float uvOffset = 0.0f;
        // Mirror the texture in place (u' = 1 - u), per kTerrainDecalFlagMirror.
        bool mirror = false;
        // -1 means "use RendererOptions::customDefaultDepthOffset".
        int depthOffset = -1;
    };

    [[nodiscard]] inline bool HasDecalModifiers(const TerrainDecalOverlayOverrides& o) noexcept
    {
        return o.aspectMultiplier != 1.0f || o.uvScaleU != 1.0f ||
               o.uvScaleV != 1.0f || o.uvOffset != 0.0f || o.mirror;
    }

    using OverlayOverridesResolver = bool (*)(void* overlayManager, uint32_t overlayId,
                                              TerrainDecalOverlayOverrides& overrides, void* userData);

    class ClippedTerrainDecalRenderer final
    {
    public:
        explicit ClippedTerrainDecalRenderer(RendererOptions options = {});

        void SetOptions(const RendererOptions& options) noexcept;
        [[nodiscard]] const RendererOptions& GetOptions() const noexcept;

        void SetOverlayUvWindow(uint32_t overlayId, const TerrainDecalUvWindow& uvWindow);
        [[nodiscard]] bool RemoveOverlayUvWindow(uint32_t overlayId) noexcept;
        void ClearOverlayUvWindows() noexcept;
        [[nodiscard]] bool TryGetOverlayUvWindow(uint32_t overlayId, TerrainDecalUvWindow& uvWindow) const noexcept;
        void SetOverlayOverridesResolver(OverlayOverridesResolver resolver, void* userData) noexcept;

        [[nodiscard]] DrawResult Draw(const DrawRequest& request);

    private:
        RendererOptions options_;
        std::unordered_map<uint32_t, TerrainDecalUvWindow> overlayUvWindows_;
        OverlayOverridesResolver overlayOverridesResolver_ = nullptr;
        void* overlayOverridesResolverUserData_ = nullptr;
    };
}
