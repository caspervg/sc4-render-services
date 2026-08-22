#include "ClippedTerrainDecalRenderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "cISTETerrain.h"
#include "utils/Logger.h"

namespace
{
    // SC4 uses primType 0 for caller-supplied explicit triangles.
    constexpr uint32_t kPrimTypeTriangleList = 0;
    constexpr uint32_t kTerrainVertexFormat = 0x0B;
    constexpr float kClipEpsilon = 1.0e-5f;
    using SetTexTransform4Fn = void(__thiscall*)(SC4DrawContext*, const float*, int);

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

    struct PackedTerrainVertex
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

    static_assert(sizeof(PackedTerrainVertex) == 0x20,
                  "PackedTerrainVertex must match the game's 32-byte terrain vertex layout.");

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
    };

    struct TextureTransformOverride
    {
        std::array<float, 16> adjusted{};
        bool active = false;
    };

    struct ClipBounds
    {
        float minU = 0.0f;
        float maxU = 1.0f;
        float minV = 0.0f;
        float maxV = 1.0f;
    };

    struct ClipDebugSample
    {
        bool captured = false;
        int cellX = 0;
        int cellZ = 0;
        std::array<PackedTerrainVertex, 4> sourceVertices{};
        std::array<ClipVertex, 4> slotVertices{};
        std::array<ClipVertex, 4> activeVertices{};
        bool activeTransformUsed = false;
        bool slotMayIntersect = false;
        bool activeMayIntersect = false;
        bool slotAllInside = false;
        bool activeAllInside = false;
    };

    [[nodiscard]] bool ShouldLogOverlayOnce(const uint32_t overlayId, const char* const category) noexcept
    {
        static std::mutex mutex;
        static std::unordered_set<uint64_t> seenKeys;

        const uint64_t key =
            (static_cast<uint64_t>(std::hash<std::string_view>{}(category)) << 32u) | overlayId;

        std::lock_guard lock(mutex);
        return seenKeys.insert(key).second;
    }

    void LogClipDebugSample(const uint32_t overlayId,
                            const ClipDebugSample& sample,
                            const ClipBounds& bounds,
                            const bool clipU,
                            const bool clipV) noexcept
    {
        if (!sample.captured) {
            return;
        }

        const auto& sv = sample.slotVertices;
        LOG_TRACE(
            "TerrainDecalRenderer: overlay {} clip sample cell=({}, {}) bounds u[{:.3f},{:.3f}] v[{:.3f},{:.3f}] clipU={} clipV={} "
            "slot intersects={} inside={} "
            "slotUVs=[({:.3f},{:.3f}), ({:.3f},{:.3f}), ({:.3f},{:.3f}), ({:.3f},{:.3f})] "
            "activeUsed={} active intersects={} inside={} "
            "activeUVs=[({:.3f},{:.3f}), ({:.3f},{:.3f}), ({:.3f},{:.3f}), ({:.3f},{:.3f})]",
            overlayId,
            sample.cellX,
            sample.cellZ,
            bounds.minU,
            bounds.maxU,
            bounds.minV,
            bounds.maxV,
            clipU,
            clipV,
            sample.slotMayIntersect,
            sample.slotAllInside,
            sv[0].clipU,
            sv[0].clipV,
            sv[1].clipU,
            sv[1].clipV,
            sv[2].clipU,
            sv[2].clipV,
            sv[3].clipU,
            sv[3].clipV,
            sample.activeTransformUsed,
            sample.activeMayIntersect,
            sample.activeAllInside,
            sample.activeVertices[0].clipU,
            sample.activeVertices[0].clipV,
            sample.activeVertices[1].clipU,
            sample.activeVertices[1].clipV,
            sample.activeVertices[2].clipU,
            sample.activeVertices[2].clipV,
            sample.activeVertices[3].clipU,
            sample.activeVertices[3].clipV);
    }

    [[nodiscard]] bool HasFinitePosition(const PackedTerrainVertex& vertex) noexcept
    {
        return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z);
    }

    [[nodiscard]] bool AllSourceVerticesHaveFinitePosition(const std::array<PackedTerrainVertex, 4>& vertices) noexcept
    {
        return std::all_of(vertices.begin(), vertices.end(), [](const PackedTerrainVertex& vertex) {
            return HasFinitePosition(vertex);
        });
    }

    [[nodiscard]] bool MatrixHasFiniteComponents(const float* matrix) noexcept
    {
        if (!matrix) {
            return false;
        }

        for (size_t i = 0; i < 16; ++i) {
            if (!std::isfinite(matrix[i])) {
                return false;
            }
        }

        return true;
    }

    void LogClipNanSample(const uint32_t overlayId,
                          const ClipDebugSample& sample,
                          const float* matrix) noexcept
    {
        if (!sample.captured) {
            return;
        }

        const auto& sv = sample.sourceVertices;
        LOG_TRACE(
            "TerrainDecalRenderer: overlay {} clip-nan cell=({}, {}) finiteSource={} finiteMatrix={} "
            "source=[({:.3f},{:.3f},{:.3f}), ({:.3f},{:.3f},{:.3f}), ({:.3f},{:.3f},{:.3f}), ({:.3f},{:.3f},{:.3f})] "
            "matrixRow0=[{:.6f},{:.6f},{:.6f},{:.6f}] matrixRow1=[{:.6f},{:.6f},{:.6f},{:.6f}] "
            "matrixRow2=[{:.6f},{:.6f},{:.6f},{:.6f}] matrixRow3=[{:.6f},{:.6f},{:.6f},{:.6f}]",
            overlayId,
            sample.cellX,
            sample.cellZ,
            AllSourceVerticesHaveFinitePosition(sample.sourceVertices),
            MatrixHasFiniteComponents(matrix),
            sv[0].x,
            sv[0].y,
            sv[0].z,
            sv[1].x,
            sv[1].y,
            sv[1].z,
            sv[2].x,
            sv[2].y,
            sv[2].z,
            sv[3].x,
            sv[3].y,
            sv[3].z,
            matrix ? matrix[0] : 0.0f,
            matrix ? matrix[4] : 0.0f,
            matrix ? matrix[8] : 0.0f,
            matrix ? matrix[12] : 0.0f,
            matrix ? matrix[1] : 0.0f,
            matrix ? matrix[5] : 0.0f,
            matrix ? matrix[9] : 0.0f,
            matrix ? matrix[13] : 0.0f,
            matrix ? matrix[2] : 0.0f,
            matrix ? matrix[6] : 0.0f,
            matrix ? matrix[10] : 0.0f,
            matrix ? matrix[14] : 0.0f,
            matrix ? matrix[3] : 0.0f,
            matrix ? matrix[7] : 0.0f,
            matrix ? matrix[11] : 0.0f,
            matrix ? matrix[15] : 0.0f);
    }

    void LogNonFiniteMatrixSample(const uint32_t overlayId, const float* matrix) noexcept
    {
        LOG_TRACE(
            "TerrainDecalRenderer: overlay {} falling through because slot matrix is non-finite "
            "matrixRow0=[{:.6f},{:.6f},{:.6f},{:.6f}] matrixRow1=[{:.6f},{:.6f},{:.6f},{:.6f}] "
            "matrixRow2=[{:.6f},{:.6f},{:.6f},{:.6f}] matrixRow3=[{:.6f},{:.6f},{:.6f},{:.6f}]",
            overlayId,
            matrix ? matrix[0] : 0.0f,
            matrix ? matrix[4] : 0.0f,
            matrix ? matrix[8] : 0.0f,
            matrix ? matrix[12] : 0.0f,
            matrix ? matrix[1] : 0.0f,
            matrix ? matrix[5] : 0.0f,
            matrix ? matrix[9] : 0.0f,
            matrix ? matrix[13] : 0.0f,
            matrix ? matrix[2] : 0.0f,
            matrix ? matrix[6] : 0.0f,
            matrix ? matrix[10] : 0.0f,
            matrix ? matrix[14] : 0.0f,
            matrix ? matrix[3] : 0.0f,
            matrix ? matrix[7] : 0.0f,
            matrix ? matrix[11] : 0.0f,
            matrix ? matrix[15] : 0.0f);
    }

    [[nodiscard]] uint32_t NormalizeOverlayIdKey(const uint32_t overlayId) noexcept
    {
        return overlayId & 0x7FFFFFFFu;
    }

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

    using DrawPrimsFn = void(__thiscall*)(SC4DrawContext*, uint32_t, uint32_t, uint32_t, const void*);
    using SetDepthOffsetFn = void(__thiscall*)(SC4DrawContext*, int);
    // SC4DrawContext::SetTransparency is a tail-call shim to the renderer; the float
    // opacity stays on the stack just like the original DrawDecals call sequence.
    using SetTransparencyFn = void(__thiscall*)(SC4DrawContext*, float);

    // Vanilla DrawDecals uses depth offset 2; DrawShadows uses 3 (higher = closer to camera in SC4).
    // Set decals to 4+ to ensure they win the depth test against shadow pixels.
    constexpr int kVanillaDecalDepthOffset = 2;
    constexpr std::ptrdiff_t kOverlaySlotOpacityOffset = 0x9C;

    [[nodiscard]] OverlaySlotView ReadOverlaySlotView(const std::byte* slotBase)
    {
        OverlaySlotView result{};
        if (!slotBase) {
            return result;
        }

        result.state = *reinterpret_cast<const int32_t*>(slotBase + 0x00);
        result.flags = *reinterpret_cast<const uint32_t*>(slotBase + 0x04);
        result.rect = *reinterpret_cast<const TerrainDrawRect*>(slotBase + 0x0C);
        result.matrix = reinterpret_cast<const float*>(slotBase + 0x1C);
        return result;
    }

    [[nodiscard]] bool ShouldClipU(const uint32_t flags) noexcept
    {
        if ((flags & 0x20u) != 0u) {
            return false;
        }

        return (flags & 0x2u) == 0u;
    }

    [[nodiscard]] bool ShouldClipV(const uint32_t flags) noexcept
    {
        return (flags & 0x20u) == 0u;
    }

    [[nodiscard]] bool IsValidUvWindow(const TerrainDecalUvWindow& uvRect) noexcept
    {
        return std::isfinite(uvRect.u1) &&
               std::isfinite(uvRect.v1) &&
               std::isfinite(uvRect.u2) &&
               std::isfinite(uvRect.v2) &&
               uvRect.u1 < uvRect.u2 &&
               uvRect.v1 < uvRect.v2;
    }

    [[nodiscard]] const char* DescribeOverlayUvMode(const TerrainDecalUvMode mode) noexcept
    {
        switch (mode) {
        case TerrainDecalUvMode::StretchSubrect:
            return "stretch";
        case TerrainDecalUvMode::ClipSubrect:
            return "clip";
        default:
            return "unknown";
        }
    }

    [[nodiscard]] float ReadOverlaySlotOpacity(const std::byte* const slotBase) noexcept
    {
        float opacity = 1.0f;
        if (slotBase) {
            std::memcpy(&opacity, slotBase + kOverlaySlotOpacityOffset, sizeof(opacity));
        }
        return opacity;
    }

    void SetOverlayTransparency(SC4DrawContext* const drawContext,
                                const TerrainDecal::HookAddresses& addresses,
                                const float opacity) noexcept
    {
        if (drawContext && addresses.setTransparency) {
            const auto setTransparency = reinterpret_cast<SetTransparencyFn>(addresses.setTransparency);
            setTransparency(drawContext, opacity);
        }
    }

    [[nodiscard]] bool TryResolveOverlayId(const TerrainDecal::DrawRequest& request, uint32_t& overlayId) noexcept
    {
        overlayId = 0;

        if (!request.addresses ||
            !request.overlayManager ||
            !request.overlaySlotBase ||
            request.addresses->overlaySlotsPtrOffset <= 0 ||
            request.addresses->overlaySlotStride <= 0) {
            return false;
        }

        const auto* const overlayManagerBytes = reinterpret_cast<const std::byte*>(request.overlayManager);
        const auto* const slotsBase =
            *reinterpret_cast<const std::byte* const*>(overlayManagerBytes + request.addresses->overlaySlotsPtrOffset);
        if (!slotsBase) {
            return false;
        }

        const auto slotAddress = reinterpret_cast<uintptr_t>(request.overlaySlotBase);
        const auto slotsBaseAddress = reinterpret_cast<uintptr_t>(slotsBase);
        if (slotAddress < slotsBaseAddress) {
            return false;
        }

        const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(slotAddress - slotsBaseAddress);
        if ((delta % request.addresses->overlaySlotStride) != 0) {
            return false;
        }

        const auto slotIndex = delta / request.addresses->overlaySlotStride;
        if (slotIndex < 0) {
            return false;
        }

        overlayId = static_cast<uint32_t>(slotIndex);
        return true;
    }

    [[nodiscard]] TextureTransformOverride BuildTextureTransformOverride(
        const float* baseTransform,
        const TerrainDecalUvWindow& uvRect,
        const bool applyUvWindow,
        const float aspectMultiplier,
        const float uvScaleU,
        const float uvScaleV,
        const float uvOffset,
        const bool mirror) noexcept
    {
        TextureTransformOverride result{};
        if (!baseTransform) {
            return result;
        }
        if (applyUvWindow && !IsValidUvWindow(uvRect)) {
            return result;
        }

        result.adjusted = {
            baseTransform[0],  baseTransform[1],  baseTransform[2],  baseTransform[3],
            baseTransform[4],  baseTransform[5],  baseTransform[6],  baseTransform[7],
            baseTransform[8],  baseTransform[9],  baseTransform[10], baseTransform[11],
            baseTransform[12], baseTransform[13], baseTransform[14], baseTransform[15]
        };

        constexpr int uColumn[] = {0, 4, 8, 12};
        constexpr int vColumn[] = {1, 5, 9, 13};
        constexpr int wColumn[] = {3, 7, 11, 15};

        const bool applyModifiers =
            aspectMultiplier != 1.0f || uvScaleU != 1.0f ||
            uvScaleV != 1.0f || uvOffset != 0.0f;

        if (applyModifiers) {
            const float scaleU = uvScaleU * aspectMultiplier;
            const float scaleV = uvScaleV;
            for (size_t i = 0; i < std::size(uColumn); ++i) {
                result.adjusted[uColumn[i]] =
                    scaleU * baseTransform[uColumn[i]] +
                    uvOffset * baseTransform[wColumn[i]];
                result.adjusted[vColumn[i]] =
                    scaleV * baseTransform[vColumn[i]] +
                    uvOffset * baseTransform[wColumn[i]];
            }
        }

        if (mirror) {
            // u' = 1 - u keeps UVs inside [0,1]; applied before the UV window so
            // the flip happens within the chosen sub-rect.
            for (size_t i = 0; i < std::size(uColumn); ++i) {
                result.adjusted[uColumn[i]] =
                    result.adjusted[wColumn[i]] - result.adjusted[uColumn[i]];
            }
        }

        if (applyUvWindow) {
            const float width = uvRect.u2 - uvRect.u1;
            const float height = uvRect.v2 - uvRect.v1;
            std::array<float, 16> modified = result.adjusted;
            for (size_t i = 0; i < std::size(uColumn); ++i) {
                result.adjusted[uColumn[i]] = width * modified[uColumn[i]] +
                                              uvRect.u1 * modified[wColumn[i]];
                result.adjusted[vColumn[i]] = height * modified[vColumn[i]] +
                                              uvRect.v1 * modified[wColumn[i]];
            }
        }

        result.active = applyModifiers || mirror || applyUvWindow;
        return result;
    }

    [[nodiscard]] float UnpackColorChannel(const uint32_t color, const int shift) noexcept
    {
        return static_cast<float>((color >> shift) & 0xFFu);
    }

    [[nodiscard]] uint32_t PackColor(const float a, const float r, const float g, const float b) noexcept
    {
        const auto clampByte = [](const float value) -> uint32_t {
            const float clamped = std::clamp(value, 0.0f, 255.0f);
            return static_cast<uint32_t>(std::lround(clamped));
        };

        return (clampByte(a) << 24u) |
               (clampByte(r) << 16u) |
               (clampByte(g) << 8u) |
               clampByte(b);
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

        const float aA = UnpackColorChannel(a.diffuse, 24);
        const float aR = UnpackColorChannel(a.diffuse, 16);
        const float aG = UnpackColorChannel(a.diffuse, 8);
        const float aB = UnpackColorChannel(a.diffuse, 0);
        const float bA = UnpackColorChannel(b.diffuse, 24);
        const float bR = UnpackColorChannel(b.diffuse, 16);
        const float bG = UnpackColorChannel(b.diffuse, 8);
        const float bB = UnpackColorChannel(b.diffuse, 0);

        out.diffuse = PackColor(std::lerp(aA, bA, t),
                                std::lerp(aR, bR, t),
                                std::lerp(aG, bG, t),
                                std::lerp(aB, bB, t));
        return out;
    }

    void EvaluateFootprintUv(const float* matrix, ClipVertex& vertex) noexcept
    {
        if (!matrix) {
            return;
        }

        const float sourceX = vertex.vertex.x;
        const float sourceY = vertex.vertex.y;
        const float sourceZ = vertex.vertex.z;

        const float u = sourceX * matrix[0] + sourceY * matrix[4] + sourceZ * matrix[8] + matrix[12];
        const float v = sourceX * matrix[1] + sourceY * matrix[5] + sourceZ * matrix[9] + matrix[13];
        const float w = sourceX * matrix[3] + sourceY * matrix[7] + sourceZ * matrix[11] + matrix[15];

        if (std::fabs(w) > kClipEpsilon && std::fabs(w - 1.0f) > kClipEpsilon) {
            vertex.clipU = u / w;
            vertex.clipV = v / w;
        }
        else {
            vertex.clipU = u;
            vertex.clipV = v;
        }
    }

    [[nodiscard]] ClipVertex LerpClipVertex(const ClipVertex& a,
                                            const ClipVertex& b,
                                            const float t) noexcept
    {
        ClipVertex out{};
        out.vertex = LerpVertex(a.vertex, b.vertex, t);
        out.clipU = std::lerp(a.clipU, b.clipU, t);
        out.clipV = std::lerp(a.clipV, b.clipV, t);
        return out;
    }

    [[nodiscard]] bool HasFiniteClipUv(const ClipVertex& vertex) noexcept
    {
        return std::isfinite(vertex.clipU) && std::isfinite(vertex.clipV);
    }

    [[nodiscard]] bool AllVerticesHaveFiniteClipUv(const std::array<ClipVertex, 4>& vertices) noexcept
    {
        return std::all_of(vertices.begin(), vertices.end(), [](const ClipVertex& vertex) {
            return HasFiniteClipUv(vertex);
        });
    }

    [[nodiscard]] bool IsInsidePlane(const ClipVertex& v,
                                     const bool useU,
                                     const bool isMinPlane,
                                     const float limit) noexcept
    {
        if (!HasFiniteClipUv(v)) {
            return false;
        }

        const float value = useU ? v.clipU : v.clipV;
        return isMinPlane ? value >= (limit - kClipEpsilon) : value <= (limit + kClipEpsilon);
    }

    [[nodiscard]] ClipVertex IntersectPlane(const ClipVertex& a,
                                            const ClipVertex& b,
                                            const bool useU,
                                            const float limit) noexcept
    {
        const float av = useU ? a.clipU : a.clipV;
        const float bv = useU ? b.clipU : b.clipV;
        const float denom = bv - av;
        const float t = std::fabs(denom) > kClipEpsilon ? (limit - av) / denom : 0.0f;
        return LerpClipVertex(a, b, std::clamp(t, 0.0f, 1.0f));
    }

    void ClipPolygonAgainstPlane(std::vector<ClipVertex>& polygon,
                                 const bool useU,
                                 const bool isMinPlane,
                                 const float limit)
    {
        if (polygon.empty()) {
            return;
        }

        std::vector<ClipVertex> output;
        output.reserve(polygon.size() + 2);

        ClipVertex previous = polygon.back();
        bool previousInside = IsInsidePlane(previous, useU, isMinPlane, limit);

        for (const auto& current : polygon) {
            const bool currentInside = IsInsidePlane(current, useU, isMinPlane, limit);

            if (currentInside != previousInside) {
                output.push_back(IntersectPlane(previous, current, useU, limit));
            }

            if (currentInside) {
                output.push_back(current);
            }

            previous = current;
            previousInside = currentInside;
        }

        polygon = std::move(output);
    }

    void EmitTriangleFan(const std::vector<ClipVertex>& polygon,
                         std::vector<PackedTerrainVertex>& output)
    {
        if (polygon.size() < 3) {
            return;
        }

        for (size_t i = 1; i + 1 < polygon.size(); ++i) {
            output.push_back(polygon[0].vertex);
            output.push_back(polygon[i].vertex);
            output.push_back(polygon[i + 1].vertex);
        }
    }

    void ClipAndEmitPolygon(std::vector<ClipVertex> polygon,
                             const bool clipU,
                             const bool clipV,
                             const ClipBounds& bounds,
                             std::vector<PackedTerrainVertex>& output)
    {
        if (clipU) {
            ClipPolygonAgainstPlane(polygon, true, true, bounds.minU);
            ClipPolygonAgainstPlane(polygon, true, false, bounds.maxU);
        }

        if (clipV) {
            ClipPolygonAgainstPlane(polygon, false, true, bounds.minV);
            ClipPolygonAgainstPlane(polygon, false, false, bounds.maxV);
        }

        EmitTriangleFan(polygon, output);
    }

    [[nodiscard]] bool AllVerticesInside(const std::array<ClipVertex, 4>& vertices,
                                         const bool clipU,
                                         const bool clipV,
                                         const ClipBounds& bounds) noexcept
    {
        return std::all_of(vertices.begin(), vertices.end(), [clipU, clipV, bounds](const ClipVertex& vertex) {
            if (!HasFiniteClipUv(vertex)) {
                return false;
            }

            const bool insideU = !clipU || (vertex.clipU >= bounds.minU - kClipEpsilon &&
                                            vertex.clipU <= bounds.maxU + kClipEpsilon);
            const bool insideV = !clipV || (vertex.clipV >= bounds.minV - kClipEpsilon &&
                                            vertex.clipV <= bounds.maxV + kClipEpsilon);
            return insideU && insideV;
        });
    }

    [[nodiscard]] bool AnyVertexInside(const std::array<ClipVertex, 4>& vertices,
                                       const bool clipU,
                                       const bool clipV,
                                       const ClipBounds& bounds) noexcept
    {
        return std::any_of(vertices.begin(), vertices.end(), [clipU, clipV, bounds](const ClipVertex& vertex) {
            if (!HasFiniteClipUv(vertex)) {
                return false;
            }

            const bool insideU = !clipU || (vertex.clipU >= bounds.minU - kClipEpsilon &&
                                            vertex.clipU <= bounds.maxU + kClipEpsilon);
            const bool insideV = !clipV || (vertex.clipV >= bounds.minV - kClipEpsilon &&
                                            vertex.clipV <= bounds.maxV + kClipEpsilon);
            return insideU && insideV;
        });
    }

    [[nodiscard]] bool QuadMayIntersectClipBox(const std::array<ClipVertex, 4>& vertices,
                                               const bool clipU,
                                               const bool clipV,
                                               const ClipBounds& bounds) noexcept
    {
        if (!AllVerticesHaveFiniteClipUv(vertices)) {
            return false;
        }

        if (AnyVertexInside(vertices, clipU, clipV, bounds)) {
            return true;
        }

        float minU = vertices[0].clipU;
        float maxU = vertices[0].clipU;
        float minV = vertices[0].clipV;
        float maxV = vertices[0].clipV;

        for (const auto& vertex : vertices) {
            minU = std::min(minU, vertex.clipU);
            maxU = std::max(maxU, vertex.clipU);
            minV = std::min(minV, vertex.clipV);
            maxV = std::max(maxV, vertex.clipV);
        }

        const bool overlapsU = !clipU || !(maxU < bounds.minU || minU > bounds.maxU);
        const bool overlapsV = !clipV || !(maxV < bounds.minV || minV > bounds.maxV);
        return overlapsU && overlapsV;
    }

    [[nodiscard]] const RowTableEntry* ReadRowTable(const uintptr_t globalAddress) noexcept
    {
        if (globalAddress == 0) {
            return nullptr;
        }

        return *reinterpret_cast<const RowTableEntry* const*>(globalAddress);
    }

    [[nodiscard]] const PackedTerrainVertex* GetTerrainVertexArray(const uintptr_t globalAddress) noexcept
    {
        if (globalAddress == 0) {
            return nullptr;
        }

        return *reinterpret_cast<const PackedTerrainVertex* const*>(globalAddress);
    }

    [[nodiscard]] const CellInfoEntry* GetCellInfoRow(const RowTableEntry* rows, const int row) noexcept
    {
        if (!rows || row < 0) {
            return nullptr;
        }

        return reinterpret_cast<const CellInfoEntry*>(rows[row].data);
    }

    [[nodiscard]] const uint16_t* ReadAllLevelCellIndices(const uintptr_t globalAddress) noexcept
    {
        if (globalAddress == 0) {
            return nullptr;
        }

        return reinterpret_cast<const uint16_t*>(*reinterpret_cast<const void* const*>(globalAddress));
    }

    [[nodiscard]] TerrainGridDimensions ReadTerrainGridDimensions(const TerrainDecal::HookAddresses& addresses) noexcept
    {
        TerrainGridDimensions result{};
        if (addresses.terrainCellCountXPtr != 0) {
            result.cellCountX = *reinterpret_cast<const int*>(addresses.terrainCellCountXPtr);
        }

        if (addresses.terrainCellCountZPtr != 0) {
            result.cellCountZ = *reinterpret_cast<const int*>(addresses.terrainCellCountZPtr);
        }

        if (addresses.terrainVertexCountXPtr != 0) {
            result.vertexCountX = *reinterpret_cast<const int*>(addresses.terrainVertexCountXPtr);
        }

        if (addresses.terrainVertexCountZPtr != 0) {
            result.vertexCountZ = *reinterpret_cast<const int*>(addresses.terrainVertexCountZPtr);
        }

        if (addresses.terrainVertexCountPtr != 0) {
            result.vertexCount = *reinterpret_cast<const int*>(addresses.terrainVertexCountPtr);
        }

        if (result.vertexCountZ <= 0 && result.vertexCountX > 0 && result.vertexCount > 0) {
            result.vertexCountZ = result.vertexCount / result.vertexCountX;
        }

        return result;
    }

    [[nodiscard]] TerrainDrawRect ClampTerrainDrawRect(const TerrainDrawRect& rect,
                                                       const TerrainGridDimensions& dimensions) noexcept
    {
        TerrainDrawRect result{};
        result.xStart = std::clamp(rect.xStart, 0, std::max(0, dimensions.cellCountX));
        result.zStart = std::clamp(rect.zStart, 0, std::max(0, dimensions.cellCountZ));
        result.xEnd = std::clamp(rect.xEnd, result.xStart, std::max(0, dimensions.cellCountX));
        result.zEnd = std::clamp(rect.zEnd, result.zStart, std::max(0, dimensions.cellCountZ));
        return result;
    }

    [[nodiscard]] TerrainDrawRect MakeExclusiveTerrainDrawRect(const TerrainDrawRect& rect) noexcept
    {
        TerrainDrawRect result = rect;
        if (result.xEnd < std::numeric_limits<int>::max()) {
            result.xEnd += 1;
        }
        if (result.zEnd < std::numeric_limits<int>::max()) {
            result.zEnd += 1;
        }
        return result;
    }

    [[nodiscard]] bool LoadTerrainCellVertices(const TerrainDecal::HookAddresses& addresses,
                                               const int cellX,
                                               const int cellZ,
                                               std::array<PackedTerrainVertex, 4>& result)
    {
        const TerrainGridDimensions dimensions = ReadTerrainGridDimensions(addresses);
        if (dimensions.cellCountX <= 0 || dimensions.cellCountZ <= 0 ||
            dimensions.vertexCountX <= 0 || dimensions.vertexCountZ <= 0 || dimensions.vertexCount <= 0) {
            return false;
        }

        if (cellX >= dimensions.cellCountX || cellZ >= dimensions.cellCountZ) {
            return false;
        }

        const auto* const vertices = GetTerrainVertexArray(addresses.terrainGridVerticesPtr);
        const auto* const rows = ReadRowTable(addresses.terrainCellInfoRowsPtr);
        const auto* const allLevelCellIndices = ReadAllLevelCellIndices(addresses.allLevelCellIndicesPtr);
        if (!vertices) {
            return false;
        }
        if (!rows || !allLevelCellIndices) {
            return false;
        }

        const int vertexCountX = dimensions.vertexCountX;
        const int levelIndexStride = dimensions.cellCountX + 1;
        const int levelIndexBase = levelIndexStride * cellZ;
        const uint16_t levelEntryStart = allLevelCellIndices[levelIndexBase + cellX];
        const uint16_t levelEntryEnd = allLevelCellIndices[levelIndexBase + cellX + 1];

        int rowRelativeIndex = cellX;
        const CellInfoEntry* levelEntry = nullptr;
        if (levelEntryStart < levelEntryEnd) {
            const auto* const row = GetCellInfoRow(rows, cellZ);
            if (!row) {
                return false;
            }
            levelEntry = &row[levelEntryStart];
            rowRelativeIndex = levelEntry->vertexIndex;
        }
        if (rowRelativeIndex < 0) {
            return false;
        }

        const int baseIndex = cellZ * vertexCountX + rowRelativeIndex;
        if (baseIndex + vertexCountX + 1 >= dimensions.vertexCount) {
            return false;
        }

        result[0] = vertices[baseIndex];
        result[1] = vertices[baseIndex + vertexCountX];
        result[2] = vertices[baseIndex + vertexCountX + 1];
        result[3] = vertices[baseIndex + 1];

        if (levelEntry) {
            // The game uses sAllLevelCellIndices to map a terrain cell (x, z) to an optional
            // leveled entry in sLevelCellInfos[z]. When present, that entry overrides both the
            // X-relative vertex index and the flattened cell height.
            const float flatY = std::bit_cast<float>(levelEntry->flatYBits);
            result[0].y = flatY;
            result[1].y = flatY;
            result[2].y = flatY;
            result[3].y = flatY;
        }

        return true;
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

    void ClippedTerrainDecalRenderer::SetOverlayUvWindow(const uint32_t overlayId, const TerrainDecalUvWindow& uvRect)
    {
        const uint32_t normalizedOverlayId = NormalizeOverlayIdKey(overlayId);
        overlayUvWindows_[normalizedOverlayId] = uvRect;
        LOG_INFO("TerrainDecalRenderer: registered UV override for overlay {} (normalized {}, mode={}) -> [{:.3f}, {:.3f}] to [{:.3f}, {:.3f}]",
                 overlayId,
                 normalizedOverlayId,
                 DescribeOverlayUvMode(uvRect.mode),
                 uvRect.u1,
                 uvRect.v1,
                 uvRect.u2,
                 uvRect.v2);
    }

    bool ClippedTerrainDecalRenderer::RemoveOverlayUvWindow(const uint32_t overlayId) noexcept
    {
        const uint32_t normalizedOverlayId = NormalizeOverlayIdKey(overlayId);
        const bool removed = overlayUvWindows_.erase(normalizedOverlayId) > 0;
        if (removed) {
            LOG_INFO("TerrainDecalRenderer: removed UV override for overlay {} (normalized {})",
                     overlayId,
                     normalizedOverlayId);
        }
        return removed;
    }

    void ClippedTerrainDecalRenderer::ClearOverlayUvWindows() noexcept
    {
        if (!overlayUvWindows_.empty()) {
            LOG_INFO("TerrainDecalRenderer: cleared {} UV override entries", overlayUvWindows_.size());
        }
        overlayUvWindows_.clear();
    }

    bool ClippedTerrainDecalRenderer::TryGetOverlayUvWindow(const uint32_t overlayId,
                                                            TerrainDecalUvWindow& uvRect) const noexcept
    {
        const uint32_t normalizedOverlayId = NormalizeOverlayIdKey(overlayId);
        const auto it = overlayUvWindows_.find(normalizedOverlayId);
        if (it == overlayUvWindows_.end()) {
            return false;
        }

        uvRect = it->second;
        return true;
    }

    void ClippedTerrainDecalRenderer::SetOverlayOverridesResolver(const OverlayOverridesResolver resolver,
                                                                  void* const userData) noexcept
    {
        overlayOverridesResolver_ = resolver;
        overlayOverridesResolverUserData_ = userData;
    }

    DrawResult ClippedTerrainDecalRenderer::Draw(const DrawRequest& request)
    {
        const bool debugOverridesActive = !overlayUvWindows_.empty();
        const bool shadowRecovery = request.mode != DrawMode::Normal;

        if (!options_.enableClippedRendering) {
            LOG_WARN("TerrainDecalRenderer: falling through to vanilla because clipped rendering is disabled");
            return DrawResult::FallThroughToVanilla;
        }

        if (!request.addresses || !request.terrain || !request.overlaySlotBase || !request.drawContext) {
            LOG_WARN("TerrainDecalRenderer: falling through before draw because request is incomplete "
                     "(addresses={}, terrain={}, slotBase={}, drawContext={})",
                     request.addresses != nullptr,
                     request.terrain != nullptr,
                     request.overlaySlotBase != nullptr,
                     request.drawContext != nullptr);
            return DrawResult::FallThroughToVanilla;
        }

        const OverlaySlotView slot = ReadOverlaySlotView(request.overlaySlotBase);
        if (slot.state != -1 || !slot.matrix) {
            LOG_WARN("TerrainDecalRenderer: falling through because slot state/matrix is invalid "
                     "(state={}, matrix={})",
                     slot.state,
                     static_cast<const void*>(slot.matrix));
            return DrawResult::FallThroughToVanilla;
        }

        const bool clipU = ShouldClipU(slot.flags);
        const bool clipV = ShouldClipV(slot.flags);
        uint32_t overlayId = 0;
        const bool hasOverlayId = TryResolveOverlayId(request, overlayId);
        if (!MatrixHasFiniteComponents(slot.matrix)) {
            if (ShouldLogOverlayOnce(overlayId, "nonfinite-matrix")) {
                LogNonFiniteMatrixSample(overlayId, slot.matrix);
            }
            return DrawResult::FallThroughToVanilla;
        }
        TerrainDecalOverlayOverrides overrides{};
        TerrainDecalUvWindow storedUvWindow{};
        bool hasUvOverride = false;
        bool hasManagedOverrides = false;
        if (hasOverlayId) {
            if (TryGetOverlayUvWindow(overlayId, storedUvWindow)) {
                overrides.hasUvWindow = true;
                overrides.uvWindow = storedUvWindow;
                hasUvOverride = true;
            }
            if (overlayOverridesResolver_ &&
                overlayOverridesResolver_(request.overlayManager,
                                           overlayId,
                                           overrides,
                                           overlayOverridesResolverUserData_)) {
                hasManagedOverrides = true;
                hasUvOverride = hasUvOverride || overrides.hasUvWindow;
            }
        }
        const bool isManagedOverlay = hasManagedOverrides || hasUvOverride;
        if (shadowRecovery && !isManagedOverlay) {
            return DrawResult::Handled;
        }

        const TerrainDecalUvWindow& uvRect = overrides.uvWindow;
        const bool hasModifiers = HasDecalModifiers(overrides);
        const bool forceCustomDraw = shadowRecovery && isManagedOverlay;
        const bool clipOnlyUvOverride = hasUvOverride && uvRect.mode == TerrainDecalUvMode::ClipSubrect;
        const bool effectiveClipU = clipU || clipOnlyUvOverride;
        const bool effectiveClipV = clipV || clipOnlyUvOverride;
        const ClipBounds clipBounds = clipOnlyUvOverride
                                          ? ClipBounds{.minU = uvRect.u1, .maxU = uvRect.u2, .minV = uvRect.v1, .maxV = uvRect.v2}
                                          : ClipBounds{};
        if (debugOverridesActive) {
            LOG_TRACE("TerrainDecalRenderer: draw slotBase={} resolvedOverlayId={} overlayId={} hasUvOverride={} flags=0x{:08X} clipU={} clipV={} effectiveClipU={} effectiveClipV={}",
                     static_cast<const void*>(request.overlaySlotBase),
                     hasOverlayId,
                     overlayId,
                     hasUvOverride,
                     slot.flags,
                     clipU,
                     clipV,
                     effectiveClipU,
                     effectiveClipV);
        }
        if (!forceCustomDraw && !effectiveClipU && !effectiveClipV && !hasUvOverride && !hasModifiers) {
            return DrawResult::FallThroughToVanilla;
        }

        const float* const baseTexTransform = request.activeTexTransform ? request.activeTexTransform : slot.matrix;
        const bool applyUvWindowToTransform = hasUvOverride && uvRect.mode == TerrainDecalUvMode::StretchSubrect;
        const TextureTransformOverride texTransformOverride =
            (applyUvWindowToTransform || hasModifiers)
                ? BuildTextureTransformOverride(baseTexTransform,
                                                uvRect,
                                                applyUvWindowToTransform,
                                                overrides.aspectMultiplier,
                                                overrides.uvScaleU,
                                                overrides.uvScaleV,
                                                overrides.uvOffset,
                                                overrides.mirror)
                : TextureTransformOverride{};
        if (applyUvWindowToTransform && !texTransformOverride.active) {
            LOG_WARN("TerrainDecalRenderer: UV override exists for overlay {} but transform override could not be built",
                     overlayId);
            return DrawResult::FallThroughToVanilla;
        }
        if (hasUvOverride) {
            LOG_TRACE("TerrainDecalRenderer: overlay {} UV override mode={} active [{:.3f}, {:.3f}] to [{:.3f}, {:.3f}] using tex stage {}",
                     overlayId,
                     DescribeOverlayUvMode(uvRect.mode),
                     uvRect.u1,
                     uvRect.v1,
                     uvRect.u2,
                     uvRect.v2,
                     request.activeTexTransformStage);
        }
        if ((applyUvWindowToTransform || hasModifiers) &&
            request.activeTexTransformStage < 0) {
            LOG_WARN("TerrainDecalRenderer: overlay {} transform override requested but no active texture transform stage was captured",
                     overlayId);
            return DrawResult::FallThroughToVanilla;
        }

        const TerrainDrawRect sourceRect = MakeExclusiveTerrainDrawRect(slot.rect);
        if (sourceRect.xStart >= sourceRect.xEnd || sourceRect.zStart >= sourceRect.zEnd) {
            if (hasUvOverride || debugOverridesActive) {
                LOG_DEBUG("TerrainDecalRenderer: overlay {} handled as empty rect [{},{}]-[{},{}]",
                         overlayId,
                         sourceRect.xStart,
                         sourceRect.zStart,
                         sourceRect.xEnd,
                         sourceRect.zEnd);
            }
            return DrawResult::Handled;
        }

        const TerrainGridDimensions dimensions = ReadTerrainGridDimensions(*request.addresses);
        if (dimensions.cellCountX <= 0 || dimensions.cellCountZ <= 0) {
            if (hasUvOverride || debugOverridesActive) {
                LOG_WARN("TerrainDecalRenderer: overlay {} handled with invalid terrain dimensions {}x{}",
                         overlayId,
                         dimensions.cellCountX,
                         dimensions.cellCountZ);
            }
            return DrawResult::Handled;
        }

        const TerrainDrawRect drawRect = ClampTerrainDrawRect(sourceRect, dimensions);
        if (drawRect.xStart >= drawRect.xEnd || drawRect.zStart >= drawRect.zEnd) {
            if (hasUvOverride || debugOverridesActive) {
                LOG_DEBUG("TerrainDecalRenderer: overlay {} handled with empty clamped rect [{},{}]-[{},{}]",
                         overlayId,
                         drawRect.xStart,
                         drawRect.zStart,
                         drawRect.xEnd,
                         drawRect.zEnd);
            }
            return DrawResult::Handled;
        }

        thread_local std::vector<PackedTerrainVertex> outputVertices;
        outputVertices.clear();
        bool loadedAnyTerrainCells = false;
        ClipDebugSample clipDebugSample{};
        const int cellCount = std::max(0, drawRect.xEnd - drawRect.xStart) *
                              std::max(0, drawRect.zEnd - drawRect.zStart);
        outputVertices.reserve(static_cast<size_t>(cellCount) * 12);

        for (int cellZ = drawRect.zStart; cellZ < drawRect.zEnd; ++cellZ) {
            for (int cellX = drawRect.xStart; cellX < drawRect.xEnd; ++cellX) {
                std::array<ClipVertex, 4> vertices{};
                std::array<PackedTerrainVertex, 4> sourceVertices{};
                if (!LoadTerrainCellVertices(*request.addresses, cellX, cellZ, sourceVertices)) {
                    continue;
                }

                loadedAnyTerrainCells = true;

                for (size_t i = 0; i < sourceVertices.size(); ++i) {
                    vertices[i].vertex = sourceVertices[i];
                }

                if (!clipDebugSample.captured) {
                    clipDebugSample.captured = true;
                    clipDebugSample.cellX = cellX;
                    clipDebugSample.cellZ = cellZ;
                    clipDebugSample.sourceVertices = sourceVertices;
                    clipDebugSample.slotVertices = vertices;
                    for (auto& vertex : clipDebugSample.slotVertices) {
                        EvaluateFootprintUv(slot.matrix, vertex);
                    }
                    clipDebugSample.slotMayIntersect =
                        QuadMayIntersectClipBox(clipDebugSample.slotVertices, effectiveClipU, effectiveClipV, clipBounds);
                    clipDebugSample.slotAllInside =
                        AllVerticesInside(clipDebugSample.slotVertices, effectiveClipU, effectiveClipV, clipBounds);

                    if (request.activeTexTransform) {
                        clipDebugSample.activeTransformUsed = true;
                        clipDebugSample.activeVertices = vertices;
                        for (auto& vertex : clipDebugSample.activeVertices) {
                            EvaluateFootprintUv(request.activeTexTransform, vertex);
                        }
                        clipDebugSample.activeMayIntersect =
                            QuadMayIntersectClipBox(clipDebugSample.activeVertices, effectiveClipU, effectiveClipV, clipBounds);
                        clipDebugSample.activeAllInside =
                            AllVerticesInside(clipDebugSample.activeVertices, effectiveClipU, effectiveClipV, clipBounds);
                    }
                }

                for (auto& vertex : vertices) {
                    EvaluateFootprintUv(slot.matrix, vertex);
                }

                if (!AllVerticesHaveFiniteClipUv(vertices)) {
                    if (ShouldLogOverlayOnce(overlayId, "clip-nan")) {
                        LogClipNanSample(overlayId, clipDebugSample, slot.matrix);
                    }
                    continue;
                }

                if (!QuadMayIntersectClipBox(vertices, effectiveClipU, effectiveClipV, clipBounds)) {
                    continue;
                }

                if (AllVerticesInside(vertices, effectiveClipU, effectiveClipV, clipBounds)) {
                    outputVertices.push_back(vertices[0].vertex);
                    outputVertices.push_back(vertices[1].vertex);
                    outputVertices.push_back(vertices[2].vertex);
                    outputVertices.push_back(vertices[0].vertex);
                    outputVertices.push_back(vertices[2].vertex);
                    outputVertices.push_back(vertices[3].vertex);
                }
                else {
                    ClipAndEmitPolygon({vertices[0], vertices[1], vertices[2], vertices[3]},
                                       effectiveClipU,
                                       effectiveClipV,
                                       clipBounds,
                                       outputVertices);
                }
            }
        }

        if (outputVertices.empty()) {
            if (!loadedAnyTerrainCells) {
                if (ShouldLogOverlayOnce(overlayId, "no-terrain-cells")) {
                    LOG_TRACE("TerrainDecalRenderer: overlay {} fell through because no terrain cells loaded", overlayId);
                }
                return DrawResult::FallThroughToVanilla;
            }

            if (!forceCustomDraw && !hasUvOverride && !hasModifiers) {
                if (ShouldLogOverlayOnce(overlayId, "clip-empty")) {
                    LOG_TRACE("TerrainDecalRenderer: overlay {} fell through because clipping produced no output vertices",
                             overlayId);
                    LogClipDebugSample(overlayId, clipDebugSample, clipBounds, effectiveClipU, effectiveClipV);
                }
                return DrawResult::FallThroughToVanilla;
            }

            if (hasUvOverride || debugOverridesActive || forceCustomDraw) {
                LOG_DEBUG("TerrainDecalRenderer: overlay {} handled but produced no output vertices", overlayId);
            }
            return DrawResult::Handled;
        }

        if (texTransformOverride.active) {
            const auto setTexTransform = reinterpret_cast<SetTexTransform4Fn>(request.addresses->setTexTransform4);
            setTexTransform(request.drawContext, texTransformOverride.adjusted.data(), request.activeTexTransformStage);
        }

        const int effectiveDepthOffset = shadowRecovery
                                             ? options_.shadowRecoveryDepthOffset
                                             : (overrides.depthOffset >= 0
                                                    ? overrides.depthOffset
                                                    : options_.customDefaultDepthOffset);

        if (request.addresses->setDepthOffset) {
            const auto setDepthOffset = reinterpret_cast<SetDepthOffsetFn>(request.addresses->setDepthOffset);
            setDepthOffset(request.drawContext, effectiveDepthOffset);
        }

        float originalOpacity = 1.0f;
        bool opacityScaled = false;
        if (request.mode == DrawMode::ShadowRecovery && request.overlaySlotBase) {
            originalOpacity = ReadOverlaySlotOpacity(request.overlaySlotBase);
            if (std::isfinite(originalOpacity)) {
                const float opacityScale = std::clamp(options_.shadowRecoveryOpacityScale, 0.0f, 1.0f);
                SetOverlayTransparency(request.drawContext, *request.addresses, originalOpacity * opacityScale);
                opacityScaled = true;
            }
        }

        const auto drawPrims = reinterpret_cast<DrawPrimsFn>(request.addresses->drawPrims);
        drawPrims(request.drawContext,
                  kPrimTypeTriangleList,
                  kTerrainVertexFormat,
                  static_cast<uint32_t>(outputVertices.size()),
                  outputVertices.data());
        if (hasUvOverride || debugOverridesActive) {
            LOG_TRACE("TerrainDecalRenderer: overlay {} submitted {} vertices", overlayId, outputVertices.size());
        }

        if (opacityScaled) {
            SetOverlayTransparency(request.drawContext, *request.addresses, originalOpacity);
        }

        if (request.addresses->setDepthOffset) {
            const auto setDepthOffset = reinterpret_cast<SetDepthOffsetFn>(request.addresses->setDepthOffset);
            setDepthOffset(request.drawContext, kVanillaDecalDepthOffset);
        }

        if (texTransformOverride.active) {
            const auto setTexTransform = reinterpret_cast<SetTexTransform4Fn>(request.addresses->setTexTransform4);
            setTexTransform(request.drawContext, baseTexTransform, request.activeTexTransformStage);
        }

        return DrawResult::Handled;
    }
}
