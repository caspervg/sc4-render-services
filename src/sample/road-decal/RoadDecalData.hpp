#pragma once

#include <vector>

struct RoadDecalPoint
{
    float x;
    float y;
    float z;
};

struct RoadDecalStroke
{
    std::vector<RoadDecalPoint> points;
    int styleId; // 0 = white, 1 = yellow, 2 = red
    float width; // world units
};

extern std::vector<RoadDecalStroke> gRoadDecalStrokes;

void RebuildRoadDecalGeometry();
void DrawRoadDecals();

// Shows the currently edited stroke (already-placed click points).
void SetRoadDecalActiveStroke(const RoadDecalStroke* stroke);

// Shows a preview segment from last placed point to current mouse pick.
void SetRoadDecalPreviewSegment(bool enabled,
                                const RoadDecalPoint& from,
                                const RoadDecalPoint& to,
                                int styleId,
                                float width);
