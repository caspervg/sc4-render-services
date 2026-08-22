#include "TerrainDecalSidecarCodec.h"

#include "cIGZIStream.h"
#include "cIGZOStream.h"

namespace {
    constexpr uint32_t kLegacyRecordSize = sizeof(TerrainDecalSidecar::PersistedTerrainDecal) - sizeof(int32_t);
    constexpr uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;

    TerrainDecalSnapshot DecodeSnapshot(const TerrainDecalSidecar::PersistedTerrainDecal& persisted) {
        TerrainDecalSnapshot snapshot{};
        snapshot.id = TerrainDecalId{persisted.decalId};
        snapshot.state.textureKey = cGZPersistResourceKey(
            persisted.textureType,
            persisted.textureGroup,
            persisted.textureInstance);
        snapshot.state.overlayType =
            static_cast<cISTETerrainView::tOverlayManagerType>(persisted.overlayType);
        snapshot.state.decalInfo.center = cS3DVector2(persisted.centerX, persisted.centerZ);
        snapshot.state.decalInfo.baseSize = persisted.baseSize;
        snapshot.state.decalInfo.rotationTurns = persisted.rotationTurns;
        snapshot.state.decalInfo.aspectMultiplier = persisted.aspectMultiplier;
        snapshot.state.decalInfo.uvScaleU = persisted.uvScaleU;
        snapshot.state.decalInfo.uvScaleV = persisted.uvScaleV;
        snapshot.state.decalInfo.uvOffset = persisted.uvOffset;
        snapshot.state.decalInfo.unknown8 = persisted.unknown8;
        snapshot.state.opacity = persisted.opacity;
        snapshot.state.enabled = (persisted.stateFlags & TerrainDecalSidecar::kEnabled) != 0;
        snapshot.state.color = cS3DVector3(persisted.colorX, persisted.colorY, persisted.colorZ);
        snapshot.state.drawMode = static_cast<uint8_t>(persisted.overlayDrawMode);
        snapshot.state.flags = persisted.overlayFlags;
        snapshot.state.hasUvWindow = (persisted.stateFlags & TerrainDecalSidecar::kHasUvWindow) != 0;
        snapshot.state.uvWindow = TerrainDecalUvWindow{
            .u1 = persisted.u1,
            .v1 = persisted.v1,
            .u2 = persisted.u2,
            .v2 = persisted.v2,
            .mode = persisted.uvMode == static_cast<uint32_t>(TerrainDecalUvMode::ClipSubrect)
                        ? TerrainDecalUvMode::ClipSubrect
                        : TerrainDecalUvMode::StretchSubrect,
        };
        snapshot.state.depthOffset = persisted.depthOffset;
        return snapshot;
    }

    TerrainDecalSidecar::PersistedTerrainDecal EncodeSnapshot(const TerrainDecalSnapshot& snapshot) {
        TerrainDecalSidecar::PersistedTerrainDecal persisted{};
        persisted.decalId = snapshot.id.value;
        persisted.textureType = snapshot.state.textureKey.type;
        persisted.textureGroup = snapshot.state.textureKey.group;
        persisted.textureInstance = snapshot.state.textureKey.instance;
        persisted.overlayType = static_cast<uint32_t>(snapshot.state.overlayType);
        persisted.centerX = snapshot.state.decalInfo.center.fX;
        persisted.centerZ = snapshot.state.decalInfo.center.fY;
        persisted.baseSize = snapshot.state.decalInfo.baseSize;
        persisted.rotationTurns = snapshot.state.decalInfo.rotationTurns;
        persisted.aspectMultiplier = snapshot.state.decalInfo.aspectMultiplier;
        persisted.uvScaleU = snapshot.state.decalInfo.uvScaleU;
        persisted.uvScaleV = snapshot.state.decalInfo.uvScaleV;
        persisted.uvOffset = snapshot.state.decalInfo.uvOffset;
        persisted.unknown8 = snapshot.state.decalInfo.unknown8;
        persisted.opacity = snapshot.state.opacity;
        if (snapshot.state.enabled) {
            persisted.stateFlags |= TerrainDecalSidecar::kEnabled;
        }
        if (snapshot.state.hasUvWindow) {
            persisted.stateFlags |= TerrainDecalSidecar::kHasUvWindow;
        }
        persisted.colorX = snapshot.state.color.fX;
        persisted.colorY = snapshot.state.color.fY;
        persisted.colorZ = snapshot.state.color.fZ;
        persisted.overlayFlags = snapshot.state.flags;
        persisted.overlayDrawMode = snapshot.state.drawMode;
        persisted.u1 = snapshot.state.uvWindow.u1;
        persisted.v1 = snapshot.state.uvWindow.v1;
        persisted.u2 = snapshot.state.uvWindow.u2;
        persisted.v2 = snapshot.state.uvWindow.v2;
        persisted.uvMode = static_cast<uint32_t>(snapshot.state.uvWindow.mode);
        persisted.depthOffset = snapshot.state.depthOffset;
        return persisted;
    }
}

