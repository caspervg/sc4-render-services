#include "ClippedTerrainDecalRenderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "cIGZGDriver.h"
#include "SC4DrawContextAccess.h"
#include "TerrainPaintMesh.h"
#include "utils/Logger.h"

namespace
{
    using namespace TerrainDecal;

    // SC4 uses primType 0 for caller-supplied explicit triangles.
    constexpr uint32_t kPrimTypeTriangleList = 0;
    constexpr uint32_t kTerrainVertexFormat = 0x0B;
    constexpr float kClipEpsilon = 1.0e-5f;

    // Vanilla DrawDecals uses depth offset 2; DrawShadows uses 3 (higher = closer to camera in SC4).
    constexpr int kVanillaDecalDepthOffset = 2;

    // Overlay slot layout (cSTEOverlayManager, 0xB4 bytes per slot).
    constexpr std::ptrdiff_t kSlotStateOffset = 0x00;
    constexpr std::ptrdiff_t kSlotFlagsOffset = 0x04;
    constexpr std::ptrdiff_t kSlotRectOffset = 0x0C;
    constexpr std::ptrdiff_t kSlotMatrixOffset = 0x1C;
    constexpr std::ptrdiff_t kSlotOpacityOffset = 0x9C;

    // Overlay flags as cSTEOverlayManager::DrawDecals (0x00736790) reads them.
    constexpr uint32_t kOverlayFlagWrapU = 0x02;
    constexpr uint32_t kOverlayFlagWrapUNoClipV = 0x20;
    constexpr uint32_t kOverlayFlagSelfLit = 0x40;

    using DrawPrimsFn = void(__thiscall*)(SC4DrawContext*, uint32_t, uint32_t, uint32_t, const void*);
    using SetDepthOffsetFn = void(__thiscall*)(SC4DrawContext*, int);
    // SC4DrawContext::SetTransparency is a tail-call shim to the renderer; the float
    // opacity stays on the stack just like the original DrawDecals call sequence.
    using SetTransparencyFn = void(__thiscall*)(SC4DrawContext*, float);
    using SetTexTransform4Fn = void(__thiscall*)(SC4DrawContext*, const float*, int);
    using ClearTexTransformFn = void(__thiscall*)(SC4DrawContext*, int);

    struct TerrainDrawRect
    {
        int xStart;
        int zStart;
        int xEnd;
        int zEnd;
    };

    struct TerrainGridDimensions
    {
        int cellCountX = 0;
        int cellCountZ = 0;
        int vertexCountX = 0;
        int vertexCountZ = 0;
        int vertexCount = 0;
    };

    using PackedTerrainVertex = TerrainVertex;

    struct ClipVertex
    {
        PackedTerrainVertex vertex{};
        float clipU = 0.0f;
        float clipV = 0.0f;
    };

    struct OverlaySlotView
    {
        int32_t state = 0;
        uint32_t flags = 0;
        TerrainDrawRect rect{};
        const float* matrix = nullptr;
        float opacity = 1.0f;
    };

    struct ClipBounds
    {
        float minU = 0.0f;
        float maxU = 1.0f;
        float minV = 0.0f;
        float maxV = 1.0f;
    };

    struct CellInfoEntry
    {
        int vertexIndex;
        uint32_t flatYBits;
    };

    struct RowTableEntry
    {
        const std::byte* data;
        uint32_t unknown1;
        uint32_t unknown2;
    };

    using TexTransform = std::array<float, 16>;

    // How the context is set up for one overlay's draw, and put back afterwards.
    struct DrawSetup
    {
        std::optional<TexTransform> texTransform{};
        const float* baseTexTransform = nullptr;
        int texTransformStage = -1;
        int depthOffset = kVanillaDecalDepthOffset;
        // Transparency for this draw; afterwards the slot's own is set again.
        std::optional<float> transparency{};
        float slotOpacity = 1.0f;
        // Terrain paint: repeat the texture, and take colour and alpha from the vertices.
        bool paint = false;
        // Terrain paint with a UV map: the texture coordinates are the vertices' own.
        bool vertexUv = false;
        uint32_t slotFlags = 0;
    };

    // Logs a category of problem once per overlay instead of every frame.
    [[nodiscard]] bool FirstTime(const uint32_t overlayId, const std::string_view category)
    {
        static std::unordered_set<uint64_t> seen;
        const uint64_t key = (static_cast<uint64_t>(std::hash<std::string_view>{}(category)) << 32u) | overlayId;
        return seen.insert(key).second;
    }

    [[nodiscard]] OverlaySlotView ReadOverlaySlotView(const std::byte* const slotBase) noexcept
    {
        OverlaySlotView result{};
        std::memcpy(&result.state, slotBase + kSlotStateOffset, sizeof(result.state));
        std::memcpy(&result.flags, slotBase + kSlotFlagsOffset, sizeof(result.flags));
        std::memcpy(&result.rect, slotBase + kSlotRectOffset, sizeof(result.rect));
        result.matrix = reinterpret_cast<const float*>(slotBase + kSlotMatrixOffset);
        std::memcpy(&result.opacity, slotBase + kSlotOpacityOffset, sizeof(result.opacity));
        return result;
    }

    [[nodiscard]] bool ShouldClipU(const uint32_t flags) noexcept
    {
        return (flags & (kOverlayFlagWrapUNoClipV | kOverlayFlagWrapU)) == 0u;
    }

    [[nodiscard]] bool ShouldClipV(const uint32_t flags) noexcept
    {
        return (flags & kOverlayFlagWrapUNoClipV) == 0u;
    }

    // The wrap modes DrawDecals set for this overlay before calling DrawRect.
    [[nodiscard]] std::pair<int, int> VanillaWrapModes(const uint32_t flags) noexcept
    {
        using namespace DrawContextAccess;
        if ((flags & kOverlayFlagWrapUNoClipV) != 0u) {
            return {kWrapRepeat, kWrapClamp};
        }
        if ((flags & kOverlayFlagWrapU) != 0u) {
            return {kWrapRepeat, kWrapRepeat};
        }
        return {kWrapClamp, kWrapClamp};
    }

