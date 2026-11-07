#include "TerrainPaintMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "mapbox/earcut.hpp"

namespace TerrainPaint
{
    namespace
    {
        using namespace Clipper2Lib;

        // Millimetres are more than enough on a 16 m grid, and keep Clipper's integers small.
        constexpr int kPrecision = 3;
        // How far a rounded inner corner may stray from a true circle.
        constexpr double kArcTolerance = 0.02;
        // The soft edge is cut into this many bands, so every triangle spans at most part of it
        // and the alpha, which the GPU interpolates linearly, follows the distance closely.
        constexpr int kFeatherBands = 2;
        constexpr double kCellArea = static_cast<double>(kCellSize) * kCellSize;

        using EarcutPoint = std::array<double, 2>;
        using EarcutRing = std::vector<EarcutPoint>;

        [[nodiscard]] int32_t CellIndex(const double coordinate) noexcept
        {
            return static_cast<int32_t>(std::floor(coordinate / kCellSize));
        }

        // The soft edge looks up outline edges in squares this big, so a vertex checks only the
        // edges near it, not every edge in its terrain cell.
        constexpr double kEdgeBucketSize = 2.0;

        [[nodiscard]] int32_t EdgeBucket(const double coordinate) noexcept
        {
            return static_cast<int32_t>(std::floor(coordinate / kEdgeBucketSize));
        }

        [[nodiscard]] double DistanceToSegment(const double px, const double pz,
                                               const double ax, const double az,
                                               const double bx, const double bz) noexcept
        {
            const double dx = bx - ax;
            const double dz = bz - az;
            const double lengthSquared = dx * dx + dz * dz;
            double t = 0.0;
            if (lengthSquared > 0.0) {
                t = std::clamp(((px - ax) * dx + (pz - az) * dz) / lengthSquared, 0.0, 1.0);
            }
            const double cx = ax + t * dx - px;
            const double cz = az + t * dz - pz;
            return std::sqrt(cx * cx + cz * cz);
        }

        [[nodiscard]] EarcutRing ToEarcutRing(const PathD& path)
        {
            EarcutRing ring;
            ring.reserve(path.size());
            for (const PointD& point : path) {
                ring.push_back({point.x, point.y});
            }
            return ring;
        }

        // Calls emit(a, b, c) for every triangle of the polygons below node. Outer rings and
        // their holes go to earcut together; islands inside holes are handled on their own.
        template <typename Emit>
        void TriangulateTree(const PolyPathD& node, Emit&& emit)
        {
            for (size_t i = 0; i < node.Count(); ++i) {
                const PolyPathD* const outer = node.Child(i);
                std::vector<EarcutRing> polygon;
                polygon.push_back(ToEarcutRing(outer->Polygon()));
                for (size_t h = 0; h < outer->Count(); ++h) {
                    const PolyPathD* const hole = outer->Child(h);
                    polygon.push_back(ToEarcutRing(hole->Polygon()));
                    TriangulateTree(*hole, emit);
                }

                std::vector<EarcutPoint> flat;
                for (const EarcutRing& ring : polygon) {
                    flat.insert(flat.end(), ring.begin(), ring.end());
                }
                const std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(polygon);
                for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                    emit(flat[indices[t]], flat[indices[t + 1]], flat[indices[t + 2]]);
                }
            }
        }

        // Barycentric weights of p in triangle (a, b, c); false for a degenerate triangle.
        bool Barycentric(const double px, const double pz,
                         const double ax, const double az, const double bx, const double bz,
                         const double cx, const double cz, std::array<double, 3>& w)
        {
            const double denom = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
            if (!(std::abs(denom) > 1.0e-12)) {
                return false;
            }
            w[0] = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / denom;
            w[1] = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / denom;
            w[2] = 1.0 - w[0] - w[1];
            return true;
        }

        struct ClipPoint
        {
            double x;
            double z;
        };

