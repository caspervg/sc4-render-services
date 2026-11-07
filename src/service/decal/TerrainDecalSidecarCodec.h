#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cGZPersistResourceKey.h"
#include "cIGZPersistDBSegment.h"
#include "public/cIGZTerrainDecalService.h"

class cIGZIStream;
class cIGZOStream;

namespace TerrainDecalSidecar
{
    constexpr uint32_t FourCC(const char a, const char b, const char c, const char d) noexcept
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(a))
             | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
             | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
    }

    constexpr uint32_t kMagic = FourCC('T', 'D', 'C', 'S');
    constexpr uint16_t kVersionMajor = 1;
    constexpr uint16_t kVersionMinor = 2;
    constexpr uint32_t kChunkTagTerrainDecals = FourCC('T', 'D', 'E', 'C');

    constexpr uint32_t kSidecarType = 0xE5C2B9A8u;
    constexpr uint32_t kSidecarGroup = FourCC('T', 'D', 'C', 'S');
    constexpr uint32_t kSidecarInstance = 0x00000001u;

    constexpr cGZPersistResourceKey kSidecarKey(kSidecarType, kSidecarGroup, kSidecarInstance);

    struct TerrainDecalSidecarHeader {
        uint32_t magic = kMagic;
        uint16_t versionMajor = kVersionMajor;
        uint16_t versionMinor = kVersionMinor;
        uint32_t flags = 0;
        uint32_t chunkCount = 1;
    };

    struct TerrainDecalChunkHeader {
        uint32_t tag = kChunkTagTerrainDecals;
        uint32_t payloadBytes = 0;
        uint32_t recordSize = 0;
        uint32_t recordCount = 0;
    };

    struct PersistedTerrainDecal {
        uint32_t decalId = 0;
        uint32_t textureType = 0;
        uint32_t textureGroup = 0;
        uint32_t textureInstance = 0;
        uint32_t overlayType = 0;
        float centerX = 0.0f;
        float centerZ = 0.0f;
        float baseSize = 16.0f;
        float rotationTurns = 0.0f;
        float aspectMultiplier = 1.0f;
        float uvScaleU = 1.0f;
        float uvScaleV = 1.0f;
        float uvOffset = 0.0f;
        float unknown8 = 0.0f;
        float opacity = 1.0f;
        uint32_t stateFlags = 0;
        float colorX = 1.0f;
        float colorY = 1.0f;
        float colorZ = 1.0f;
        uint32_t overlayFlags = 0;
        uint32_t overlayDrawMode = 0;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float u2 = 1.0f;
        float v2 = 1.0f;
        uint32_t uvMode = 0;
        int32_t depthOffset = -1;
        int32_t drawOrder = 0;
    };

    static_assert(sizeof(PersistedTerrainDecal) == 112, "Unexpected persisted terrain decal size.");

    enum PersistedTerrainDecalFlags : uint32_t {
        kEnabled = 1u << 0,
        kHasUvWindow = 1u << 1,
    };

    struct ReadResult {
        bool ok = false;
        std::string error{};
        std::vector<TerrainDecalSnapshot> decals{};
    };

    [[nodiscard]] ReadResult Read(cIGZIStream& in);
    bool Write(cIGZOStream& out, const std::vector<TerrainDecalSnapshot>& snapshots);
    bool DeleteRecord(cIGZPersistDBSegment* dbSegment);
}