    [[nodiscard]] bool IsValidUvWindow(const TerrainDecalUvWindow& uvRect) noexcept
    {
        return std::isfinite(uvRect.u1) && std::isfinite(uvRect.v1) &&
               std::isfinite(uvRect.u2) && std::isfinite(uvRect.v2) &&
               uvRect.u1 < uvRect.u2 && uvRect.v1 < uvRect.v2;
    }

    [[nodiscard]] bool MatrixHasFiniteComponents(const float* const matrix) noexcept
    {
        return matrix && std::all_of(matrix, matrix + 16, [](const float value) { return std::isfinite(value); });
    }

    [[nodiscard]] bool TryResolveOverlayId(const DrawRequest& request, uint32_t& overlayId) noexcept
    {
        overlayId = 0;
        const HookAddresses* const addresses = request.addresses;
        if (!addresses || !request.overlayManager || !request.overlaySlotBase ||
            addresses->overlaySlotsPtrOffset <= 0 || addresses->overlaySlotStride <= 0) {
            return false;
        }

        const auto* const managerBytes = static_cast<const std::byte*>(request.overlayManager);
        const auto* const slotsBase =
            *reinterpret_cast<const std::byte* const*>(managerBytes + addresses->overlaySlotsPtrOffset);
        if (!slotsBase || request.overlaySlotBase < slotsBase) {
            return false;
        }

        const std::ptrdiff_t delta = request.overlaySlotBase - slotsBase;
        if ((delta % addresses->overlaySlotStride) != 0) {
            return false;
        }

        overlayId = static_cast<uint32_t>(delta / addresses->overlaySlotStride);
        return true;
    }

    // Applies a 2D affine map to the (u, v) a texture transform produces:
    // u' = a u + b v + c, v' = d u + e v + f. The transform is row-vector style with u, v and w
    // in columns 0, 1 and 3; the constant terms scale with w so projective input stays right.
    void ApplyUvAffine(TexTransform& t,
                       const float a, const float b, const float c,
                       const float d, const float e, const float f) noexcept
    {
        for (size_t row = 0; row < 4; ++row) {
            const float u = t[row * 4 + 0];
            const float v = t[row * 4 + 1];
            const float w = t[row * 4 + 3];
            t[row * 4 + 0] = a * u + b * v + c * w;
            t[row * 4 + 1] = d * u + e * v + f * w;
        }
    }

    [[nodiscard]] TexTransform CopyTransform(const float* const source) noexcept
    {
        TexTransform t{};
        std::copy_n(source, t.size(), t.begin());
        return t;
    }

    // The decal's own UV modifiers, in the order the service documents them: scale and offset,
    // then mirror, then the UV window.
    [[nodiscard]] std::optional<TexTransform> BuildDecalTextureTransform(const float* const base,
                                                                         const DecalOverrides& overrides,
                                                                         const bool applyUvWindow) noexcept
    {
        const bool scaled = overrides.aspectMultiplier != 1.0f || overrides.uvScaleU != 1.0f ||
                            overrides.uvScaleV != 1.0f || overrides.uvOffset != 0.0f;
        if (!scaled && !overrides.mirror && !applyUvWindow) {
            return std::nullopt;
        }

        TexTransform t = CopyTransform(base);
        if (scaled) {
            ApplyUvAffine(t, overrides.uvScaleU * overrides.aspectMultiplier, 0.0f, overrides.uvOffset,
                          0.0f, overrides.uvScaleV, overrides.uvOffset);
        }
        if (overrides.mirror) {
            ApplyUvAffine(t, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
        }
        if (applyUvWindow) {
            const TerrainDecalUvWindow& w = overrides.uvWindow;
            ApplyUvAffine(t, w.u2 - w.u1, 0.0f, w.u1, 0.0f, w.v2 - w.v1, w.v1);
        }
        return t;
    }

    // The paint's texture coordinates, which are a function of ground position, expressed in
    // terms of the decal coordinates the active transform produces. The slot matrix maps ground
    // position to decal coordinates (uv_d = D p + d), so uv_paint = A p + t = A D^-1 (uv_d - d) + t.
    [[nodiscard]] std::optional<TexTransform> BuildPaintTextureTransform(const float* const base,
                                                                         const float* const slotMatrix,
                                                                         const std::array<float, 6>& uvFromGround) noexcept
    {
        const double d00 = slotMatrix[0];
        const double d01 = slotMatrix[8];
        const double d10 = slotMatrix[1];
        const double d11 = slotMatrix[9];
        const double det = d00 * d11 - d01 * d10;
        if (!(std::abs(det) > 1.0e-12)) {
            return std::nullopt;
        }
        const double i00 = d11 / det;
        const double i01 = -d01 / det;
        const double i10 = -d10 / det;
        const double i11 = d00 / det;

        const auto& g = uvFromGround;
        const double m00 = g[0] * i00 + g[1] * i10;
        const double m01 = g[0] * i01 + g[1] * i11;
        const double m10 = g[3] * i00 + g[4] * i10;
        const double m11 = g[3] * i01 + g[4] * i11;
        const double k0 = g[2] - (m00 * slotMatrix[12] + m01 * slotMatrix[13]);
        const double k1 = g[5] - (m10 * slotMatrix[12] + m11 * slotMatrix[13]);

        TexTransform t = CopyTransform(base);
        ApplyUvAffine(t,
                      static_cast<float>(m00), static_cast<float>(m01), static_cast<float>(k0),
                      static_cast<float>(m10), static_cast<float>(m11), static_cast<float>(k1));
        return t;
    }

    [[nodiscard]] float UnpackColorChannel(const uint32_t color, const int shift) noexcept
    {
        return static_cast<float>((color >> shift) & 0xFFu);
    }

    [[nodiscard]] uint32_t PackColor(const float a, const float r, const float g, const float b) noexcept
    {
        const auto clampByte = [](const float value) -> uint32_t {
            return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
        };
        return (clampByte(a) << 24u) | (clampByte(r) << 16u) | (clampByte(g) << 8u) | clampByte(b);
    }

    [[nodiscard]] PackedTerrainVertex LerpVertex(const PackedTerrainVertex& a,
                                                 const PackedTerrainVertex& b,
                                                 const float t) noexcept
    {
        PackedTerrainVertex out{};
        out.x = std::lerp(a.x, b.x, t);
        out.y = std::lerp(a.y, b.y, t);
        out.z = std::lerp(a.z, b.z, t);
        out.u = std::lerp(a.u, b.u, t);
        out.v = std::lerp(a.v, b.v, t);
        out.extra0 = std::lerp(a.extra0, b.extra0, t);
        out.extra1 = std::lerp(a.extra1, b.extra1, t);
        out.diffuse = PackColor(std::lerp(UnpackColorChannel(a.diffuse, 24), UnpackColorChannel(b.diffuse, 24), t),
                                std::lerp(UnpackColorChannel(a.diffuse, 16), UnpackColorChannel(b.diffuse, 16), t),
                                std::lerp(UnpackColorChannel(a.diffuse, 8), UnpackColorChannel(b.diffuse, 8), t),
                                std::lerp(UnpackColorChannel(a.diffuse, 0), UnpackColorChannel(b.diffuse, 0), t));
        return out;
    }

    void EvaluateFootprintUv(const float* const matrix, ClipVertex& vertex) noexcept
    {
        const float x = vertex.vertex.x;
        const float y = vertex.vertex.y;
        const float z = vertex.vertex.z;
        const float u = x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12];
        const float v = x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13];
        const float w = x * matrix[3] + y * matrix[7] + z * matrix[11] + matrix[15];

