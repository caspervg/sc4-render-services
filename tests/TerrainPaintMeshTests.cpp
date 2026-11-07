#include <memory>
// Checks for the terrain paint mesh: run TerrainPaintMeshTests.exe, it prints "ok" or aborts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "service/decal/TerrainPaintMesh.h"

using TerrainPaint::CellMesh;
using TerrainPaint::Mesh;
using TerrainPaint::MeshVertex;
using TerrainPaint::Ring;
using TerrainPaint::UvVertex;

namespace
{
    void Check(const bool condition, const char* what, const int line)
    {
        if (!condition) {
            std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
            std::abort();
        }
    }
#define CHECK(x) Check((x), #x, __LINE__)

    bool Near(const double a, const double b, const double tolerance) { return std::abs(a - b) <= tolerance; }

    Ring Rect(const float x0, const float z0, const float x1, const float z1)
    {
        return Ring{{{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}};
    }

    double TriangleArea(const std::vector<MeshVertex>& v, const size_t i)
    {
        return 0.5 * std::abs((v[i + 1].x - v[i].x) * (v[i + 2].z - v[i].z) -
                              (v[i + 1].z - v[i].z) * (v[i + 2].x - v[i].x));
    }

    // Painted area of the whole mesh, and a check that every triangle sits in its half of the
    // cell and winds the same way as the renderer's terrain triangles.
    double MeshArea(const Mesh& mesh)
    {
        double area = 0.0;
        const auto& cells = mesh.Cells();
        for (int32_t cz = cells.minZ; cz <= cells.maxZ; ++cz) {
            for (int32_t cx = cells.minX; cx <= cells.maxX; ++cx) {
                const CellMesh& cell = mesh.Cell(cx, cz);
                CHECK(cell.lower.size() % 3 == 0 && cell.upper.size() % 3 == 0);
                for (const bool lower : {true, false}) {
                    const auto& v = lower ? cell.lower : cell.upper;
                    for (size_t i = 0; i < v.size(); i += 3) {
                        const float cross = (v[i + 1].x - v[i].x) * (v[i + 2].z - v[i].z) -
                                            (v[i + 1].z - v[i].z) * (v[i + 2].x - v[i].x);
                        CHECK(cross <= 0.0f);
                        area += TriangleArea(v, i);
                        for (size_t k = i; k < i + 3; ++k) {
                            const float lx = v[k].x - cx * 16.0f;
                            const float lz = v[k].z - cz * 16.0f;
                            CHECK(lx >= -1e-3f && lx <= 16.001f && lz >= -1e-3f && lz <= 16.001f);
                            CHECK(lower ? lz >= lx - 1e-3f : lz <= lx + 1e-3f);
                            CHECK(v[k].alpha >= 0.0f && v[k].alpha <= 1.0f);
                        }
                    }
                }
            }
        }
        return area;
    }
}

int main()
{
    {
        // Two by two cells, exactly on the grid: four full cells.
        const std::vector<Ring> rings{Rect(0, 0, 32, 32)};
        const Mesh mesh(rings, 0.0f);
        CHECK(mesh.HasArea());
        CHECK(mesh.Cells().minX == 0 && mesh.Cells().maxX == 1);
        CHECK(mesh.Cells().minZ == 0 && mesh.Cells().maxZ == 1);
        CHECK(mesh.Cell(0, 0).lower.size() == 3 && mesh.Cell(0, 0).upper.size() == 3);
        CHECK(Near(MeshArea(mesh), 1024.0, 1e-2));
        CHECK(mesh.Cell(5, 5).Empty());
        CHECK(mesh.Cell(-1, 0).Empty());
    }
    {
        // Off the grid, in negative coordinates too.
        const std::vector<Ring> rings{Rect(-5, 3, 27.5f, 40)};
        const Mesh mesh(rings, 0.0f);
        CHECK(mesh.Cells().minX == -1 && mesh.Cells().maxX == 1);
        CHECK(Near(MeshArea(mesh), 32.5 * 37.0, 0.05));
    }
    {
        // A ring inside a ring is a hole (even-odd), in either winding.
        const Ring outer = Rect(0, 0, 48, 48);
        const Ring hole = Rect(10, 10, 30, 20);
        const std::vector<Ring> rings{outer, hole};
        CHECK(Near(MeshArea(Mesh(rings, 0.0f)), 48.0 * 48.0 - 20.0 * 10.0, 0.05));
        Ring reversed = hole;
        std::reverse(reversed.points.begin(), reversed.points.end());
        const std::vector<Ring> rings2{outer, reversed};
        CHECK(Near(MeshArea(Mesh(rings2, 0.0f)), 48.0 * 48.0 - 20.0 * 10.0, 0.05));
    }
    {
        // Soft edge: same area, alpha 0 on the outline, 1 deep inside.
        const std::vector<Ring> rings{Rect(2, 2, 46, 30)};
        const Mesh mesh(rings, 3.0f);
        CHECK(Near(MeshArea(mesh), 44.0 * 28.0, 0.1));
        bool sawZero = false;
        bool sawOne = false;
        bool sawMiddle = false;
        const auto& cells = mesh.Cells();
        for (int32_t cz = cells.minZ; cz <= cells.maxZ; ++cz) {
            for (int32_t cx = cells.minX; cx <= cells.maxX; ++cx) {
                for (const auto* v : {&mesh.Cell(cx, cz).lower, &mesh.Cell(cx, cz).upper}) {
                    for (const MeshVertex& p : *v) {
                        const double edge = std::min({p.x - 2.0, 46.0 - p.x, p.z - 2.0, 30.0 - p.z});
                        if (edge < 1e-3) {
                            CHECK(p.alpha < 0.01f);
                            sawZero = true;
                        }
                        if (edge > 3.0 + 1e-3) {
                            CHECK(p.alpha > 0.99f);
                            sawOne = true;
                        }
                        if (edge > 0.5 && edge < 2.5) {
                            CHECK(Near(p.alpha, edge / 3.0, 0.02));
                            sawMiddle = true;
                        }
                    }
                }
            }
        }
        CHECK(sawZero && sawOne && sawMiddle);
    }
    {
        // A soft edge wider than the shape: everything fades, nothing is solid.
        const std::vector<Ring> rings{Rect(0, 0, 4, 4)};
        const Mesh mesh(rings, 10.0f);
        CHECK(Near(MeshArea(mesh), 16.0, 0.05));
        for (const MeshVertex& p : mesh.Cell(0, 0).lower) {
            CHECK(p.alpha <= 0.21f);
        }
    }
    {
        // No area: degenerate rings, too few points, nothing at all.
        const std::vector<Ring> line{Ring{{{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}}}};
        CHECK(!Mesh(line, 0.0f).HasArea());
        const std::vector<Ring> two{Ring{{{0.0f, 0.0f}, {10.0f, 0.0f}}}};
        CHECK(!Mesh(two, 0.0f).HasArea());
        CHECK(!Mesh({}, 1.0f).HasArea());
        CHECK(Mesh({}, 1.0f).Cell(0, 0).Empty());
    }

    {
        // Texture picks: stable, by weight, zero weights never chosen.
        using TerrainPaint::PickTexture;
        const float one[] = {1.0f};
        CHECK(PickTexture(one, 7, 3, 4) == 0);
        const float weights[] = {3.0f, 0.0f, 1.0f};
        int counts[3] = {};
        for (int32_t z = -50; z < 50; ++z) {
            for (int32_t x = -50; x < 50; ++x) {
                const uint32_t pick = PickTexture(weights, 42, x, z);
                CHECK(pick < 3 && pick == PickTexture(weights, 42, x, z));
                ++counts[pick];
            }
        }
        CHECK(counts[1] == 0);
        CHECK(counts[0] > 7000 && counts[0] < 8000 && counts[2] > 2000 && counts[2] < 3000);
        const float none[] = {0.0f, -1.0f};
        CHECK(PickTexture(none, 1, 0, 0) == 0);
    }

    {
        // UV map: a 40 x 4 m strip across three cells, mapped u = x / 10, v = z / 4 by two
        // triangles that cover only x up to 30. Every vertex takes the map's coordinates, and the
        // part past the map is left out.
        const std::vector<Ring> strip{Ring{{{0.0f, 2.0f}, {40.0f, 2.0f}, {40.0f, 6.0f}, {0.0f, 6.0f}}}};
        const auto corner = [](const float x, const float z) { return UvVertex{x, z, x / 10.0f, z / 4.0f}; };
        const std::vector<UvVertex> uvMap{corner(0, 0), corner(30, 0), corner(30, 8),
                                          corner(0, 0), corner(30, 8), corner(0, 8)};
        const Mesh mesh(strip, 0.0f, uvMap);
        CHECK(mesh.HasUvMap());
        CHECK(Near(MeshArea(mesh), 30.0 * 4.0, 0.05));
        bool sawVertex = false;
        for (int32_t cx = 0; cx <= 2; ++cx) {
            for (const auto* half : {&mesh.Cell(cx, 0).lower, &mesh.Cell(cx, 0).upper}) {
                for (const MeshVertex& vertex : *half) {
                    sawVertex = true;
                    CHECK(Near(vertex.u, vertex.x / 10.0, 1e-3) && Near(vertex.v, vertex.z / 4.0, 1e-3));
                    CHECK(vertex.x <= 30.001f);
                }
            }
        }
        CHECK(sawVertex);
        CHECK(Near(MeshArea(Mesh(strip, 0.0f)), 40.0 * 4.0, 0.05));
    }

    {
        // Wear: about the share asked for is cut away, the same way every time, and only where
        // there is paint.
        const std::vector<Ring> field{Ring{{{0.0f, 0.0f}, {32.0f, 0.0f}, {32.0f, 32.0f}, {0.0f, 32.0f}}}};
        const double whole = MeshArea(Mesh(field, 0.0f));
        const TerrainPaint::Wear wear{0.3f, 0.6f, 7};
        const double worn = MeshArea(Mesh(field, 0.0f, {}, wear));
        CHECK(worn < whole * 0.85 && worn > whole * 0.55);
        CHECK(Near(MeshArea(Mesh(field, 0.0f, {}, wear)), worn, 1e-6));
        CHECK(Near(MeshArea(Mesh(field, 0.0f, {}, TerrainPaint::Wear{0.0f, 0.6f, 7})), whole, 1e-6));
        // A thin line wears through in places too.
        const std::vector<Ring> line{Ring{{{0.0f, 4.0f}, {40.0f, 4.0f}, {40.0f, 4.12f}, {0.0f, 4.12f}}}};
        const double lineArea = MeshArea(Mesh(line, 0.0f));
        const double wornLine = MeshArea(Mesh(line, 0.0f, {}, TerrainPaint::Wear{0.3f, 0.09f, 3}));
        CHECK(wornLine < lineArea * 0.9 && wornLine > lineArea * 0.4);
    }

    {
        // Reusing cells: after a change far away, a cell comes out as if made afresh; the changed
        // cell is made again.
        const auto same = [](const CellMesh& a, const CellMesh& b) {
            const auto equal = [](const std::vector<MeshVertex>& x, const std::vector<MeshVertex>& y) {
                return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](const MeshVertex& p, const MeshVertex& q) {
                           return p.x == q.x && p.z == q.z && p.alpha == q.alpha && p.u == q.u && p.v == q.v;
                       });
            };
            return equal(a.lower, b.lower) && equal(a.upper, b.upper);
        };
        const TerrainPaint::Wear wear{0.3f, 0.5f, 9};
        const std::vector<Ring> before{Ring{{{2.0f, 2.0f}, {14.0f, 2.0f}, {14.0f, 14.0f}, {2.0f, 14.0f}}},
                                       Ring{{{50.0f, 2.0f}, {60.0f, 2.0f}, {60.0f, 10.0f}}}};
        const std::vector<Ring> after{Ring{{{2.0f, 2.0f}, {14.0f, 2.0f}, {14.0f, 14.0f}, {2.0f, 14.0f}}},
                                      Ring{{{50.0f, 2.0f}, {62.0f, 4.0f}, {60.0f, 12.0f}}}};
        auto old = std::make_unique<Mesh>(before, 1.0f, std::span<const UvVertex>{}, wear);
        const CellMesh oldNear = old->Cell(0, 0);
        (void)old->Cell(3, 0);
        Mesh changed(after, 1.0f, {}, wear);
        changed.ReuseCellsFrom(std::move(old));
        const Mesh fresh(after, 1.0f, {}, wear);
        CHECK(same(changed.Cell(0, 0), fresh.Cell(0, 0)) && same(changed.Cell(0, 0), oldNear));
        CHECK(same(changed.Cell(3, 0), fresh.Cell(3, 0)));
        CHECK(!changed.Cell(3, 0).Empty());
    }

    std::puts("ok");
    return 0;
}