namespace TerrainDecalSidecar {
    ReadResult Read(cIGZIStream& in) {
        ReadResult result{};

        TerrainDecalSidecarHeader header{};
        if (!in.GetUint32(header.magic) ||
            !in.GetUint16(header.versionMajor) ||
            !in.GetUint16(header.versionMinor) ||
            !in.GetUint32(header.flags) ||
            !in.GetUint32(header.chunkCount)) {
            result.error = "truncated terrain decal sidecar header";
            return result;
        }

        if (header.magic != kMagic) {
            result.error = "invalid terrain decal sidecar magic";
            return result;
        }

        if (header.versionMajor != kVersionMajor) {
            result.error = "unsupported terrain decal sidecar major version";
            return result;
        }

        if (header.chunkCount != 1) {
            result.error = "unexpected terrain decal sidecar chunk count";
            return result;
        }

        TerrainDecalChunkHeader chunk{};
        if (!in.GetUint32(chunk.tag) ||
            !in.GetUint32(chunk.payloadBytes) ||
            !in.GetUint32(chunk.recordSize) ||
            !in.GetUint32(chunk.recordCount)) {
            result.error = "truncated terrain decal chunk header";
            return result;
        }

        if (chunk.tag != kChunkTagTerrainDecals) {
            result.error = "unexpected terrain decal chunk tag";
            return result;
        }

        if (chunk.recordSize < kLegacyRecordSize || chunk.payloadBytes > kMaxPayloadBytes ||
            static_cast<uint64_t>(chunk.recordCount) * chunk.recordSize != chunk.payloadBytes) {
            result.error = "terrain decal chunk payload size mismatch";
            return result;
        }

        const bool hasDepthOffsetField = chunk.recordSize >= sizeof(PersistedTerrainDecal);

        result.decals.reserve(chunk.recordCount);
        for (uint32_t i = 0; i < chunk.recordCount; ++i) {
            PersistedTerrainDecal persisted{};
            if (!in.GetUint32(persisted.decalId) ||
                !in.GetUint32(persisted.textureType) ||
                !in.GetUint32(persisted.textureGroup) ||
                !in.GetUint32(persisted.textureInstance) ||
                !in.GetUint32(persisted.overlayType) ||
                !in.GetFloat32(persisted.centerX) ||
                !in.GetFloat32(persisted.centerZ) ||
                !in.GetFloat32(persisted.baseSize) ||
                !in.GetFloat32(persisted.rotationTurns) ||
                !in.GetFloat32(persisted.aspectMultiplier) ||
                !in.GetFloat32(persisted.uvScaleU) ||
                !in.GetFloat32(persisted.uvScaleV) ||
                !in.GetFloat32(persisted.uvOffset) ||
                !in.GetFloat32(persisted.unknown8) ||
                !in.GetFloat32(persisted.opacity) ||
                !in.GetUint32(persisted.stateFlags) ||
                !in.GetFloat32(persisted.colorX) ||
                !in.GetFloat32(persisted.colorY) ||
                !in.GetFloat32(persisted.colorZ) ||
                !in.GetUint32(persisted.overlayFlags) ||
                !in.GetUint32(persisted.overlayDrawMode) ||
                !in.GetFloat32(persisted.u1) ||
                !in.GetFloat32(persisted.v1) ||
                !in.GetFloat32(persisted.u2) ||
                !in.GetFloat32(persisted.v2) ||
                !in.GetUint32(persisted.uvMode)) {
                result.error = "truncated terrain decal record";
                result.decals.clear();
                return result;
            }

            if (hasDepthOffsetField) {
                uint32_t depthOffsetBits = 0;
                if (!in.GetUint32(depthOffsetBits)) {
                    result.error = "truncated terrain decal record";
                    result.decals.clear();
                    return result;
                }
                persisted.depthOffset = static_cast<int32_t>(depthOffsetBits);
            }
            // Older records have no depthOffset; PersistedTerrainDecal::depthOffset already defaults to -1.

            const uint32_t consumedBytes = hasDepthOffsetField ? sizeof(PersistedTerrainDecal) : kLegacyRecordSize;
            if (chunk.recordSize > consumedBytes && !in.Skip(chunk.recordSize - consumedBytes)) {
                result.error = "truncated terrain decal record";
                result.decals.clear();
                return result;
            }

            result.decals.push_back(DecodeSnapshot(persisted));
        }

        result.ok = in.GetError() == 0;
        if (!result.ok && result.error.empty()) {
            result.error = "stream read error";
        }
        return result;
    }