        if (std::fabs(w) > kClipEpsilon && std::fabs(w - 1.0f) > kClipEpsilon) {
            vertex.clipU = u / w;
            vertex.clipV = v / w;
        }
        else {
            vertex.clipU = u;
            vertex.clipV = v;
        }
    }

    [[nodiscard]] bool HasFiniteClipUv(const ClipVertex& vertex) noexcept
    {
        return std::isfinite(vertex.clipU) && std::isfinite(vertex.clipV);
    }

    [[nodiscard]] bool IsInside(const ClipVertex& v, const bool clipU, const bool clipV, const ClipBounds& bounds) noexcept
    {
        return HasFiniteClipUv(v) &&
               (!clipU || (v.clipU >= bounds.minU - kClipEpsilon && v.clipU <= bounds.maxU + kClipEpsilon)) &&
               (!clipV || (v.clipV >= bounds.minV - kClipEpsilon && v.clipV <= bounds.maxV + kClipEpsilon));
    }

    void ClipPolygonAgainstPlane(std::vector<ClipVertex>& polygon,
                                 const bool useU,
                                 const bool isMinPlane,
                                 const float limit)
    {
        if (polygon.empty()) {
            return;
        }

        const auto inside = [&](const ClipVertex& v) {
            const float value = useU ? v.clipU : v.clipV;
            return isMinPlane ? value >= (limit - kClipEpsilon) : value <= (limit + kClipEpsilon);
        };

        std::vector<ClipVertex> output;
        output.reserve(polygon.size() + 2);
        ClipVertex previous = polygon.back();
        bool previousInside = inside(previous);
        for (const ClipVertex& current : polygon) {
            const bool currentInside = inside(current);
            if (currentInside != previousInside) {
                const float a = useU ? previous.clipU : previous.clipV;
                const float b = useU ? current.clipU : current.clipV;
                const float denom = b - a;
                const float t = std::clamp(std::fabs(denom) > kClipEpsilon ? (limit - a) / denom : 0.0f, 0.0f, 1.0f);
                output.push_back(ClipVertex{
                    .vertex = LerpVertex(previous.vertex, current.vertex, t),
                    .clipU = std::lerp(previous.clipU, current.clipU, t),
                    .clipV = std::lerp(previous.clipV, current.clipV, t),
                });
            }
            if (currentInside) {
                output.push_back(current);
            }
            previous = current;
            previousInside = currentInside;
        }
        polygon = std::move(output);
    }

    // A terrain cell's two triangles as corners of LoadTerrainCellVertices, as
    // SetTriangleIndicesForCell (0x00752AF0) builds them. A flipped cell is split along its other
    // diagonal: the game's cliff code and mods such as the DJEM fix flip cells.
    [[nodiscard]] const std::array<size_t, 6>& CellTriangles(const bool flipped) noexcept
    {
        static constexpr std::array<size_t, 6> kNormal{0, 1, 2, 0, 2, 3};
        static constexpr std::array<size_t, 6> kFlipped{0, 1, 3, 3, 1, 2};
        return flipped ? kFlipped : kNormal;
    }

    // Clips each terrain triangle on its own: a fan over the clipped quad would crease the cell
    // along a line the terrain does not have.
    void ClipAndEmitCell(const std::array<ClipVertex, 4>& quad,
                         const std::array<size_t, 6>& triangles,
                         const bool clipU,
                         const bool clipV,
                         const ClipBounds& bounds,
                         std::vector<PackedTerrainVertex>& output)
    {
        for (size_t t = 0; t < triangles.size(); t += 3) {
            std::vector<ClipVertex> polygon{quad[triangles[t]], quad[triangles[t + 1]], quad[triangles[t + 2]]};
            if (clipU) {
                ClipPolygonAgainstPlane(polygon, true, true, bounds.minU);
                ClipPolygonAgainstPlane(polygon, true, false, bounds.maxU);
            }
            if (clipV) {
                ClipPolygonAgainstPlane(polygon, false, true, bounds.minV);
                ClipPolygonAgainstPlane(polygon, false, false, bounds.maxV);
            }
            for (size_t i = 1; i + 1 < polygon.size(); ++i) {
                output.push_back(polygon[0].vertex);
                output.push_back(polygon[i].vertex);
                output.push_back(polygon[i + 1].vertex);
            }
        }
    }

    [[nodiscard]] bool QuadMayIntersectClipBox(const std::array<ClipVertex, 4>& vertices,
                                               const bool clipU,
                                               const bool clipV,
                                               const ClipBounds& bounds) noexcept
    {
        float minU = vertices[0].clipU;
        float maxU = minU;
        float minV = vertices[0].clipV;
        float maxV = minV;
        for (const ClipVertex& vertex : vertices) {
            minU = std::min(minU, vertex.clipU);
            maxU = std::max(maxU, vertex.clipU);
            minV = std::min(minV, vertex.clipV);
            maxV = std::max(maxV, vertex.clipV);
        }
        const bool overlapsU = !clipU || !(maxU < bounds.minU || minU > bounds.maxU);
        const bool overlapsV = !clipV || !(maxV < bounds.minV || minV > bounds.maxV);
        return overlapsU && overlapsV;
    }

    template <typename T>
    [[nodiscard]] T ReadGlobal(const uintptr_t address) noexcept
    {
        return address != 0 ? *reinterpret_cast<const T*>(address) : T{};
    }

    [[nodiscard]] TerrainGridDimensions ReadTerrainGridDimensions(const HookAddresses& addresses) noexcept
    {
        TerrainGridDimensions result{
            .cellCountX = ReadGlobal<int>(addresses.terrainCellCountXPtr),
            .cellCountZ = ReadGlobal<int>(addresses.terrainCellCountZPtr),
            .vertexCountX = ReadGlobal<int>(addresses.terrainVertexCountXPtr),
            .vertexCountZ = ReadGlobal<int>(addresses.terrainVertexCountZPtr),
            .vertexCount = ReadGlobal<int>(addresses.terrainVertexCountPtr),
        };
        if (result.vertexCountZ <= 0 && result.vertexCountX > 0 && result.vertexCount > 0) {
            result.vertexCountZ = result.vertexCount / result.vertexCountX;
        }
        return result;
    }

    // The overlay's cells, as an exclusive range clamped to the terrain.
    [[nodiscard]] TerrainDrawRect ClampedDrawRect(const TerrainDrawRect& rect,
                                                  const TerrainGridDimensions& dimensions) noexcept
    {
        constexpr int kMax = std::numeric_limits<int>::max();
        TerrainDrawRect result{};
        result.xStart = std::clamp(rect.xStart, 0, std::max(0, dimensions.cellCountX));
        result.zStart = std::clamp(rect.zStart, 0, std::max(0, dimensions.cellCountZ));
        result.xEnd = std::clamp(rect.xEnd < kMax ? rect.xEnd + 1 : kMax, result.xStart, std::max(0, dimensions.cellCountX));
        result.zEnd = std::clamp(rect.zEnd < kMax ? rect.zEnd + 1 : kMax, result.zStart, std::max(0, dimensions.cellCountZ));
        return result;
    }

    // The cells the terrain view puts on screen this frame, read the way the game's DrawRect
    // (0x00735720) reads them, so a large overlay costs only what is visible.
    class DrawFrustum
    {
    public:
        DrawFrustum(const HookAddresses& addresses, const void* const overlayManager, const TerrainGridDimensions& dimensions)
        {
            const auto* const manager = static_cast<const std::byte*>(overlayManager);
            if (!manager || addresses.overlayTerrainViewOffset <= 0 || !addresses.drawFrustumRowFlagsPtr ||
                ReadField<uint8_t>(manager, addresses.overlayWholeRectFlagOffsetA) != 0 ||
                ReadField<uint8_t>(manager, addresses.overlayWholeRectFlagOffsetB) != 0) {
                return;
            }
            const auto* const view = ReadField<const std::byte*>(manager, addresses.overlayTerrainViewOffset);
            rowVisible_ = ReadGlobal<const uint8_t*>(addresses.drawFrustumRowFlagsPtr);
            columnStart_ = ReadGlobal<const int*>(addresses.drawFrustumColumnStartPtr);
            columnEnd_ = ReadGlobal<const int*>(addresses.drawFrustumColumnEndPtr);
            if (!view || !rowVisible_ || !columnStart_ || !columnEnd_) {
                return;
            }
            rowStart_ = std::max(0, ReadField<int>(view, addresses.terrainViewDrawRowStartOffset));
            rowEnd_ = std::min(dimensions.vertexCountZ, ReadField<int>(view, addresses.terrainViewDrawRowEndOffset));
            active_ = true;
        }

        // Narrows the cells [x0, x1) of cell row z to those on screen; false when none are.
        [[nodiscard]] bool VisibleCells(const int z, int& x0, int& x1) const noexcept
        {
            if (active_) {
                if (z < rowStart_ || z + 1 >= rowEnd_ || (rowVisible_[z] == 0 && rowVisible_[z + 1] == 0)) {
                    return false;
                }
                x0 = std::max(x0, columnStart_[z]);
                x1 = std::min(x1, columnEnd_[z] - 1);
            }
            return x0 < x1;
        }

    private:
        template <typename T>
        [[nodiscard]] static T ReadField(const std::byte* const base, const std::ptrdiff_t offset) noexcept
        {
            T value{};
            std::memcpy(&value, base + offset, sizeof(value));
            return value;
        }

        bool active_ = false;
        const uint8_t* rowVisible_ = nullptr;
        const int* columnStart_ = nullptr;
        const int* columnEnd_ = nullptr;
        int rowStart_ = 0;
        int rowEnd_ = 0;
    };

    // Which terrain cells are split along their other diagonal, read from the records the way
    // SetTriangleIndicesForCell (0x00752AF0) does. One load per cell asked about. Without the
    // records every cell counts as not flipped.
    class TerrainCellFlips
    {
    public:
        TerrainCellFlips(const HookAddresses& addresses, const void* const overlayManager, const TerrainGridDimensions& dimensions)
            : dimensions_(dimensions)
        {
            if (!overlayManager || addresses.overlayTerrainViewOffset <= 0 || addresses.terrainViewTerrainOffset <= 0 ||
                addresses.terrainVertexRecordsOffset <= 0) {
                return;
            }
            const auto* const view = DrawContextAccess::ReadField<const std::byte*>(overlayManager, addresses.overlayTerrainViewOffset);
            const auto* const terrain = view ? DrawContextAccess::ReadField<const std::byte*>(view, addresses.terrainViewTerrainOffset) : nullptr;
            records_ = terrain ? DrawContextAccess::ReadField<const std::byte*>(terrain, addresses.terrainVertexRecordsOffset) : nullptr;
        }

        [[nodiscard]] bool IsFlipped(const int cellX, const int cellZ) const noexcept
        {
            if (!records_ || cellX < 0 || cellZ < 0 || cellX >= dimensions_.cellCountX || cellZ >= dimensions_.cellCountZ) {
                return false;
            }
            const auto index = static_cast<size_t>(cellZ * dimensions_.vertexCountX + cellX);
            uint16_t flags = 0;
            std::memcpy(&flags, records_ + index * kRecordSize + kRecordFlagsOffset, sizeof(flags));
            return (flags & kFlippedFlags) != 0;
        }

    private:
        static constexpr size_t kRecordSize = 0x24;
        static constexpr size_t kRecordFlagsOffset = 0x22;
        static constexpr uint16_t kFlippedFlags = 0x3000;  // internal 0x1000, external 0x2000

        TerrainGridDimensions dimensions_;
        const std::byte* records_ = nullptr;
    };

    // The four corners of a terrain cell: (x, z), (x, z + 1), (x + 1, z + 1), (x + 1, z).
    [[nodiscard]] bool LoadTerrainCellVertices(const HookAddresses& addresses,
                                               const TerrainGridDimensions& dimensions,
                                               const int cellX,
                                               const int cellZ,
                                               std::array<PackedTerrainVertex, 4>& result)
    {
        if (dimensions.cellCountX <= 0 || dimensions.cellCountZ <= 0 ||
            dimensions.vertexCountX <= 0 || dimensions.vertexCountZ <= 0 || dimensions.vertexCount <= 0 ||
            cellX >= dimensions.cellCountX || cellZ >= dimensions.cellCountZ) {
            return false;
        }

        const auto* const vertices = ReadGlobal<const PackedTerrainVertex*>(addresses.terrainGridVerticesPtr);
        const auto* const rows = ReadGlobal<const RowTableEntry*>(addresses.terrainCellInfoRowsPtr);
        const auto* const allLevelCellIndices = ReadGlobal<const uint16_t*>(addresses.allLevelCellIndicesPtr);
        if (!vertices || !rows || !allLevelCellIndices) {
            return false;
        }

        // The game maps a terrain cell to an optional levelled entry in sLevelCellInfos[z]. When
        // present, that entry overrides both the X-relative vertex index and the cell height.
        const int levelIndexBase = (dimensions.cellCountX + 1) * cellZ;
        const uint16_t levelEntryStart = allLevelCellIndices[levelIndexBase + cellX];
        const uint16_t levelEntryEnd = allLevelCellIndices[levelIndexBase + cellX + 1];

        int rowRelativeIndex = cellX;
        const CellInfoEntry* levelEntry = nullptr;
        if (levelEntryStart < levelEntryEnd) {
            const auto* const row = reinterpret_cast<const CellInfoEntry*>(rows[cellZ].data);
            if (!row) {
                return false;
            }
            levelEntry = &row[levelEntryStart];
            rowRelativeIndex = levelEntry->vertexIndex;
        }
        if (rowRelativeIndex < 0) {
            return false;
        }

        const int vertexCountX = dimensions.vertexCountX;
        const int baseIndex = cellZ * vertexCountX + rowRelativeIndex;
        if (baseIndex + vertexCountX + 1 >= dimensions.vertexCount) {
            return false;
        }

        result[0] = vertices[baseIndex];
        result[1] = vertices[baseIndex + vertexCountX];
        result[2] = vertices[baseIndex + vertexCountX + 1];
        result[3] = vertices[baseIndex + 1];
        if (levelEntry) {
            const float flatY = std::bit_cast<float>(levelEntry->flatYBits);
            for (PackedTerrainVertex& vertex : result) {
                vertex.y = flatY;
            }
        }
        return true;
    }

    // A terrain cell's corners on the grid, in the order LoadTerrainCellVertices returns them.
    struct GridCorner
    {
        float x;
        float z;
    };

    // Keeps paint triangles that lie on terrain triangle (a, b, c) by their barycentric weights,
    // which depend only on where the corners are on the grid, not on the terrain's heights.
    void PlaceVertices(const std::vector<TerrainPaint::MeshVertex>& triangles,
                       const GridCorner a,
                       const GridCorner b,
                       const GridCorner c,
                       std::vector<PaintDrawCache::PlacedVertex>& output)
    {
        const float denom = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        for (const TerrainPaint::MeshVertex& p : triangles) {
            output.push_back(PaintDrawCache::PlacedVertex{
                .x = p.x,
                .z = p.z,
                .wa = ((b.z - c.z) * (p.x - c.x) + (c.x - b.x) * (p.z - c.z)) / denom,
                .wb = ((c.z - a.z) * (p.x - c.x) + (a.x - c.x) * (p.z - c.z)) / denom,
                .alpha = p.alpha,
                .u = p.u,
                .v = p.v,
            });
        }
    }

    // The overlay's cells [x0, x1) of one cell row, placed on their terrain triangles. A long
    // shape is placed only as far as it has been on screen.
    void BuildPaintRow(const PaintOverlay& paint,
                       const TerrainCellFlips& flips,
                       const int cellZ,
                       const int x0,
                       const int x1,
                       PaintDrawCache::Row& row)
    {
        const TerrainPaint::CellRange& range = paint.mesh->Cells();
        std::array<float, kTerrainPaintMaxTextures> weights{};
        const uint32_t textureCount = std::min(paint.style->textureCount, kTerrainPaintMaxTextures);
        for (uint32_t i = 0; i < textureCount; ++i) {
            weights[i] = paint.style->textures[i].weight;
        }
        const std::span<const float> weightSpan(weights.data(), textureCount);

        row.cells.clear();
        row.vertices.clear();
        constexpr float kSize = TerrainPaint::kCellSize;
        for (int cellX = std::max(x0, range.minX); cellX < std::min(x1, range.maxX + 1); ++cellX) {
            if (textureCount > 1 &&
                TerrainPaint::PickTexture(weightSpan, paint.style->variationSeed, cellX, cellZ) != paint.textureIndex) {
                continue;
            }
            const bool flipped = flips.IsFlipped(cellX, cellZ);
            const TerrainPaint::CellMesh& cell = paint.mesh->Cell(cellX, cellZ, flipped);
            if (cell.Empty()) {
                continue;
            }
            const float left = static_cast<float>(cellX) * kSize;
            const float top = static_cast<float>(cellZ) * kSize;
            const std::array<GridCorner, 4> corners{GridCorner{left, top},
                                                    GridCorner{left, top + kSize},
                                                    GridCorner{left + kSize, top + kSize},
                                                    GridCorner{left + kSize, top}};
            row.cells.push_back(PaintDrawCache::CellSpan{
                .cellX = cellX,
                .first = static_cast<uint32_t>(row.vertices.size()),
                .lowerCount = static_cast<uint32_t>(cell.lower.size()),
                .upperCount = static_cast<uint32_t>(cell.upper.size()),
                .flipped = flipped,
            });
            const std::array<size_t, 6>& t = CellTriangles(flipped);
            PlaceVertices(cell.lower, corners[t[0]], corners[t[1]], corners[t[2]], row.vertices);
            PlaceVertices(cell.upper, corners[t[3]], corners[t[4]], corners[t[5]], row.vertices);
        }
        row.built = true;
        row.builtX0 = x0;
        row.builtX1 = x1;
    }

    // Lifts placed vertices onto the terrain triangle (a, b, c): height, colour and the rest by
    // the weights, alpha from the paint, and with a UV map the texture coordinates too.
    void LiftVertices(const std::span<const PaintDrawCache::PlacedVertex> placed,
                      const PackedTerrainVertex& a,
                      const PackedTerrainVertex& b,
                      const PackedTerrainVertex& c,
                      const float alphaScale,
                      const bool vertexUv,
                      std::vector<PackedTerrainVertex>& output)
    {
        const auto channels = [](const PackedTerrainVertex& v) {
            return std::array<float, 3>{UnpackColorChannel(v.diffuse, 16), UnpackColorChannel(v.diffuse, 8),
                                        UnpackColorChannel(v.diffuse, 0)};
        };
        const std::array<float, 3> ca = channels(a);
        const std::array<float, 3> cb = channels(b);
        const std::array<float, 3> cc = channels(c);
        for (const PaintDrawCache::PlacedVertex& p : placed) {
            const float wc = 1.0f - p.wa - p.wb;
            const auto mix = [&](const float va, const float vb, const float vc) { return p.wa * va + p.wb * vb + wc * vc; };
            output.push_back(PackedTerrainVertex{
                .x = p.x,
                .y = mix(a.y, b.y, c.y),
                .z = p.z,
                .diffuse = PackColor(255.0f * std::clamp(p.alpha * alphaScale, 0.0f, 1.0f),
                                     mix(ca[0], cb[0], cc[0]),
                                     mix(ca[1], cb[1], cc[1]),
                                     mix(ca[2], cb[2], cc[2])),
                .u = vertexUv ? p.u : mix(a.u, b.u, c.u),
                .v = vertexUv ? p.v : mix(a.v, b.v, c.v),
                .extra0 = mix(a.extra0, b.extra0, c.extra0),
                .extra1 = mix(a.extra1, b.extra1, c.extra1),
            });
        }
    }

    // The paint overlay's triangles on the visible part of the terrain.
    void LayPaint(const HookAddresses& addresses,
                  const TerrainGridDimensions& dimensions,
                  const DrawFrustum& frustum,
                  const TerrainCellFlips& flips,
                  const TerrainDrawRect& drawRect,
                  const PaintOverlay& paint,
                  const float alphaScale,
                  std::vector<PackedTerrainVertex>& output)
    {
        const TerrainPaint::CellRange& range = paint.mesh->Cells();
        PaintDrawCache& cache = *paint.cache;
        const auto rowCount = static_cast<size_t>(range.maxZ - range.minZ + 1);
        if (cache.rows.size() != rowCount) {
            cache.rows.assign(rowCount, {});
        }

        const int zStart = std::max(drawRect.zStart, range.minZ);
        const int zEnd = std::min(drawRect.zEnd, range.maxZ + 1);
        for (int cellZ = zStart; cellZ < zEnd; ++cellZ) {
            int x0 = std::max(drawRect.xStart, range.minX);
            int x1 = std::min(drawRect.xEnd, range.maxX + 1);
            if (!frustum.VisibleCells(cellZ, x0, x1)) {
                continue;
            }
            PaintDrawCache::Row& row = cache.rows[static_cast<size_t>(cellZ - range.minZ)];
            const auto firstVisible = [&] {
                return std::ranges::lower_bound(row.cells, x0, {}, &PaintDrawCache::CellSpan::cellX);
            };
            const auto diagonalChanged = [&] {
                for (auto span = firstVisible(); span != row.cells.end() && span->cellX < x1; ++span) {
                    if (span->flipped != flips.IsFlipped(span->cellX, cellZ)) {
                        return true;
                    }
                }
                return false;
            };
            if (!row.built || x0 < row.builtX0 || x1 > row.builtX1 || diagonalChanged()) {
                // The cells' cuts are kept by the mesh: placing them again is cheap.
                BuildPaintRow(paint, flips, cellZ, row.built ? std::min(x0, row.builtX0) : x0,
                              row.built ? std::max(x1, row.builtX1) : x1, row);
            }

            const std::span<const PaintDrawCache::PlacedVertex> vertices(row.vertices);
            for (auto span = firstVisible(); span != row.cells.end() && span->cellX < x1; ++span) {
                std::array<PackedTerrainVertex, 4> corners{};
                if (!LoadTerrainCellVertices(addresses, dimensions, span->cellX, cellZ, corners)) {
                    continue;
                }
                const bool vertexUv = paint.mesh->HasUvMap();
                const std::array<size_t, 6>& t = CellTriangles(span->flipped);
                LiftVertices(vertices.subspan(span->first, span->lowerCount),
                             corners[t[0]], corners[t[1]], corners[t[2]], alphaScale, vertexUv, output);
                LiftVertices(vertices.subspan(span->first + span->lowerCount, span->upperCount),
                             corners[t[3]], corners[t[4]], corners[t[5]], alphaScale, vertexUv, output);
            }
        }
    }

    void ScaleAlpha(std::vector<PackedTerrainVertex>& vertices, const float scale) noexcept
    {
        for (PackedTerrainVertex& vertex : vertices) {
            const float alpha = std::clamp(UnpackColorChannel(vertex.diffuse, 24) * scale, 0.0f, 255.0f);
            vertex.diffuse = (vertex.diffuse & 0x00FFFFFFu) | (static_cast<uint32_t>(std::lround(alpha)) << 24u);
        }
    }

    void Submit(const DrawRequest& request, const DrawSetup& setup, const std::vector<PackedTerrainVertex>& vertices)
    {
        const HookAddresses& addresses = *request.addresses;
        SC4DrawContext* const context = request.drawContext;

        if (setup.texTransform) {
            reinterpret_cast<SetTexTransform4Fn>(addresses.setTexTransform4)(context, setup.texTransform->data(),
                                                                             setup.texTransformStage);
        }
        if (setup.vertexUv) {
            reinterpret_cast<ClearTexTransformFn>(addresses.clearTexTransform)(context, setup.texTransformStage);
        }
        reinterpret_cast<SetDepthOffsetFn>(addresses.setDepthOffset)(context, setup.depthOffset);
        if (setup.transparency) {
            reinterpret_cast<SetTransparencyFn>(addresses.setTransparency)(context, *setup.transparency);
        }

        const bool ambientFromVertex = (setup.slotFlags & kOverlayFlagSelfLit) == 0u;
        cIGZGDriver* const driver = DrawContextAccess::Driver(context);
        const auto setWrapModes = reinterpret_cast<DrawContextAccess::SetTexWrapModesFn>(addresses.setTexWrapModes);
        if (setup.paint) {
            // The paint texture repeats across the area. DrawDecals takes the diffuse colour,
            // and with it the alpha, from the material; the soft edge needs it per vertex.
            setWrapModes(context, DrawContextAccess::kWrapRepeat, DrawContextAccess::kWrapRepeat, 0);
            if (driver) {
                driver->EnableVertexColors(ambientFromVertex, true);
            }
        }

        if (request.aroundDraw) {
            request.aroundDraw(request.aroundDrawUserData, context, true);
        }
        reinterpret_cast<DrawPrimsFn>(addresses.drawPrims)(context,
                                                           kPrimTypeTriangleList,
                                                           kTerrainVertexFormat,
                                                           static_cast<uint32_t>(vertices.size()),
                                                           vertices.data());
        if (request.aroundDraw) {
            request.aroundDraw(request.aroundDrawUserData, context, false);
        }

        // Put back what DrawDecals set up for this overlay.
        if (setup.paint) {
            if (driver) {
                driver->EnableVertexColors(ambientFromVertex, false);
            }
            const auto [wrapU, wrapV] = VanillaWrapModes(setup.slotFlags);
            setWrapModes(context, wrapU, wrapV, 0);
        }
        if (setup.transparency) {
            reinterpret_cast<SetTransparencyFn>(addresses.setTransparency)(context, setup.slotOpacity);
        }
        reinterpret_cast<SetDepthOffsetFn>(addresses.setDepthOffset)(context, kVanillaDecalDepthOffset);
        if (setup.texTransform || setup.vertexUv) {
            reinterpret_cast<SetTexTransform4Fn>(addresses.setTexTransform4)(context, setup.baseTexTransform,
                                                                             setup.texTransformStage);
        }
    }
}

