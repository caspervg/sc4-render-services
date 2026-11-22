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
        // Time for cutting and placing terrain paint rows per frame, over all paints. 0: no cap.
        float paintRowBudgetMs = 4.0f;
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
        uint32_t frame = 0;
    };

    // One texture of a terrain paint. The paint has one overlay per texture; each draws the
    // cells that pick its texture.
    // What the renderer keeps of one paint overlay between frames: the overlay's share of the
    // paint, cut into cells and placed on the cells' terrain triangles by weights, so a frame only
    // has to read the terrain under it. Built a row of cells at a time, when the row is first on
    // screen. Heights and colours are read every frame, so it never goes stale when the terrain
    // changes; a row is placed again when one of its cells changes diagonal, and it is all
    // dropped whenever the paint changes.
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
            bool flipped;         // the cell's diagonal when it was placed
            uint8_t frame;
        };

        struct Row
        {
            bool built = false;
            // The cache's generation it was built in: older rows are drawn until built again.
            uint32_t generation = 0;
            // The cells [builtX0, builtX1) of the row that are placed: what was on screen so far.
            int32_t builtX0 = 0;
            int32_t builtX1 = 0;
            std::vector<CellSpan> cells;  // by cellX
            std::vector<PlacedVertex> vertices;
        };

        struct FrameRange
        {
            uint32_t frame;
            uint32_t first;  // into lastDrawn
            uint32_t count;
        };

        // Rows from cell row rowsMinZ.
        std::vector<Row> rows;
        int32_t rowsMinZ = 0;
        uint32_t generation = 0;
        // The texture frames the rows picked from.
        uint32_t frames = 0;
        // The vertices laid in the last decal pass, which the shadow passes after it draw again.
        std::vector<TerrainVertex> lastDrawn;
        // Empty when they all use frame 0.
        std::vector<FrameRange> lastDrawnFrames;
        uint32_t lastDrawnPass = 0;
        bool lastDrawnValid = false;

        // This overlay's part of the row budget.
        uint32_t budgetFrame = 0;
        double budgetSpentMs = 0.0;
        bool budgetStarved = false;

        // The paint changed: its rows are built again, and drawn as they were until then.
        void Invalidate() noexcept
        {
            ++generation;
            lastDrawnValid = false;
        }
    };

    // Caps the time cutting and placing paint rows takes per frame. Each overlay with rows to
    // build gets an equal share; one held back by the others goes first in the next frame.
    class RowBudget
    {
    public:
        void SetFrameMs(const float ms) noexcept { frameMs_ = ms; }
        void BeginFrame(double nowMs) noexcept;
        [[nodiscard]] bool MayBuild(PaintDrawCache& cache, double nowMs) noexcept;
        void Spend(PaintDrawCache& cache, double ms) noexcept;

    private:
        // Without a frame signal, a frame ends after this long.
        static constexpr double kMaxFrameMs = 100.0;

        double frameMs_ = 4.0;
        double frameStartMs_ = 0.0;
        double spentMs_ = 0.0;
        uint32_t frame_ = 1;
        uint32_t waiting_ = 0;
        uint32_t waitingBefore_ = 1;
    };

    inline void RowBudget::BeginFrame(const double nowMs) noexcept
    {
        waitingBefore_ = waiting_ > 0 ? waiting_ : 1u;
        waiting_ = 0;
        spentMs_ = 0.0;
        frameStartMs_ = nowMs;
        ++frame_;
    }

    inline bool RowBudget::MayBuild(PaintDrawCache& cache, const double nowMs) noexcept
    {
        if (!(frameMs_ > 0.0)) {
            return true;
        }
        if (nowMs - frameStartMs_ > kMaxFrameMs) {
            BeginFrame(nowMs);
        }
        if (cache.budgetFrame != frame_) {
            cache.budgetFrame = frame_;
            cache.budgetSpentMs = 0.0;
            ++waiting_;
        }
        const double share = frameMs_ / waitingBefore_;
        if (cache.budgetSpentMs >= share) {
            return false;
        }
        if (spentMs_ < frameMs_ || cache.budgetStarved) {
            return true;
        }
        cache.budgetStarved = true;
        return false;
    }

    inline void RowBudget::Spend(PaintDrawCache& cache, const double ms) noexcept
    {
        spentMs_ += ms;
        cache.budgetSpentMs += ms;
        cache.budgetStarved = false;
    }

    struct PaintOverlay
    {
        const TerrainPaint::Mesh* mesh = nullptr;
        const TerrainPaintStyle* style = nullptr;
        uint32_t paintId = 0;
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
    // Passes the service draws some overlays in, apart from their overlay manager's own pass.
    enum class OwnPass : uint8_t
    {
        None,
        // Right after the terrain, before lots and networks.
        UnderLots,
        // After the sidewalks of networks, before their surfaces.
        OverSidewalks,
        Count,
    };

    class ManagedOverlaySource
    {
    public:
        // False for overlays the service does not manage; the game draws those itself.
        virtual bool ResolveOverlay(const void* overlayManager, uint32_t overlayId, ManagedOverlay& out) = 0;
        // The draw order of an overlay, 0 for those the service does not manage. The second
        // value orders overlays with equal draw order: 0 for the game's, then creation order.
        virtual std::pair<int32_t, uint32_t> OverlayDrawOrder(const void* overlayManager, uint32_t overlayId) = 0;
        // The pass of its own an overlay is drawn in, None for the overlay manager's pass.
        virtual OwnPass OverlayOwnPass(const void* overlayManager, uint32_t overlayId) = 0;
        // Whether any overlay of the manager is drawn in that pass. Cheap: asked every pass.
        virtual bool HasOverlaysIn(const void* overlayManager, OwnPass pass) = 0;

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
        // Starts the next frame's row budget.
        void BeginFrame() noexcept;

    private:
        RendererOptions options_;
        RowBudget rowBudget_;
        ManagedOverlaySource* source_ = nullptr;
    };
}
