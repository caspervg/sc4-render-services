#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cGZPersistResourceKey.h"
#include "public/cIGZTerrainPaintService.h"
#include "TerrainDecalSidecarCodec.h"

class cIGZIStream;
class cIGZOStream;
class cIGZPersistDBSegment;

// Terrain paints in the city save. A record of its own next to the decal record, so a service
// that does not know paints still reads its decals.
//
// Layout (little endian): magic 'TPNT', u16 major, u16 minor, u32 paint count, then per paint a
// u32 byte count followed by that many bytes. A reader skips what it does not know at the end of
// a paint, so newer minor versions may append fields.
namespace TerrainPaintCodec
{
    constexpr uint32_t kMagic = TerrainDecalSidecar::FourCC('T', 'P', 'N', 'T');
    constexpr uint16_t kVersionMajor = 1;
    // 1: the UV map, after the points. 2: wear, after the UV map. 3: the layer, after wear.
    constexpr uint16_t kVersionMinor = 3;

    constexpr cGZPersistResourceKey kRecordKey(TerrainDecalSidecar::kSidecarType,
                                               TerrainDecalSidecar::kSidecarGroup,
                                               0x00000002u);

    struct PaintSnapshot
    {
        TerrainPaintId id{};
        TerrainPaintStyle style{};
        std::vector<TerrainPaintPoint> points{};
        std::vector<uint32_t> ringSizes{};
        // Three per triangle; empty when the textures lie flat.
        std::vector<TerrainPaintUvVertex> uvMap{};
    };

    struct ReadResult
    {
        bool ok = false;
        std::string error{};
        std::vector<PaintSnapshot> paints{};
    };

    [[nodiscard]] ReadResult Read(cIGZIStream& in);
    bool Write(cIGZOStream& out, const std::vector<const PaintSnapshot*>& paints);
    bool DeleteRecord(cIGZPersistDBSegment* dbSegment);
}