namespace TerrainDecal
{
    ClippedTerrainDecalRenderer::ClippedTerrainDecalRenderer(const RendererOptions options)
        : options_(options)
    {
    }

    void ClippedTerrainDecalRenderer::SetOptions(const RendererOptions& options) noexcept
    {
        options_ = options;
    }

    const RendererOptions& ClippedTerrainDecalRenderer::GetOptions() const noexcept
    {
        return options_;
    }

    void ClippedTerrainDecalRenderer::SetOverlaySource(ManagedOverlaySource* const source) noexcept
    {
        source_ = source;
    }

    DrawResult ClippedTerrainDecalRenderer::Draw(const DrawRequest& request)
    {
        if (!options_.enableClippedRendering || !request.addresses || !request.overlaySlotBase || !request.drawContext) {
            return DrawResult::FallThroughToVanilla;
        }

        const OverlaySlotView slot = ReadOverlaySlotView(request.overlaySlotBase);
        if (slot.state != -1) {
            return DrawResult::FallThroughToVanilla;
        }

        uint32_t overlayId = 0;
        const bool hasOverlayId = TryResolveOverlayId(request, overlayId);
        if (!MatrixHasFiniteComponents(slot.matrix)) {
            if (FirstTime(overlayId, "nonfinite-matrix")) {
                LOG_WARN("TerrainDecalRenderer: overlay {} has a non-finite matrix, the game draws it", overlayId);
            }
            return DrawResult::FallThroughToVanilla;
        }

        ManagedOverlay managed{};
        const bool isManaged = hasOverlayId && source_ && source_->ResolveOverlay(request.overlayManager, overlayId, managed);
        const bool shadowRecovery = request.mode != DrawMode::Normal;
        if (shadowRecovery && !isManaged) {
            // Only the service's own overlays are drawn again over the shadows.
            return DrawResult::Handled;
        }

        const HookAddresses& addresses = *request.addresses;
        const TerrainGridDimensions dimensions = ReadTerrainGridDimensions(addresses);
        const TerrainDrawRect drawRect = ClampedDrawRect(slot.rect, dimensions);
        const float* const baseTexTransform = request.activeTexTransform ? request.activeTexTransform : slot.matrix;

        thread_local std::vector<PackedTerrainVertex> output;
        output.clear();

        DrawSetup setup{
            .baseTexTransform = baseTexTransform,
            .texTransformStage = request.activeTexTransformStage,
            .slotOpacity = slot.opacity,
            .slotFlags = slot.flags,
        };

        const DrawFrustum frustum(addresses, request.overlayManager, dimensions);
        const TerrainCellFlips flips(addresses, request.overlayManager, dimensions);

        if (isManaged && managed.kind == ManagedOverlay::Kind::Paint) {
            const PaintOverlay& paint = managed.paint;
            if (!paint.mesh || !paint.style || !paint.cache || request.activeTexTransformStage < 0) {
                return DrawResult::Handled;
            }
            PaintDrawCache& cache = *paint.cache;

            // The shadow passes draw again what the decal pass before them laid.
            if (!(shadowRecovery && cache.lastDrawnValid && cache.lastDrawnPass == request.pass)) {
                cache.lastDrawn.clear();
                LayPaint(addresses, dimensions, frustum, flips, drawRect, paint,
                         std::clamp(paint.style->opacity, 0.0f, 1.0f), cache.lastDrawn);
                cache.lastDrawnPass = request.pass;
                cache.lastDrawnValid = true;
            }
            const std::vector<PackedTerrainVertex>* vertices = &cache.lastDrawn;
            const float recoveryScale = request.mode == DrawMode::ShadowRecovery
                                            ? std::clamp(options_.shadowRecoveryOpacityScale, 0.0f, 1.0f)
                                            : 1.0f;
            if (recoveryScale < 1.0f) {
                output = cache.lastDrawn;
                ScaleAlpha(output, recoveryScale);
                vertices = &output;
            }
            if (vertices->empty()) {
                return DrawResult::Handled;
            }

            if (paint.mesh->HasUvMap()) {
                setup.vertexUv = addresses.clearTexTransform != 0;
            }
            else {
                setup.texTransform = BuildPaintTextureTransform(baseTexTransform, slot.matrix, paint.uvFromGround);
            }
            if (!setup.texTransform && !setup.vertexUv) {
                if (FirstTime(overlayId, "paint-matrix")) {
                    LOG_WARN("TerrainDecalRenderer: paint overlay {} has a singular slot matrix", overlayId);
                }
                return DrawResult::Handled;
            }
            setup.paint = true;
            // The opacity is in the vertex alpha.
            setup.transparency = 1.0f;
            setup.depthOffset = shadowRecovery ? options_.shadowRecoveryDepthOffset
                                : paint.style->depthOffset >= 0 ? paint.style->depthOffset
                                                                : options_.customDefaultDepthOffset;
            Submit(request, setup, *vertices);
            return DrawResult::Handled;
        }

        // A decal: the game's, or one of the service's with its overrides.
        const DecalOverrides& overrides = managed.decal;
        const bool clipToWindow = isManaged && overrides.hasUvWindow && overrides.uvWindow.mode == TerrainDecalUvMode::ClipSubrect;
        const bool stretchToWindow = isManaged && overrides.hasUvWindow && overrides.uvWindow.mode == TerrainDecalUvMode::StretchSubrect;
        if (overrides.hasUvWindow && !IsValidUvWindow(overrides.uvWindow)) {
            if (FirstTime(overlayId, "bad-uv-window")) {
                LOG_WARN("TerrainDecalRenderer: overlay {} has an invalid UV window, the game draws it", overlayId);
            }
            return shadowRecovery ? DrawResult::Handled : DrawResult::FallThroughToVanilla;
        }

        const bool clipU = ShouldClipU(slot.flags) || clipToWindow;
        const bool clipV = ShouldClipV(slot.flags) || clipToWindow;
        const ClipBounds bounds = clipToWindow
                                      ? ClipBounds{overrides.uvWindow.u1, overrides.uvWindow.u2, overrides.uvWindow.v1, overrides.uvWindow.v2}
                                      : ClipBounds{};
        setup.texTransform = isManaged ? BuildDecalTextureTransform(baseTexTransform, overrides, stretchToWindow) : std::nullopt;
        if (setup.texTransform && request.activeTexTransformStage < 0) {
            if (FirstTime(overlayId, "no-tex-stage")) {
                LOG_WARN("TerrainDecalRenderer: overlay {} needs a texture transform but none was captured", overlayId);
            }
            return shadowRecovery ? DrawResult::Handled : DrawResult::FallThroughToVanilla;
        }
        if (!shadowRecovery && !clipU && !clipV && !setup.texTransform && !isManaged) {
            return DrawResult::FallThroughToVanilla;
        }

        bool loadedAnyCell = false;
        for (int cellZ = drawRect.zStart; cellZ < drawRect.zEnd; ++cellZ) {
            int x0 = drawRect.xStart;
            int x1 = drawRect.xEnd;
            if (!frustum.VisibleCells(cellZ, x0, x1)) {
                continue;
            }
            for (int cellX = x0; cellX < x1; ++cellX) {
                std::array<PackedTerrainVertex, 4> corners{};
                if (!LoadTerrainCellVertices(addresses, dimensions, cellX, cellZ, corners)) {
                    continue;
                }
                loadedAnyCell = true;

                std::array<ClipVertex, 4> quad{};
                bool finite = true;
                for (size_t i = 0; i < quad.size(); ++i) {
                    quad[i].vertex = corners[i];
                    EvaluateFootprintUv(slot.matrix, quad[i]);
                    finite = finite && HasFiniteClipUv(quad[i]);
                }
                if (!finite || !QuadMayIntersectClipBox(quad, clipU, clipV, bounds)) {
                    continue;
                }

                const std::array<size_t, 6>& triangles = CellTriangles(flips.IsFlipped(cellX, cellZ));
                if (std::all_of(quad.begin(), quad.end(), [&](const ClipVertex& v) { return IsInside(v, clipU, clipV, bounds); })) {
                    for (const size_t i : triangles) {
                        output.push_back(quad[i].vertex);
                    }
                }
                else {
                    ClipAndEmitCell(quad, triangles, clipU, clipV, bounds, output);
                }
            }
        }

        if (output.empty()) {
            // Without terrain the globals are not ready yet, and a game decal whose clip came
            // out empty is left to the game as before: let the game try either way.
            if (shadowRecovery || (loadedAnyCell && isManaged)) {
                return DrawResult::Handled;
            }
            return DrawResult::FallThroughToVanilla;
        }

        setup.depthOffset = shadowRecovery ? options_.shadowRecoveryDepthOffset
                            : overrides.depthOffset >= 0 ? overrides.depthOffset
                                                         : options_.customDefaultDepthOffset;
        if (request.mode == DrawMode::ShadowRecovery && std::isfinite(slot.opacity)) {
            setup.transparency = slot.opacity * std::clamp(options_.shadowRecoveryOpacityScale, 0.0f, 1.0f);
        }
        Submit(request, setup, output);
        return DrawResult::Handled;
    }
}
