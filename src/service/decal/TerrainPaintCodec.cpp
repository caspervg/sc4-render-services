#include "TerrainPaintCodec.h"

#include "cIGZIStream.h"
#include "cIGZOStream.h"
#include "cIGZPersistDBSegment.h"

namespace TerrainPaintCodec
{
    namespace
    {
        constexpr uint32_t kMaxPaints = 1u << 20;
        constexpr uint32_t kMaxPaintBytes = 16u * 1024u * 1024u;

        // Bits of the persisted state word.
        constexpr uint32_t kStateEnabled = 1u << 0;

        // Reads fields of one paint and counts the bytes, so the rest can be skipped.
        class PaintReader
        {
        public:
            explicit PaintReader(cIGZIStream& in) : in_(in) {}

            bool U32(uint32_t& value) { return Count_(in_.GetUint32(value), 4); }
            bool S32(int32_t& value) { return Count_(in_.GetSint32(value), 4); }
            bool F32(float& value) { return Count_(in_.GetFloat32(value), 4); }
            [[nodiscard]] uint32_t Consumed() const noexcept { return consumed_; }

        private:
            bool Count_(const bool ok, const uint32_t bytes)
            {
                consumed_ += bytes;
                return ok;
            }

            cIGZIStream& in_;
            uint32_t consumed_ = 0;
        };

        [[nodiscard]] uint32_t PaintByteCount(const PaintSnapshot& paint) noexcept
        {
            constexpr uint32_t kFixed = 4                // id
                                      + 4                // texture count
                                      + 4                // seed
                                      + 4 * 4            // texture size, rotation, offset x, z
                                      + 5 * 4            // colour, opacity, feather
                                      + 4 + 4            // draw order, depth offset
                                      + 4                // state
                                      + 4                // ring count
                                      + 4                // UV map vertex count
                                      + 3 * 4;           // wear, patch size, seed
            return kFixed + paint.style.textureCount * 16u +
                   static_cast<uint32_t>(paint.ringSizes.size()) * 4u +
                   static_cast<uint32_t>(paint.points.size()) * 8u +
                   static_cast<uint32_t>(paint.uvMap.size()) * 16u;
        }

        bool ReadPaint(cIGZIStream& in, const uint32_t byteCount, PaintSnapshot& paint, std::string& error)
        {
            PaintReader r(in);
            TerrainPaintStyle& style = paint.style;
            uint32_t textureCount = 0;
            if (!r.U32(paint.id.value) || !r.U32(textureCount) || textureCount > kTerrainPaintMaxTextures) {
                error = "bad terrain paint header";
                return false;
            }
            style.textureCount = textureCount;
            for (uint32_t i = 0; i < textureCount; ++i) {
                TerrainPaintTexture& texture = style.textures[i];
                if (!r.U32(texture.key.type) || !r.U32(texture.key.group) || !r.U32(texture.key.instance) ||
                    !r.F32(texture.weight)) {
                    error = "truncated terrain paint texture";
                    return false;
                }
            }

            uint32_t state = 0;
            uint32_t ringCount = 0;
            if (!r.U32(style.variationSeed) ||
                !r.F32(style.textureSizeMeters) || !r.F32(style.textureRotationTurns) ||
                !r.F32(style.textureOffsetX) || !r.F32(style.textureOffsetZ) ||
                !r.F32(style.color.fX) || !r.F32(style.color.fY) || !r.F32(style.color.fZ) ||
                !r.F32(style.opacity) || !r.F32(style.featherMeters) ||
                !r.S32(style.drawOrder) || !r.S32(style.depthOffset) ||
                !r.U32(state) || !r.U32(ringCount)) {
                error = "truncated terrain paint style";
                return false;
            }
            style.enabled = (state & kStateEnabled) != 0;
            style.flags = 0;

            // Each ring size and point takes bytes, so the byte count bounds them.
            if (ringCount > byteCount / 4u) {
                error = "bad terrain paint ring count";
                return false;
            }
            paint.ringSizes.resize(ringCount);
            uint64_t pointCount = 0;
            for (uint32_t& size : paint.ringSizes) {
                if (!r.U32(size)) {
                    error = "truncated terrain paint rings";
                    return false;
                }
                pointCount += size;
            }
            if (pointCount > byteCount / 8u) {
                error = "bad terrain paint point count";
                return false;
            }
            paint.points.resize(static_cast<size_t>(pointCount));
            for (TerrainPaintPoint& point : paint.points) {
                if (!r.F32(point.x) || !r.F32(point.z)) {
                    error = "truncated terrain paint points";
                    return false;
                }
            }

            // Minor version 1 and later: the UV map.
            if (r.Consumed() + 4u <= byteCount) {
                uint32_t uvCount = 0;
                if (!r.U32(uvCount) || uvCount % 3u != 0 || uvCount > (byteCount - r.Consumed()) / 16u) {
                    error = "bad terrain paint UV map";
                    return false;
                }
                paint.uvMap.resize(uvCount);
                for (TerrainPaintUvVertex& vertex : paint.uvMap) {
                    if (!r.F32(vertex.x) || !r.F32(vertex.z) || !r.F32(vertex.u) || !r.F32(vertex.v)) {
                        error = "truncated terrain paint UV map";
                        return false;
                    }
                }
            }

            // Minor version 2 and later: wear.
            if (r.Consumed() + 12u <= byteCount &&
                (!r.F32(style.wear) || !r.F32(style.wearPatchMeters) || !r.U32(style.wearSeed))) {
                error = "truncated terrain paint wear";
                return false;
            }

            if (r.Consumed() > byteCount) {
                error = "terrain paint longer than its byte count";
                return false;
            }
            if (r.Consumed() < byteCount && !in.Skip(byteCount - r.Consumed())) {
                error = "truncated terrain paint";
                return false;
            }
            return true;
        }
    }

