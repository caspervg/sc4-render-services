#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "clipper2/clipper.h"

namespace TerrainPaint
{
    // The game's terrain cells are 16 m squares with corners on multiples of 16.
    constexpr float kCellSize = 16.0f;

    struct MeshVertex
    {
        float x = 0.0f;
        float z = 0.0f;
        // 1 inside the soft edge, falling to 0 at the outline.
        float alpha = 1.0f;
        // Texture coordinates, from the UV map when the paint has one.
        float u = 0.0f;
        float v = 0.0f;
    };

    // Worn paint: patches cut out of about `amount` of the paint, `patchMeters` across.
    struct Wear
    {
        float amount = 0.0f;
        float patchMeters = 0.5f;
        uint32_t seed = 0;
    };

    // A UV map corner: a ground point and its texture coordinates.
    struct UvVertex
    {
        float x = 0.0f;
        float z = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
    };

    // A terrain cell is drawn as two triangles over its corners c00 (x0, z0), c01 (x0, z1),
    // c11 (x1, z1) and c10 (x1, z0): lower = c00 c01 c11 and upper = c00 c11 c10, the split the
    // decal renderer uses. Each half of the painted area is kept apart so its vertices can take
    // their height from the right terrain triangle.
    struct CellMesh
    {
        // Three vertices per triangle.
        std::vector<MeshVertex> lower;
        std::vector<MeshVertex> upper;

        [[nodiscard]] bool Empty() const noexcept { return lower.empty() && upper.empty(); }
    };

    struct CellRange
    {
        int32_t minX = 0;
        int32_t minZ = 0;
        int32_t maxX = -1;  // inclusive
        int32_t maxZ = -1;

        [[nodiscard]] bool Empty() const noexcept { return maxX < minX || maxZ < minZ; }
    };

    struct Ring
    {
        std::vector<std::pair<float, float>> points;
    };

    // Which of several textures a terrain cell gets: by weight, the same every time for the same
    // seed and cell. Negative weights count as 0; with no positive weight, the first texture.
    [[nodiscard]] uint32_t PickTexture(std::span<const float> weights, uint32_t seed, int32_t cellX, int32_t cellZ);

    // The painted area of one paint, cut into terrain cells. Cells are cut on first use and kept,
    // so a large paint costs only what is on screen.
    class Mesh
    {
    public:
        Mesh() = default;
        // uvMap: triangles, three corners each. With one, every vertex gets its texture
        // coordinates from the triangle it lies in, and the pieces are cut along the triangles
        // so the coordinates follow them exactly; area outside the map is left out.
        // Wear is cut cell by cell, when a cell is first drawn, so it costs only what is on screen.
        Mesh(std::span<const Ring> rings, float featherMeters, std::span<const UvVertex> uvMap = {}, Wear wear = {});

        // A paint that changed shape is mostly the same: cells whose paint did not change (with
        // what is near enough to matter) are taken from the paint as it was, not cut again.
        void ReuseCellsFrom(std::unique_ptr<Mesh> previous);

        [[nodiscard]] bool HasArea() const noexcept { return !area_.empty(); }
        [[nodiscard]] bool HasUvMap() const noexcept { return !uvMap_.empty(); }
        [[nodiscard]] const CellRange& Cells() const noexcept { return cells_; }

        // Empty when the cell is outside the area.
        [[nodiscard]] const CellMesh& Cell(int32_t cellX, int32_t cellZ) const;

    private:
        struct Edge
        {
            double ax, az, bx, bz;
        };

        [[nodiscard]] CellMesh BuildCell_(int32_t cellX, int32_t cellZ) const;
        [[nodiscard]] float AlphaAt_(double x, double z) const;
        // The area in a cell as nested regions from the outline inwards, each with whether it is
        // the solid core: the soft-edge bands, then the core. Without a soft edge, only the core.
        [[nodiscard]] std::vector<std::pair<Clipper2Lib::PathsD, bool>> RegionsIn_(const Clipper2Lib::RectD& cellRect) const;
        [[nodiscard]] std::vector<MeshVertex> MapUv_(const std::vector<MeshVertex>& triangles, uint64_t cellKey) const;
        // The patches worn away in a cell, sampled where it has paint; empty without wear.
        [[nodiscard]] Clipper2Lib::PathsD WearPatches_(const Clipper2Lib::RectD& cellRect, int32_t cellX, int32_t cellZ) const;
        [[nodiscard]] static uint64_t Key_(int32_t cellX, int32_t cellZ) noexcept;
        // Everything a cell's mesh is made from: the area around the cell and the UV map there,
        // in an order that does not depend on how the rings happen to start.
        [[nodiscard]] std::vector<double> Signature_(int32_t cellX, int32_t cellZ) const;
        [[nodiscard]] bool SameSettings_(const Mesh& other) const noexcept;

        Clipper2Lib::PathsD area_;
        float featherMeters_ = 0.0f;
        Wear wear_{};
        CellRange cells_{};
        // Outline edges by the 2 m squares within featherMeters_ of them, for the soft-edge distance.
        std::unordered_map<uint64_t, std::vector<Edge>> edgesNearCell_;
        std::vector<UvVertex> uvMap_;
        // UV map triangles (by first corner) by the cells their bounds touch.
        std::unordered_map<uint64_t, std::vector<uint32_t>> uvTrianglesNearCell_;
        struct CachedCell
        {
            std::vector<double> signature;
            CellMesh mesh;
        };
        mutable std::unordered_map<uint64_t, CachedCell> cellCache_;
        std::unique_ptr<Mesh> previous_;
    };
}
