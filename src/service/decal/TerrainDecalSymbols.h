#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace TerrainDecal
{
    struct HookAddresses
    {
        uint16_t gameVersion = 0;
        // cSTEOverlayManager::DrawOverlays(SC4DrawContext*), thiscall: one manager's decals and
        // shadows, as the renderer's view object pass draws them.
        uintptr_t drawOverlays = 0;
        uintptr_t drawDecals = 0;
        uintptr_t drawDecalsCallSite = 0;
        uintptr_t drawRect = 0;
        uintptr_t drawRectCallSite = 0;
        uintptr_t drawShadowsCallSite = 0;
        uintptr_t drawShadowsRoughCallSite = 0;
        uintptr_t setTexTransform4 = 0;
        uintptr_t setTexTransform4CallSite = 0;
        uintptr_t clearTexTransform = 0;
        uintptr_t setTransparency = 0;
        uintptr_t setTexWrapModes = 0;
        uintptr_t setDepthOffset = 0;
        uintptr_t setVertexBuffer = 0;
        uintptr_t drawPrims = 0;
        uintptr_t drawPrimsIndexed = 0;
        uintptr_t drawPrimsIndexedRaw = 0;
        uintptr_t drawTerrainMeshSubsetInDrawFrustum = 0;
        uintptr_t drawTerrainMeshSubsetWithVertBufExtensionInDrawFrustum = 0;
        uintptr_t terrainGridVerticesPtr = 0;
        uintptr_t terrainCellInfoRowsPtr = 0;
        uintptr_t levelCellQuadsPtr = 0;
        uintptr_t allLevelCellIndicesPtr = 0;
        uintptr_t terrainCellCountXPtr = 0;
        uintptr_t terrainCellCountZPtr = 0;
        uintptr_t terrainVertexCountXPtr = 0;
        uintptr_t terrainVertexCountZPtr = 0;
        uintptr_t terrainVertexCountPtr = 0;
        // What the terrain view puts on screen this frame, by vertex row z: whether the row is in
        // the draw frustum, and the vertex columns [start, end) drawn in it (cells start to end - 1).
        uintptr_t drawFrustumRowFlagsPtr = 0;
        uintptr_t drawFrustumColumnStartPtr = 0;
        uintptr_t drawFrustumColumnEndPtr = 0;
        // The overlay manager's terrain view, and the view's drawn vertex rows [start, end).
        std::ptrdiff_t overlayTerrainViewOffset = 0;
        std::ptrdiff_t terrainViewDrawRowStartOffset = 0;
        std::ptrdiff_t terrainViewDrawRowEndOffset = 0;
        // The view's terrain, and the terrain's per-vertex records.
        std::ptrdiff_t terrainViewTerrainOffset = 0;
        std::ptrdiff_t terrainVertexRecordsOffset = 0;
        // Overlay manager flags that make DrawRect draw the whole rect, not just the frustum.
        std::ptrdiff_t overlayWholeRectFlagOffsetA = 0;
        std::ptrdiff_t overlayWholeRectFlagOffsetB = 0;
        std::ptrdiff_t overlayRectOffset = 0;
        std::ptrdiff_t overlaySlotsPtrOffset = 0;
        std::ptrdiff_t overlaySlotStride = 0;
    };

    [[nodiscard]] std::optional<HookAddresses> ResolveHookAddresses(uint16_t gameVersion);
    [[nodiscard]] std::string_view DescribeKnownAddressSet(uint16_t gameVersion) noexcept;
}
