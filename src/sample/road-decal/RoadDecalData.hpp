#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RoadDecalPoint
{
    float x;
    float y;
    float z;
    bool hardCorner = false;
};

enum class RoadMarkupType : uint32_t
{
    SolidWhiteLine,
    DashedWhiteLine,
    SolidYellowLine,
    DashedYellowLine,
    DoubleSolidYellow,
    SolidWhiteEdgeLine,

    ArrowStraight,
    ArrowLeft,
    ArrowRight,
    ArrowLeftRight,
    ArrowStraightLeft,
    ArrowStraightRight,
    ArrowUTurn,

    ZebraCrosswalk,
    LadderCrosswalk,
    ContinentalCrosswalk,
    StopBar,

    YieldTriangle,
    ParkingSpace,
    BikeSymbol,
    BusLane,

    TextStop,
    TextSlow,
    TextSchool,
    TextBusOnly
};

enum class RoadMarkupCategory : uint32_t
{
    LaneDivider,
    DirectionalArrow,
    Crossing,
    ZoneMarking,
    TextLabel
};

enum class PlacementMode : uint32_t
{
    Freehand,
    TwoPoint,
    SingleClick,
    Rectangle,
    Snapping
};

struct RoadMarkupProperties
{
    RoadMarkupType type;
    RoadMarkupCategory category;
    const char* displayName;
    const char* description;
    float defaultWidth;
    float defaultLength;
    uint32_t defaultColor;
    bool supportsDashing;
    bool requiresStraightSection;
};

struct RoadMarkupStroke
{
    RoadMarkupType type = RoadMarkupType::SolidWhiteLine;
    std::vector<RoadDecalPoint> points;
    float width = 0.15f;
    float length = 3.0f;
    float rotation = 0.0f;
    bool dashed = false;
    float dashLength = 3.0f;
    float gapLength = 9.0f;
    uint32_t color = 0;
    float opacity = 1.0f;
    bool visible = true;
    uint32_t layerId = 0;
};

struct RoadMarkupLayer
{
    uint32_t id = 0;
    std::string name;
    std::vector<RoadMarkupStroke> strokes;
    bool visible = true;
    bool locked = false;
    int renderOrder = 0;
};

extern std::vector<RoadMarkupLayer> gRoadMarkupLayers;
extern int gActiveLayerIndex;

const RoadMarkupProperties& GetRoadMarkupProperties(RoadMarkupType type);
RoadMarkupCategory GetMarkupCategory(RoadMarkupType type);
const std::vector<RoadMarkupType>& GetRoadMarkupTypesForCategory(RoadMarkupCategory category);

void EnsureDefaultRoadMarkupLayer();
RoadMarkupLayer* GetActiveRoadMarkupLayer();
bool AddRoadMarkupLayer(const std::string& name);
void DeleteActiveRoadMarkupLayer();
bool AddRoadMarkupStrokeToActiveLayer(const RoadMarkupStroke& stroke);
void UndoLastRoadMarkupStroke();
void ClearAllRoadMarkupStrokes();
size_t GetTotalRoadMarkupStrokeCount();

void RebuildRoadDecalGeometry();
void DrawRoadDecals();

// Shows the currently edited stroke (already-placed click points).
void SetRoadDecalActiveStroke(const RoadMarkupStroke* stroke);

// Shows a preview segment from last placed point to current mouse pick.
void SetRoadDecalPreviewSegment(bool enabled,
                                const RoadMarkupStroke& stroke);

bool SaveMarkupsToFile(const char* filepath);
bool LoadMarkupsFromFile(const char* filepath);