    ReadResult Read(cIGZIStream& in)
    {
        ReadResult result{};
        uint32_t magic = 0;
        uint16_t major = 0;
        uint16_t minor = 0;
        uint32_t count = 0;
        if (!in.GetUint32(magic) || !in.GetUint16(major) || !in.GetUint16(minor) || !in.GetUint32(count)) {
            result.error = "truncated terrain paint record header";
            return result;
        }
        if (magic != kMagic) {
            result.error = "invalid terrain paint record magic";
            return result;
        }
        if (major != kVersionMajor) {
            result.error = "unsupported terrain paint record major version";
            return result;
        }
        if (count > kMaxPaints) {
            result.error = "too many terrain paints";
            return result;
        }

        result.paints.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t byteCount = 0;
            if (!in.GetUint32(byteCount) || byteCount > kMaxPaintBytes) {
                result.error = "bad terrain paint byte count";
                result.paints.clear();
                return result;
            }
            PaintSnapshot paint{};
            if (!ReadPaint(in, byteCount, paint, result.error)) {
                result.paints.clear();
                return result;
            }
            result.paints.push_back(std::move(paint));
        }

        result.ok = in.GetError() == 0;
        if (!result.ok) {
            result.error = "stream read error";
            result.paints.clear();
        }
        return result;
    }

    bool Write(cIGZOStream& out, const std::vector<const PaintSnapshot*>& paints)
    {
        if (!out.SetUint32(kMagic) || !out.SetUint16(kVersionMajor) || !out.SetUint16(kVersionMinor) ||
            !out.SetUint32(static_cast<uint32_t>(paints.size()))) {
            return false;
        }

        for (const PaintSnapshot* const paint : paints) {
            const TerrainPaintStyle& style = paint->style;
            bool ok = out.SetUint32(PaintByteCount(*paint)) &&
                      out.SetUint32(paint->id.value) &&
                      out.SetUint32(style.textureCount);
            for (uint32_t i = 0; ok && i < style.textureCount; ++i) {
                const TerrainPaintTexture& texture = style.textures[i];
                ok = out.SetUint32(texture.key.type) && out.SetUint32(texture.key.group) &&
                     out.SetUint32(texture.key.instance) && out.SetFloat32(texture.weight);
            }
            ok = ok &&
                 out.SetUint32(style.variationSeed) &&
                 out.SetFloat32(style.textureSizeMeters) && out.SetFloat32(style.textureRotationTurns) &&
                 out.SetFloat32(style.textureOffsetX) && out.SetFloat32(style.textureOffsetZ) &&
                 out.SetFloat32(style.color.fX) && out.SetFloat32(style.color.fY) && out.SetFloat32(style.color.fZ) &&
                 out.SetFloat32(style.opacity) && out.SetFloat32(style.featherMeters) &&
                 out.SetSint32(style.drawOrder) && out.SetSint32(style.depthOffset) &&
                 out.SetUint32(style.enabled ? kStateEnabled : 0u) &&
                 out.SetUint32(static_cast<uint32_t>(paint->ringSizes.size()));
            for (size_t i = 0; ok && i < paint->ringSizes.size(); ++i) {
                ok = out.SetUint32(paint->ringSizes[i]);
            }
            for (size_t i = 0; ok && i < paint->points.size(); ++i) {
                ok = out.SetFloat32(paint->points[i].x) && out.SetFloat32(paint->points[i].z);
            }
            ok = ok && out.SetUint32(static_cast<uint32_t>(paint->uvMap.size()));
            for (size_t i = 0; ok && i < paint->uvMap.size(); ++i) {
                const TerrainPaintUvVertex& vertex = paint->uvMap[i];
                ok = out.SetFloat32(vertex.x) && out.SetFloat32(vertex.z) && out.SetFloat32(vertex.u) &&
                     out.SetFloat32(vertex.v);
            }
            ok = ok && out.SetFloat32(style.wear) && out.SetFloat32(style.wearPatchMeters) && out.SetUint32(style.wearSeed);
            if (!ok) {
                return false;
            }
        }

        return out.GetError() == 0;
    }

    bool DeleteRecord(cIGZPersistDBSegment* const dbSegment)
    {
        return dbSegment ? dbSegment->DeleteRecord(kRecordKey) : false;
    }
}