        // Sutherland-Hodgman: the part of a convex polygon inside the triangle (a, b, c).
        std::vector<ClipPoint> ClipToTriangle(std::vector<ClipPoint> polygon, const UvVertex* const corners)
        {
            const double orientation = (corners[1].x - corners[0].x) * (corners[2].z - corners[0].z) -
                                       (corners[1].z - corners[0].z) * (corners[2].x - corners[0].x);
            const double sign = orientation >= 0.0 ? 1.0 : -1.0;
            for (int edge = 0; edge < 3 && polygon.size() >= 3; ++edge) {
                const UvVertex& a = corners[edge];
                const UvVertex& b = corners[(edge + 1) % 3];
                const auto side = [&](const ClipPoint& p) {
                    return sign * ((b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x));
                };
                std::vector<ClipPoint> output;
                output.reserve(polygon.size() + 1);
                ClipPoint previous = polygon.back();
                double previousSide = side(previous);
                for (const ClipPoint& current : polygon) {
                    const double currentSide = side(current);
                    if ((currentSide >= 0.0) != (previousSide >= 0.0)) {
                        const double t = previousSide / (previousSide - currentSide);
                        output.push_back({previous.x + t * (current.x - previous.x), previous.z + t * (current.z - previous.z)});
                    }
                    if (currentSide >= 0.0) {
                        output.push_back(current);
                    }
                    previous = current;
                    previousSide = currentSide;
                }
                polygon = std::move(output);
            }
            return polygon;
        }

        // A small, fast generator: the same patches every time for the same seed and cell.
        class Random
        {
        public:
            explicit Random(uint32_t seed) : state_(seed ? seed : 0x9E3779B9u) {}

            // Uniform in [0, 1).
            double Next() noexcept
            {
                state_ ^= state_ << 13;
                state_ ^= state_ >> 17;
                state_ ^= state_ << 5;
                return static_cast<double>(state_) / 4294967296.0;
            }

        private:
            uint32_t state_;
        };

        // Odd number of rings around the point: inside, as the even-odd rule counts.
        bool Inside(const PathsD& paths, const PointD point)
        {
            bool inside = false;
            for (const PathD& path : paths) {
                if (PointInPolygon(point, path) != PointInPolygonResult::IsOutside) {
                    inside = !inside;
                }
            }
            return inside;
        }

