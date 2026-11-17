#pragma once

#include <cstdint>

// What drawing terrain paint costs per frame, for the "Terrain paint stats" panel
// (INI: ShowTerrainPaintStats).
namespace TerrainPaintStats
{
    enum class Pass : uint8_t
    {
        OverLots,
        UnderLots,
        OverShadows,
        ShadowRecovery,
        ShadowRecoveryCapture,
        Count,
    };

    struct Counters
    {
        uint64_t overlays = 0;
        uint64_t rows = 0;       // BuildPaintRow calls
        uint64_t cells = 0;      // cells visited while lifting
        uint64_t vertices = 0;   // vertices lifted
        uint64_t drawCalls = 0;
        double buildMs = 0.0;
        double liftMs = 0.0;     // lifting and the rest of the setup before the draw
        double submitMs = 0.0;
    };

    // The renderer measures only while this is set.
    inline bool enabled = false;

    // One CreatePaint, SetPaintShape or SetPaintStyle call. Cells are cut when first drawn, so
    // that work shows up in the draw passes' build time instead.
    struct Edit
    {
        const char* kind = "";
        uint32_t paintId = 0;
        double ms = 0.0;
        double meshMs = 0.0;      // making the mesh again (BuildMesh)
        bool newMesh = false;
        bool newOverlays = false;  // the paint's overlays were made again
    };

    // The call being measured, if any.
    inline Edit* currentEdit = nullptr;

    [[nodiscard]] double NowMs() noexcept;
    void Add(Pass pass, uint32_t paintId, uint32_t textureCount, const Counters& counters);
    void AddEdit(const Edit& edit);

    // Measures a paint edit call from construction to destruction.
    class EditScope
    {
    public:
        EditScope(const char* const kind, const uint32_t paintId) noexcept
        {
            if (enabled && !currentEdit) {
                edit_.kind = kind;
                edit_.paintId = paintId;
                start_ = NowMs();
                currentEdit = &edit_;
            }
        }

        ~EditScope()
        {
            if (currentEdit == &edit_) {
                currentEdit = nullptr;
                edit_.ms = NowMs() - start_;
                AddEdit(edit_);
            }
        }

        EditScope(const EditScope&) = delete;
        EditScope& operator=(const EditScope&) = delete;

    private:
        Edit edit_{};
        double start_ = 0.0;
    };
    // Once per frame. Averages are published every second and logged every 10 seconds.
    void EndFrame();
    void RenderPanel();
}
