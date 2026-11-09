#include "TerrainDecalSymbols.h"

namespace TerrainDecal
{
    std::optional<HookAddresses> ResolveHookAddresses(const uint16_t gameVersion)
    {
        switch (gameVersion) {
        case 641:
            return HookAddresses{
                .gameVersion = 641,
                .drawDecals = 0x00736790u,
                .drawDecalsCallSite = 0x00739320u,
                .drawRect = 0x00735720u,
                .drawRectCallSite = 0x00736B88u,
                .drawShadowsCallSite = 0x0073935Fu,
                .drawShadowsRoughCallSite = 0x00739373u,
                .setTexTransform4 = 0x007D4340u,
                .setTexTransform4CallSite = 0x00736B7Cu,
                // SC4DrawContext::ClearTexTransform: the stage takes the vertex's first texture
                // coordinates as they are.
                .clearTexTransform = 0x007D4420u,
                .setTransparency = 0x007D2A20u,
                .setTexWrapModes = 0x007D41E0u,
                .setDepthOffset = 0x007D4480u,
                .setVertexBuffer = 0x007D2970u,
                .drawPrims = 0x007D2990u,
                .drawPrimsIndexed = 0x007D29C0u,
                .drawPrimsIndexedRaw = 0x007D29F0u,
                .drawTerrainMeshSubsetInDrawFrustum = 0x007541C0u,
                .drawTerrainMeshSubsetWithVertBufExtensionInDrawFrustum = 0x007545C0u,
                .terrainGridVerticesPtr = 0x00B4C758u,
                .terrainCellInfoRowsPtr = 0x00B4C6ACu,
                .levelCellQuadsPtr = 0x00B4C6B0u,
                .allLevelCellIndicesPtr = 0x00B4C6B8u,
                .terrainCellCountXPtr = 0x00B4C744u,
                .terrainCellCountZPtr = 0x00B4C748u,
                .terrainVertexCountXPtr = 0x00B4C74Cu,
                .terrainVertexCountZPtr = 0x00B4C750u,
                .terrainVertexCountPtr = 0x00B4C754u,
                // sbDrawTerrainInDrawFrustum, sDrawColStart, sDrawColEnd as
                // DrawTerrainMeshSubsetInDrawFrustum (0x007541C0) reads them.
                .drawFrustumRowFlagsPtr = 0x00B4C6D0u,
                .drawFrustumColumnStartPtr = 0x00B4C6D8u,
                .drawFrustumColumnEndPtr = 0x00B4C6D4u,
                .overlayTerrainViewOffset = 0x8C,
                .terrainViewDrawRowStartOffset = 0x98,
                .terrainViewDrawRowEndOffset = 0x9C,
                .terrainViewTerrainOffset = 0x20,
                .terrainVertexRecordsOffset = 0x6C,
                .overlayWholeRectFlagOffsetA = 0x15,
                .overlayWholeRectFlagOffsetB = 0x16,
                .overlayRectOffset = 0x0C,
                .overlaySlotsPtrOffset = 0x98,
                .overlaySlotStride = 0xB4,
            };
        default:
            return std::nullopt;
        }
    }

    std::string_view DescribeKnownAddressSet(const uint16_t gameVersion) noexcept
    {
        switch (gameVersion) {
        case 641:
            return "SimCity 4 1.1.641 Windows x86";
        default:
            return "unknown";
        }
    }
}