        // The decal renderer's terrain triangles wind clockwise in x/z; keep ours the same so
        // a culling state treats them alike.
        void AppendTriangle(std::vector<MeshVertex>& out, MeshVertex a, MeshVertex b, MeshVertex c)
        {
            const float cross = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
            if (cross > 0.0f) {
                std::swap(b, c);
            }
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        }
    }

    uint32_t PickTexture(const std::span<const float> weights, const uint32_t seed, const int32_t cellX, const int32_t cellZ)
    {
        if (weights.size() <= 1) {
            return 0;
        }

        double total = 0.0;
        for (const float weight : weights) {
            total += std::max(0.0f, weight);
        }
        if (!(total > 0.0)) {
            return 0;
        }

        // A murmur3 finaliser over seed and cell spreads neighbouring cells well.
        uint32_t h = seed ^ (static_cast<uint32_t>(cellX) * 0x9E3779B1u) ^ (static_cast<uint32_t>(cellZ) * 0x85EBCA77u);
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;

        double pick = (static_cast<double>(h) / 4294967296.0) * total;
        uint32_t last = 0;
        for (uint32_t i = 0; i < weights.size(); ++i) {
            const double weight = std::max(0.0f, weights[i]);
            if (weight <= 0.0) {
                continue;
            }
            last = i;
            if (pick < weight) {
                return i;
            }
            pick -= weight;
        }
        return last;
    }

    Mesh::Mesh(const std::span<const Ring> rings, const float featherMeters, const std::span<const UvVertex> uvMap,
               const Wear wear)
        : featherMeters_(std::max(0.0f, featherMeters))
        , wear_(wear)
    {
        for (size_t i = 0; i + 2 < uvMap.size(); i += 3) {
            const bool finite = std::all_of(uvMap.begin() + static_cast<std::ptrdiff_t>(i),
                                            uvMap.begin() + static_cast<std::ptrdiff_t>(i + 3), [](const UvVertex& c) {
                return std::isfinite(c.x) && std::isfinite(c.z) && std::isfinite(c.u) && std::isfinite(c.v);
            });
            if (!finite) {
                continue;
            }
            const auto first = static_cast<uint32_t>(uvMap_.size());
            uvMap_.insert(uvMap_.end(), uvMap.begin() + static_cast<std::ptrdiff_t>(i),
                          uvMap.begin() + static_cast<std::ptrdiff_t>(i + 3));
            const auto [minX, maxX] = std::minmax({uvMap[i].x, uvMap[i + 1].x, uvMap[i + 2].x});
            const auto [minZ, maxZ] = std::minmax({uvMap[i].z, uvMap[i + 1].z, uvMap[i + 2].z});
            for (int32_t cz = CellIndex(minZ); cz <= CellIndex(maxZ); ++cz) {
                for (int32_t cx = CellIndex(minX); cx <= CellIndex(maxX); ++cx) {
                    uvTrianglesNearCell_[Key_(cx, cz)].push_back(first);
                }
            }
        }

        PathsD input;
        for (const Ring& ring : rings) {
            if (ring.points.size() < 3) {
                continue;
            }
            PathD path;
            path.reserve(ring.points.size());
            for (const auto& [x, z] : ring.points) {
                if (std::isfinite(x) && std::isfinite(z)) {
                    path.emplace_back(x, z);
                }
            }
            if (path.size() >= 3) {
                input.push_back(std::move(path));
            }
        }

        // Even-odd in, clean non-overlapping rings out: every later step can use non-zero.
        area_ = Union(input, FillRule::EvenOdd, kPrecision);
        if (area_.empty() || std::abs(Area(area_)) <= 0.0) {
            area_.clear();
            return;
        }


        const RectD bounds = GetBounds(area_);
        cells_ = CellRange{
            .minX = CellIndex(bounds.left),
            .minZ = CellIndex(bounds.top),
            .maxX = CellIndex(std::nextafter(bounds.right, bounds.left)),
            .maxZ = CellIndex(std::nextafter(bounds.bottom, bounds.top)),
        };

        if (featherMeters_ > 0.0f) {
            const double reach = featherMeters_;
            for (const PathD& path : area_) {
                for (size_t i = 0; i < path.size(); ++i) {
                    const PointD& a = path[i];
                    const PointD& b = path[(i + 1) % path.size()];
                    const Edge edge{a.x, a.y, b.x, b.y};
                    const int32_t x0 = EdgeBucket(std::min(a.x, b.x) - reach);
                    const int32_t x1 = EdgeBucket(std::max(a.x, b.x) + reach);
                    const int32_t z0 = EdgeBucket(std::min(a.y, b.y) - reach);
                    const int32_t z1 = EdgeBucket(std::max(a.y, b.y) + reach);
                    for (int32_t cz = z0; cz <= z1; ++cz) {
                        for (int32_t cx = x0; cx <= x1; ++cx) {
                            edgesNearCell_[Key_(cx, cz)].push_back(edge);
                        }
                    }
                }
            }
        }
    }

    const CellMesh& Mesh::Cell(const int32_t cellX, const int32_t cellZ) const
    {
        static const CellMesh kEmpty{};
        if (area_.empty() || cellX < cells_.minX || cellX > cells_.maxX || cellZ < cells_.minZ || cellZ > cells_.maxZ) {
            return kEmpty;
        }

        const uint64_t key = Key_(cellX, cellZ);
        if (const auto it = cellCache_.find(key); it != cellCache_.end()) {
            return it->second.mesh;
        }

        std::vector<double> signature = Signature_(cellX, cellZ);
        if (previous_) {
            if (const auto it = previous_->cellCache_.find(key);
                it != previous_->cellCache_.end() && it->second.signature == signature) {
                return cellCache_.emplace(key, std::move(it->second)).first->second.mesh;
            }
        }
        return cellCache_.emplace(key, CachedCell{std::move(signature), BuildCell_(cellX, cellZ)}).first->second.mesh;
    }

    void Mesh::ReuseCellsFrom(std::unique_ptr<Mesh> previous)
    {
        if (!previous || !SameSettings_(*previous)) {
            return;
        }
        // One generation back is enough; older ones would only hold memory.
        previous->previous_.reset();
        previous_ = std::move(previous);
    }

    bool Mesh::SameSettings_(const Mesh& other) const noexcept
    {
        return featherMeters_ == other.featherMeters_ && wear_.amount == other.wear_.amount &&
               wear_.patchMeters == other.wear_.patchMeters && wear_.seed == other.wear_.seed &&
               uvMap_.empty() == other.uvMap_.empty();
    }

    std::vector<double> Mesh::Signature_(const int32_t cellX, const int32_t cellZ) const
    {
        // As far around the cell as RegionsIn_ and the soft edge look.
        const double margin = featherMeters_ * 1.5 + 0.1;
        const double x0 = static_cast<double>(cellX) * kCellSize - margin;
        const double z0 = static_cast<double>(cellZ) * kCellSize - margin;
        const RectD near(x0, z0, x0 + kCellSize + 2.0 * margin, z0 + kCellSize + 2.0 * margin);
        const PathsD local = RectClip(near, area_, kPrecision);

        std::vector<std::pair<double, double>> points;
        for (const PathD& path : local) {
            for (const PointD& point : path) {
                points.emplace_back(point.x, point.y);
            }
        }
        std::ranges::sort(points);
        std::vector<double> signature;
        signature.reserve(points.size() * 2 + 2);
        signature.push_back(static_cast<double>(local.size()));
        for (const auto& [x, z] : points) {
            signature.push_back(x);
            signature.push_back(z);
        }
        if (const auto it = uvTrianglesNearCell_.find(Key_(cellX, cellZ)); it != uvTrianglesNearCell_.end()) {
            for (const uint32_t first : it->second) {
                for (int corner = 0; corner < 3; ++corner) {
                    const UvVertex& v = uvMap_[first + corner];
                    signature.insert(signature.end(), {v.x, v.z, v.u, v.v});
                }
            }
        }
        return signature;
    }

    CellMesh Mesh::BuildCell_(const int32_t cellX, const int32_t cellZ) const
    {
        const double x0 = static_cast<double>(cellX) * kCellSize;
        const double z0 = static_cast<double>(cellZ) * kCellSize;
        const double x1 = x0 + kCellSize;
        const double z1 = z0 + kCellSize;
        const RectD cellRect(x0, z0, x1, z1);
        const PathsD lowerTriangle{PathD{{x0, z0}, {x0, z1}, {x1, z1}}};
        const PathsD upperTriangle{PathD{{x0, z0}, {x1, z1}, {x1, z0}}};

        const PathsD worn = WearPatches_(cellRect, cellX, cellZ);
        CellMesh mesh;
        for (auto& [region, core] : RegionsIn_(cellRect)) {
            PathsD inCell = std::move(region);
            if (!worn.empty() && !inCell.empty()) {
                inCell = Difference(inCell, worn, FillRule::NonZero, kPrecision);
            }
            if (inCell.empty()) {
                continue;
            }

            if (core && std::abs(std::abs(Area(inCell)) - kCellArea) < 1.0e-3) {
                // The whole cell is solid paint: the two terrain triangles as they are.
                const auto v = [](const double x, const double z) {
                    return MeshVertex{static_cast<float>(x), static_cast<float>(z), 1.0f};
                };
                AppendTriangle(mesh.lower, v(x0, z0), v(x0, z1), v(x1, z1));
                AppendTriangle(mesh.upper, v(x0, z0), v(x1, z1), v(x1, z0));
                continue;
            }

            for (const bool lower : {true, false}) {
                PolyTreeD tree;
                BooleanOp(ClipType::Intersection, FillRule::NonZero, inCell,
                          lower ? lowerTriangle : upperTriangle, tree, kPrecision);
                std::vector<MeshVertex>& out = lower ? mesh.lower : mesh.upper;
                const auto vertex = [&](const EarcutPoint& p) {
                    const float alpha = core ? 1.0f : AlphaAt_(p[0], p[1]);
                    return MeshVertex{static_cast<float>(p[0]), static_cast<float>(p[1]), alpha};
                };
                TriangulateTree(tree, [&](const EarcutPoint& a, const EarcutPoint& b, const EarcutPoint& c) {
                    AppendTriangle(out, vertex(a), vertex(b), vertex(c));
                });
            }
        }

        if (!uvMap_.empty()) {
            mesh.lower = MapUv_(mesh.lower, Key_(cellX, cellZ));
            mesh.upper = MapUv_(mesh.upper, Key_(cellX, cellZ));
        }
        return mesh;
    }

    std::vector<MeshVertex> Mesh::MapUv_(const std::vector<MeshVertex>& triangles, const uint64_t cellKey) const
    {
        std::vector<MeshVertex> out;
        const auto near = uvTrianglesNearCell_.find(cellKey);
        if (near == uvTrianglesNearCell_.end()) {
            return out;
        }

        for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
            const MeshVertex& a = triangles[t];
            const MeshVertex& b = triangles[t + 1];
            const MeshVertex& c = triangles[t + 2];
            const auto [minX, maxX] = std::minmax({a.x, b.x, c.x});
            const auto [minZ, maxZ] = std::minmax({a.z, b.z, c.z});

            for (const uint32_t first : near->second) {
                const UvVertex* const corners = &uvMap_[first];
                const auto [uMinX, uMaxX] = std::minmax({corners[0].x, corners[1].x, corners[2].x});
                const auto [uMinZ, uMaxZ] = std::minmax({corners[0].z, corners[1].z, corners[2].z});
                if (uMaxX < minX || uMinX > maxX || uMaxZ < minZ || uMinZ > maxZ) {
                    continue;
                }

                const std::vector<ClipPoint> piece = ClipToTriangle({{a.x, a.z}, {b.x, b.z}, {c.x, c.z}}, corners);
                if (piece.size() < 3) {
                    continue;
                }
                std::vector<MeshVertex> vertices;
                vertices.reserve(piece.size());
                for (const ClipPoint& p : piece) {
                    std::array<double, 3> inPiece{};
                    std::array<double, 3> inMap{};
                    if (!Barycentric(p.x, p.z, a.x, a.z, b.x, b.z, c.x, c.z, inPiece) ||
                        !Barycentric(p.x, p.z, corners[0].x, corners[0].z, corners[1].x, corners[1].z,
                                     corners[2].x, corners[2].z, inMap)) {
                        break;
                    }
                    vertices.push_back(MeshVertex{
                        .x = static_cast<float>(p.x),
                        .z = static_cast<float>(p.z),
                        .alpha = static_cast<float>(inPiece[0] * a.alpha + inPiece[1] * b.alpha + inPiece[2] * c.alpha),
                        .u = static_cast<float>(inMap[0] * corners[0].u + inMap[1] * corners[1].u + inMap[2] * corners[2].u),
                        .v = static_cast<float>(inMap[0] * corners[0].v + inMap[1] * corners[1].v + inMap[2] * corners[2].v),
                    });
                }
                if (vertices.size() != piece.size()) {
                    continue;
                }
                for (size_t i = 1; i + 1 < vertices.size(); ++i) {
                    AppendTriangle(out, vertices[0], vertices[i], vertices[i + 1]);
                }
            }
        }
        return out;
    }

    std::vector<std::pair<PathsD, bool>> Mesh::RegionsIn_(const RectD& cellRect) const
    {
        std::vector<std::pair<PathsD, bool>> regions;
        if (!(featherMeters_ > 0.0f)) {
            regions.emplace_back(RectClip(cellRect, area_, kPrecision), true);
            return regions;
        }

        // The bands are worked out from the part of the area near the cell, so a change to a long
        // shape costs only the cells drawn. The cut around that part is far enough away that the
        // insets from it stay out of the cell.
        const double margin = featherMeters_ * 1.5 + 0.1;
        const RectD near(cellRect.left - margin, cellRect.top - margin, cellRect.right + margin, cellRect.bottom + margin);
        const PathsD local = RectClip(near, area_, kPrecision);
        if (local.empty()) {
            return regions;
        }
        PathsD outer = local;
        for (int band = 1; band <= kFeatherBands; ++band) {
            const double inset = featherMeters_ * band / kFeatherBands;
            // Mitred: the bands only split the soft edge for the triangles, the alpha comes from the
            // distance to the outline. Round joins put dozens of points on every corner, which
            // marking-like shapes have hundreds of.
            PathsD inner = InflatePaths(local, -inset, JoinType::Miter, EndType::Polygon, 2.0, kPrecision, kArcTolerance);
            regions.emplace_back(RectClip(cellRect, Difference(outer, inner, FillRule::NonZero, kPrecision), kPrecision),
                                 false);
            outer = std::move(inner);
        }
        regions.emplace_back(RectClip(cellRect, outer, kPrecision), true);
        return regions;
    }

    PathsD Mesh::WearPatches_(const RectD& cellRect, const int32_t cellX, const int32_t cellZ) const
    {
        PathsD patches;
        const double amount = std::clamp(static_cast<double>(wear_.amount), 0.0, 0.95);
        if (!(amount > 0.0) || !(wear_.patchMeters > 0.0f)) {
            return patches;
        }
        const PathsD paint = RectClip(cellRect, area_, kPrecision);
        const double paintArea = std::abs(Area(paint));
        if (!(paintArea > 0.0)) {
            return patches;
        }

        // Randomly placed patches leave exp(-cover) of the paint, so this many take away the share
        // asked for. A patch is a rough circle, its size varied by up to 40%: on average about
        // 0.8 of the circle's area.
        constexpr int kMaxPatches = 400;
        constexpr int kCorners = 7;
        double radius = wear_.patchMeters * 0.5;
        const double cover = -std::log(1.0 - amount);
        double count = cover * paintArea / (0.8 * std::numbers::pi * radius * radius);
        if (count > kMaxPatches) {
            // Bigger patches rather than too many to cut quickly.
            radius *= std::sqrt(count / kMaxPatches);
            count = kMaxPatches;
        }

        Random random(wear_.seed ^ (static_cast<uint32_t>(cellX) * 0x9E3779B1u) ^ (static_cast<uint32_t>(cellZ) * 0x85EBCA77u));
        // The fraction is placed or not by chance, so small amounts still wear small paints.
        const auto wanted = static_cast<int>(count + random.Next());
        const RectD bounds = GetBounds(paint);
        const double width = bounds.right - bounds.left;
        const double depth = bounds.bottom - bounds.top;
        for (int tries = 0; static_cast<int>(patches.size()) < wanted && tries < wanted * 60 + 60; ++tries) {
            const PointD centre(bounds.left + random.Next() * width, bounds.top + random.Next() * depth);
            if (!Inside(paint, centre)) {
                continue;
            }
            const double size = radius * (0.6 + 0.8 * random.Next());
            const double turn = random.Next() * 2.0 * std::numbers::pi;
            PathD patch;
            patch.reserve(kCorners);
            for (int i = 0; i < kCorners; ++i) {
                // Turning one way only: overlapping patches need no union before the cut.
                const double angle = turn + 2.0 * std::numbers::pi * i / kCorners;
                const double reach = size * (0.7 + 0.3 * random.Next());
                patch.emplace_back(centre.x + reach * std::cos(angle), centre.y + reach * std::sin(angle));
            }
            patches.push_back(std::move(patch));
        }
        return patches;
    }

    float Mesh::AlphaAt_(const double x, const double z) const
    {
        const auto it = edgesNearCell_.find(Key_(EdgeBucket(x), EdgeBucket(z)));
        if (it == edgesNearCell_.end() || featherMeters_ <= 0.0f) {
            return 1.0f;
        }

        double distance = featherMeters_;
        for (const Edge& edge : it->second) {
            distance = std::min(distance, DistanceToSegment(x, z, edge.ax, edge.az, edge.bx, edge.bz));
        }
        return static_cast<float>(std::clamp(distance / featherMeters_, 0.0, 1.0));
    }

    uint64_t Mesh::Key_(const int32_t cellX, const int32_t cellZ) noexcept
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32) | static_cast<uint32_t>(cellZ);
    }
}
