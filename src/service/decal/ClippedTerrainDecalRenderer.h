#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "cRZRect.h"
#include "public/cIGZTerrainDecalService.h"
#include "public/cIGZTerrainPaintService.h"
#include "TerrainDecalSymbols.h"

class SC4DrawContext;
class cISTETerrain;
class cISTETerrainView;

namespace TerrainPaint
{
    class Mesh;
}

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
        ShadowRecoveryCapture,
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

    // The game's terrain vertex, 32 bytes, as DrawPrims takes it with vertex format 0x0B.
    struct TerrainVertex
    {
        float x;
        float y;
        float z;
        uint32_t diffuse;
        float u;
        float v;
        float extra0;
        float extra1;
    };

    static_assert(sizeof(TerrainVertex) == 0x20, "TerrainVertex must match the game's terrain vertex layout.");

    struct DrawRequest
    {
        void* overlayManager = nullptr;
        SC4DrawContext* drawContext = nullptr;
        const cRZRect* rect = nullptr;
        const std::byte* overlaySlotBase = nullptr;
        const float* activeTexTransform = nullptr;
        int activeTexTransformStage = -1;
        const HookAddresses* addresses = nullptr;
        DrawMode mode = DrawMode::Normal;
        // Changes with every decal pass; the shadow passes after a decal pass share its number.
        uint32_t pass = 0;
        // Called right before (before = true) and right after the overlay's geometry is
        // submitted, once the context is set up for it. Lets the caller add driver state for
        // that one draw.
        void (*aroundDraw)(void* userData, SC4DrawContext* drawContext, bool before) = nullptr;
        void* aroundDrawUserData = nullptr;
    };

    // What the decal service set for one of its decals.
    struct DecalOverrides
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

    // One texture of a terrain paint. The paint has one overlay per texture; each draws the
    // cells that pick its texture.
    // What the renderer keeps of one paint overlay between frames: the overlay's share of the
    // paint, cut into cells and placed on the cells' terrain triangles by weights, so a frame only
    // has to read the terrain under it. Built a row of cells at a time, when the row is first on
    // screen. Heights and colours are read every frame, so it never goes stale when the terrain
    // changes; it is dropped whenever the paint changes.
    struct PaintDrawCache
    {
        struct PlacedVertex
        {
            float x;
            float z;
            // Barycentric weights of the terrain triangle's first two corners.
            float wa;
            float wb;
            float alpha;
            // Texture coordinates, for a paint with a UV map.
            float u;
            float v;
        };

        struct CellSpan
        {
            int32_t cellX;
            uint32_t first;       // into Row::vertices
            uint32_t lowerCount;  // then upperCount vertices on the upper triangle
            uint32_t upperCount;
        };

        struct Row
        {
            bool built = false;
            // The cells [builtX0, builtX1) of the row that are placed: what was on screen so far.
            int32_t builtX0 = 0;
            int32_t builtX1 = 0;
            std::vector<CellSpan> cells;  // by cellX
            std::vector<PlacedVertex> vertices;
        };

        // Rows from the paint's first cell row.
        std::vector<Row> rows;
        // The vertices laid in the last decal pass, which the shadow passes after it draw again.
        std::vector<TerrainVertex> lastDrawn;
        uint32_t lastDrawnPass = 0;
        bool lastDrawnValid = false;
    };

    struct PaintOverlay
    {
        const TerrainPaint::Mesh* mesh = nullptr;
        const TerrainPaintStyle* style = nullptr;
        uint32_t textureIndex = 0;
        // Texture coordinates from ground coordinates: u = a x + b z + c, v = d x + e z + f.
        std::array<float, 6> uvFromGround{};
        PaintDrawCache* cache = nullptr;
    };

    struct ManagedOverlay
    {
        enum class Kind
        {
            Decal,
            Paint,
        };

        Kind kind = Kind::Decal;
        DecalOverrides decal{};
        PaintOverlay paint{};
    };

    // Implemented by the service that owns the overlays the renderer customises.
    class ManagedOverlaySource
    {
    public:
        // False for overlays the service does not manage; the game draws those itself.
        virtual bool ResolveOverlay(const void* overlayManager, uint32_t overlayId, ManagedOverlay& out) = 0;
        // The draw order of an overlay, 0 for those the service does not manage. The second
        // value orders overlays with equal draw order: 0 for the game's, then creation order.
        virtual std::pair<int32_t, uint32_t> OverlayDrawOrder(const void* overlayManager, uint32_t overlayId) = 0;

    protected:
        ~ManagedOverlaySource() = default;
    };

    class ClippedTerrainDecalRenderer final
    {
    public:
        explicit ClippedTerrainDecalRenderer(RendererOptions options = {});

        void SetOptions(const RendererOptions& options) noexcept;
        [[nodiscard]] const RendererOptions& GetOptions() const noexcept;
        void SetOverlaySource(ManagedOverlaySource* source) noexcept;

        [[nodiscard]] DrawResult Draw(const DrawRequest& request);

    private:
        RendererOptions options_;
        ManagedOverlaySource* source_ = nullptr;
    };
}
