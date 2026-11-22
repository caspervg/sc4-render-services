// Times cutting the cells of a large patchy paint, like a "soft patches" layer: run
// TerrainPaintMeshBench.exe, it prints the time per cell.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "service/decal/TerrainPaintMesh.h"

using TerrainPaint::Mesh;
using TerrainPaint::Ring;

int main()
{
    // About 500 cells of 16 m, covered by blobs a few metres across with dense outlines.
    constexpr float kSide = 360.0f;
    std::vector<Ring> rings;
    uint32_t state = 12345;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / 4294967296.0f;
    };
    for (int i = 0; i < 2500; ++i) {
        const float cx = next() * kSide;
        const float cz = next() * kSide;
        const float radius = 1.5f + next() * 4.5f;
        Ring ring;
        constexpr int kPoints = 32;
        for (int p = 0; p < kPoints; ++p) {
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(p) / kPoints;
            const float reach = radius * (0.8f + 0.2f * next());
            ring.points.emplace_back(cx + reach * std::cos(angle), cz + reach * std::sin(angle));
        }
        rings.push_back(std::move(ring));
    }

    for (const float feather : {0.0f, 2.0f}) {
        const auto start = std::chrono::steady_clock::now();
        const Mesh mesh(rings, feather);
        const auto built = std::chrono::steady_clock::now();
        size_t cells = 0;
        size_t vertices = 0;
        for (int32_t z = mesh.Cells().minZ; z <= mesh.Cells().maxZ; ++z) {
            for (int32_t x = mesh.Cells().minX; x <= mesh.Cells().maxX; ++x) {
                const auto& cell = mesh.Cell(x, z);
                vertices += cell.lower.size() + cell.upper.size();
                ++cells;
            }
        }
        const auto done = std::chrono::steady_clock::now();
        const double meshMs = std::chrono::duration<double, std::milli>(built - start).count();
        const double cellMs = std::chrono::duration<double, std::milli>(done - built).count();
        std::printf("feather %.0f m: mesh %.1f ms, %zu cells in %.1f ms (%.3f ms per cell), %zu vertices\n", feather,
                    meshMs, cells, cellMs, cellMs / static_cast<double>(cells), vertices);
    }
    return 0;
}