    bool Write(cIGZOStream& out, const std::vector<TerrainDecalSnapshot>& snapshots) {
        const TerrainDecalSidecarHeader header{};
        const TerrainDecalChunkHeader chunk{
            .tag = kChunkTagTerrainDecals,
            .payloadBytes = static_cast<uint32_t>(snapshots.size() * sizeof(PersistedTerrainDecal)),
            .recordSize = sizeof(PersistedTerrainDecal),
            .recordCount = static_cast<uint32_t>(snapshots.size()),
        };

        if (!out.SetUint32(header.magic) ||
            !out.SetUint16(header.versionMajor) ||
            !out.SetUint16(header.versionMinor) ||
            !out.SetUint32(header.flags) ||
            !out.SetUint32(header.chunkCount) ||
            !out.SetUint32(chunk.tag) ||
            !out.SetUint32(chunk.payloadBytes) ||
            !out.SetUint32(chunk.recordSize) ||
            !out.SetUint32(chunk.recordCount)) {
            return false;
        }

        for (const TerrainDecalSnapshot& snapshot : snapshots) {
            const PersistedTerrainDecal persisted = EncodeSnapshot(snapshot);
            if (!out.SetUint32(persisted.decalId) ||
                !out.SetUint32(persisted.textureType) ||
                !out.SetUint32(persisted.textureGroup) ||
                !out.SetUint32(persisted.textureInstance) ||
                !out.SetUint32(persisted.overlayType) ||
                !out.SetFloat32(persisted.centerX) ||
                !out.SetFloat32(persisted.centerZ) ||
                !out.SetFloat32(persisted.baseSize) ||
                !out.SetFloat32(persisted.rotationTurns) ||
                !out.SetFloat32(persisted.aspectMultiplier) ||
                !out.SetFloat32(persisted.uvScaleU) ||
                !out.SetFloat32(persisted.uvScaleV) ||
                !out.SetFloat32(persisted.uvOffset) ||
                !out.SetFloat32(persisted.unknown8) ||
                !out.SetFloat32(persisted.opacity) ||
                !out.SetUint32(persisted.stateFlags) ||
                !out.SetFloat32(persisted.colorX) ||
                !out.SetFloat32(persisted.colorY) ||
                !out.SetFloat32(persisted.colorZ) ||
                !out.SetUint32(persisted.overlayFlags) ||
                !out.SetUint32(persisted.overlayDrawMode) ||
                !out.SetFloat32(persisted.u1) ||
                !out.SetFloat32(persisted.v1) ||
                !out.SetFloat32(persisted.u2) ||
                !out.SetFloat32(persisted.v2) ||
                !out.SetUint32(persisted.uvMode) ||
                !out.SetUint32(static_cast<uint32_t>(persisted.depthOffset))) {
                return false;
            }
        }

        return out.GetError() == 0;
    }

    bool DeleteRecord(cIGZPersistDBSegment* const dbSegment) {
        return dbSegment ? dbSegment->DeleteRecord(kSidecarKey) : false;
    }
}
