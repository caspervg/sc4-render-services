#define NOMINMAX

#include "RoadDecalData.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d.h>
#include <d3dtypes.h>
#include <ddraw.h>

#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "GZServPtrs.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay{nullptr};
std::vector<RoadMarkupLayer> gRoadMarkupLayers;
int gActiveLayerIndex = 0;

namespace
{
    constexpr float kDecalTerrainOffset = 0.05f;
    constexpr float kTerrainGridSpacing = 16.0f;
    constexpr uint32_t kRoadDecalZBias = 1;
    constexpr float kMinLen = 1.0e-4f;
    constexpr float kDoubleYellowSpacing = 0.10f;
    constexpr uint32_t kMarkupFileMagic = 0x4B4D4452; // RDMK
    constexpr uint32_t kMarkupFileVersion = 1;

    template<typename T>
    bool WriteValue(std::ofstream& out, const T& value);

    template<typename T>
    bool ReadValue(std::ifstream& in, T& value);

    struct RoadDecalStateGuard
    {
        explicit RoadDecalStateGuard(IDirect3DDevice7* dev)
            : device(dev)
        {
            if (!device) {
                return;
            }

            okZEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZENABLE, &zEnable));
            okZWrite = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &zWrite));
            okLighting = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_LIGHTING, &lighting));
            okAlphaBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &alphaBlend));
            okAlphaTest = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, &alphaTest));
            okAlphaFunc = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHAFUNC, &alphaFunc));
            okAlphaRef = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHAREF, &alphaRef));
            okStencilEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_STENCILENABLE, &stencilEnable));
            okSrcBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_SRCBLEND, &srcBlend));
            okDstBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_DESTBLEND, &dstBlend));
            okCullMode = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_CULLMODE, &cullMode));
            okFogEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_FOGENABLE, &fogEnable));
            okRangeFogEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, &rangeFogEnable));
            okZFunc = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZFUNC, &zFunc));
            okZBias = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZBIAS, &zBias));
            okTexture0 = SUCCEEDED(device->GetTexture(0, &texture0));
            okTexture1 = SUCCEEDED(device->GetTexture(1, &texture1));
            okTs0ColorOp = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_COLOROP, &ts0ColorOp));
            okTs0ColorArg1 = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_COLORARG1, &ts0ColorArg1));
            okTs0AlphaOp = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_ALPHAOP, &ts0AlphaOp));
            okTs0AlphaArg1 = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &ts0AlphaArg1));
            okTs1ColorOp = SUCCEEDED(device->GetTextureStageState(1, D3DTSS_COLOROP, &ts1ColorOp));
            okTs1AlphaOp = SUCCEEDED(device->GetTextureStageState(1, D3DTSS_ALPHAOP, &ts1AlphaOp));
        }

        ~RoadDecalStateGuard()
        {
            if (!device) {
                return;
            }

            if (okZEnable) device->SetRenderState(D3DRENDERSTATE_ZENABLE, zEnable);
            if (okZWrite) device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, zWrite);
            if (okLighting) device->SetRenderState(D3DRENDERSTATE_LIGHTING, lighting);
            if (okAlphaBlend) device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, alphaBlend);
            if (okAlphaTest) device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, alphaTest);
            if (okAlphaFunc) device->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, alphaFunc);
            if (okAlphaRef) device->SetRenderState(D3DRENDERSTATE_ALPHAREF, alphaRef);
            if (okStencilEnable) device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, stencilEnable);
            if (okSrcBlend) device->SetRenderState(D3DRENDERSTATE_SRCBLEND, srcBlend);
            if (okDstBlend) device->SetRenderState(D3DRENDERSTATE_DESTBLEND, dstBlend);
            if (okCullMode) device->SetRenderState(D3DRENDERSTATE_CULLMODE, cullMode);
            if (okFogEnable) device->SetRenderState(D3DRENDERSTATE_FOGENABLE, fogEnable);
            if (okRangeFogEnable) device->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, rangeFogEnable);
            if (okZFunc) device->SetRenderState(D3DRENDERSTATE_ZFUNC, zFunc);
            if (okZBias) device->SetRenderState(D3DRENDERSTATE_ZBIAS, zBias);
            if (okTexture0) device->SetTexture(0, texture0);
            if (okTexture1) device->SetTexture(1, texture1);
            if (okTs0ColorOp) device->SetTextureStageState(0, D3DTSS_COLOROP, ts0ColorOp);
            if (okTs0ColorArg1) device->SetTextureStageState(0, D3DTSS_COLORARG1, ts0ColorArg1);
            if (okTs0AlphaOp) device->SetTextureStageState(0, D3DTSS_ALPHAOP, ts0AlphaOp);
            if (okTs0AlphaArg1) device->SetTextureStageState(0, D3DTSS_ALPHAARG1, ts0AlphaArg1);
            if (okTs1ColorOp) device->SetTextureStageState(1, D3DTSS_COLOROP, ts1ColorOp);
            if (okTs1AlphaOp) device->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1AlphaOp);
            if (texture0) texture0->Release();
            if (texture1) texture1->Release();
        }

        IDirect3DDevice7* device = nullptr;
        bool okZEnable = false;
        bool okZWrite = false;
        bool okLighting = false;
        bool okAlphaBlend = false;
        bool okAlphaTest = false;
        bool okAlphaFunc = false;
        bool okAlphaRef = false;
        bool okStencilEnable = false;
        bool okSrcBlend = false;
        bool okDstBlend = false;
        bool okCullMode = false;
        bool okFogEnable = false;
        bool okRangeFogEnable = false;
        bool okZFunc = false;
        bool okZBias = false;
        bool okTexture0 = false;
        bool okTexture1 = false;
        bool okTs0ColorOp = false;
        bool okTs0ColorArg1 = false;
        bool okTs0AlphaOp = false;
        bool okTs0AlphaArg1 = false;
        bool okTs1ColorOp = false;
        bool okTs1AlphaOp = false;
        DWORD zEnable = 0;
        DWORD zWrite = 0;
        DWORD lighting = 0;
        DWORD alphaBlend = 0;
        DWORD alphaTest = 0;
        DWORD alphaFunc = 0;
        DWORD alphaRef = 0;
        DWORD stencilEnable = 0;
        DWORD srcBlend = 0;
        DWORD dstBlend = 0;
        DWORD cullMode = 0;
        DWORD fogEnable = 0;
        DWORD rangeFogEnable = 0;
        DWORD zFunc = 0;
        DWORD zBias = 0;
        DWORD ts0ColorOp = 0;
        DWORD ts0ColorArg1 = 0;
        DWORD ts0AlphaOp = 0;
        DWORD ts0AlphaArg1 = 0;
        DWORD ts1ColorOp = 0;
        DWORD ts1AlphaOp = 0;
        IDirectDrawSurface7* texture0 = nullptr;
        IDirectDrawSurface7* texture1 = nullptr;
    };

    void BuildStrokeVertices(const RoadMarkupStroke& stroke, std::vector<RoadDecalVertex>& outVerts);
    void DrawVertexBuffer(IDirect3DDevice7* device, const std::vector<RoadDecalVertex>& verts);

    struct RoadDecalVertex
    {
        float x;
        float y;
        float z;
        DWORD diffuse;
    };

    std::vector<RoadDecalVertex> gRoadDecalVertices;
    std::vector<RoadDecalVertex> gRoadDecalActiveVertices;
    std::vector<RoadDecalVertex> gRoadDecalPreviewVertices;

    constexpr std::array<RoadMarkupProperties, 25> kMarkupProps = {{
        {RoadMarkupType::SolidWhiteLine, RoadMarkupCategory::LaneDivider, "Solid White", "Continuous white lane divider.", 0.15f, 0.0f, 0xE0FFFFFF, false, false},
        {RoadMarkupType::DashedWhiteLine, RoadMarkupCategory::LaneDivider, "Dashed White", "Dashed white lane divider.", 0.15f, 0.0f, 0xE0FFFFFF, true, false},
        {RoadMarkupType::SolidYellowLine, RoadMarkupCategory::LaneDivider, "Solid Yellow", "Continuous yellow centerline.", 0.15f, 0.0f, 0xE0FFD700, false, false},
        {RoadMarkupType::DashedYellowLine, RoadMarkupCategory::LaneDivider, "Dashed Yellow", "Dashed yellow centerline.", 0.15f, 0.0f, 0xE0FFD700, true, false},
        {RoadMarkupType::DoubleSolidYellow, RoadMarkupCategory::LaneDivider, "Double Yellow", "Double solid yellow centerline.", 0.15f, 0.0f, 0xE0FFD700, false, true},
        {RoadMarkupType::SolidWhiteEdgeLine, RoadMarkupCategory::LaneDivider, "Edge Line", "Solid white edge line.", 0.15f, 0.0f, 0xE0FFFFFF, false, true},

        {RoadMarkupType::ArrowStraight, RoadMarkupCategory::DirectionalArrow, "Straight Arrow", "Straight lane arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ArrowLeft, RoadMarkupCategory::DirectionalArrow, "Left Arrow", "Left turn arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ArrowRight, RoadMarkupCategory::DirectionalArrow, "Right Arrow", "Right turn arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ArrowLeftRight, RoadMarkupCategory::DirectionalArrow, "Left/Right Arrow", "Left-right arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ArrowStraightLeft, RoadMarkupCategory::DirectionalArrow, "Straight+Left", "Straight-left arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ArrowStraightRight, RoadMarkupCategory::DirectionalArrow, "Straight+Right", "Straight-right arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ArrowUTurn, RoadMarkupCategory::DirectionalArrow, "U-Turn Arrow", "U-turn arrow.", 1.20f, 3.00f, 0xE0FFFFFF, false, true},

        {RoadMarkupType::ZebraCrosswalk, RoadMarkupCategory::Crossing, "Zebra Crosswalk", "Parallel zebra stripes.", 0.50f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::LadderCrosswalk, RoadMarkupCategory::Crossing, "Ladder Crosswalk", "Zebra with side rails.", 0.50f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ContinentalCrosswalk, RoadMarkupCategory::Crossing, "Continental", "Thicker stripe crossing.", 0.80f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::StopBar, RoadMarkupCategory::Crossing, "Stop Bar", "Stop line bar.", 0.40f, 0.00f, 0xE0FFFFFF, false, true},

        {RoadMarkupType::YieldTriangle, RoadMarkupCategory::ZoneMarking, "Yield Triangle", "Yield marker.", 0.15f, 1.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::ParkingSpace, RoadMarkupCategory::ZoneMarking, "Parking Space", "Parking outline marker.", 0.15f, 5.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::BikeSymbol, RoadMarkupCategory::ZoneMarking, "Bike Symbol", "Bike lane symbol.", 0.20f, 2.50f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::BusLane, RoadMarkupCategory::ZoneMarking, "Bus Lane", "Bus lane marker.", 0.20f, 6.00f, 0xE0FFFFFF, false, true},

        {RoadMarkupType::TextStop, RoadMarkupCategory::TextLabel, "STOP", "Text marker phase 2.", 0.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::TextSlow, RoadMarkupCategory::TextLabel, "SLOW", "Text marker phase 2.", 0.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::TextSchool, RoadMarkupCategory::TextLabel, "SCHOOL", "Text marker phase 2.", 0.20f, 4.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::TextBusOnly, RoadMarkupCategory::TextLabel, "BUS ONLY", "Text marker phase 2.", 0.20f, 5.00f, 0xE0FFFFFF, false, true},
    }};

    const RoadMarkupProperties& FindProps(RoadMarkupType type)
    {
        for (const auto& props : kMarkupProps) {
            if (props.type == type) {
                return props;
            }
        }
        return kMarkupProps[0];
    }

    DWORD ApplyOpacity(uint32_t color, float opacity)
    {
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        const uint32_t baseA = (color >> 24U) & 0xFFU;
        const uint32_t outA = static_cast<uint32_t>(std::clamp(baseA * opacity, 0.0f, 255.0f));
        return (color & 0x00FFFFFFU) | (outA << 24U);
    }

    cISTETerrain* GetActiveTerrain()
    {
        cISC4AppPtr app;
        cISC4City* city = app ? app->GetCity() : nullptr;
        return city ? city->GetTerrain() : nullptr;
    }

    float SampleTerrainHeight(cISTETerrain* terrain, float x, float z)
    {
        if (!terrain) {
            return 0.0f;
        }

        const float cellX = std::floor(x / kTerrainGridSpacing);
        const float cellZ = std::floor(z / kTerrainGridSpacing);
        const float x0 = cellX * kTerrainGridSpacing;
        const float z0 = cellZ * kTerrainGridSpacing;
        const float x1 = x0 + kTerrainGridSpacing;
        const float z1 = z0 + kTerrainGridSpacing;
        const float tx = std::clamp((x - x0) / kTerrainGridSpacing, 0.0f, 1.0f);
        const float tz = std::clamp((z - z0) / kTerrainGridSpacing, 0.0f, 1.0f);

        const float h00 = terrain->GetAltitudeAtNearestGrid(x0, z0);
        const float h10 = terrain->GetAltitudeAtNearestGrid(x1, z0);
        const float h01 = terrain->GetAltitudeAtNearestGrid(x0, z1);
        const float h11 = terrain->GetAltitudeAtNearestGrid(x1, z1);
        const float hx0 = h00 + (h10 - h00) * tx;
        const float hx1 = h01 + (h11 - h01) * tx;
        return hx0 + (hx1 - hx0) * tz;
    }

    void ConformPointsToTerrain(std::vector<RoadDecalPoint>& points)
    {
        auto* terrain = GetActiveTerrain();
        if (!terrain) {
            return;
        }
        for (auto& point : points) {
            point.y = SampleTerrainHeight(terrain, point.x, point.z) + kDecalTerrainOffset;
        }
    }

    bool GetDirectionXZ(const RoadDecalPoint& a, const RoadDecalPoint& b, float& outTx, float& outTz, float& outLen)
    {
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len <= kMinLen) {
            return false;
        }
        outTx = dx / len;
        outTz = dz / len;
        outLen = len;
        return true;
    }

    void EmitTriangle(const RoadDecalPoint& a, const RoadDecalPoint& b, const RoadDecalPoint& c, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        outVerts.push_back({a.x, a.y, a.z, color});
        outVerts.push_back({b.x, b.y, b.z, color});
        outVerts.push_back({c.x, c.y, c.z, color});
    }

    void EmitQuad(const RoadDecalPoint& a, const RoadDecalPoint& b, const RoadDecalPoint& c, const RoadDecalPoint& d, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        EmitTriangle(a, b, c, color, outVerts);
        EmitTriangle(a, c, d, color, outVerts);
    }

    RoadDecalPoint LocalPoint(const RoadDecalPoint& center,
                              float rightX,
                              float rightZ,
                              float fwdX,
                              float fwdZ,
                              float lateral,
                              float forward)
    {
        return {
            center.x + rightX * lateral + fwdX * forward,
            center.y,
            center.z + rightZ * lateral + fwdZ * forward,
            false
        };
    }

    void BuildLine(const std::vector<RoadDecalPoint>& points,
                   float width,
                   DWORD color,
                   bool dashed,
                   float dashLength,
                   float gapLength,
                   std::vector<RoadDecalVertex>& outVerts)
    {
        if (points.size() < 2 || width <= 0.0f) {
            return;
        }

        auto path = points;
        ConformPointsToTerrain(path);

        dashLength = std::max(0.05f, dashLength);
        gapLength = std::max(0.0f, gapLength);
        const float cycleLength = dashLength + gapLength;
        float cyclePos = 0.0f;
        const float halfWidth = width * 0.5f;

        for (size_t i = 0; i + 1 < path.size(); ++i) {
            const auto& p0 = path[i];
            const auto& p1 = path[i + 1];
            float tx = 0.0f;
            float tz = 0.0f;
            float segLen = 0.0f;
            if (!GetDirectionXZ(p0, p1, tx, tz, segLen)) {
                continue;
            }
            const float nx = -tz;
            const float nz = tx;

            auto emitSlice = [&](float t0, float t1) {
                const RoadDecalPoint a{p0.x + (p1.x - p0.x) * t0, p0.y + (p1.y - p0.y) * t0, p0.z + (p1.z - p0.z) * t0, false};
                const RoadDecalPoint b{p0.x + (p1.x - p0.x) * t1, p0.y + (p1.y - p0.y) * t1, p0.z + (p1.z - p0.z) * t1, false};
                const RoadDecalPoint aL{a.x - nx * halfWidth, a.y, a.z - nz * halfWidth, false};
                const RoadDecalPoint aR{a.x + nx * halfWidth, a.y, a.z + nz * halfWidth, false};
                const RoadDecalPoint bL{b.x - nx * halfWidth, b.y, b.z - nz * halfWidth, false};
                const RoadDecalPoint bR{b.x + nx * halfWidth, b.y, b.z + nz * halfWidth, false};
                EmitQuad(aL, bL, bR, aR, color, outVerts);
            };

            if (!dashed || cycleLength <= 0.0f) {
                emitSlice(0.0f, 1.0f);
                continue;
            }

            float segPos = 0.0f;
            while (segPos < segLen) {
                const float boundary = (cyclePos < dashLength) ? dashLength : cycleLength;
                float step = boundary - cyclePos;
                if (step <= 0.0f) {
                    cyclePos = 0.0f;
                    continue;
                }
                step = (std::min)(step, segLen - segPos);

                if (cyclePos < dashLength) {
                    emitSlice(segPos / segLen, (segPos + step) / segLen);
                }
                segPos += step;
                cyclePos += step;
                if (cyclePos >= cycleLength - 1.0e-4f) {
                    cyclePos = 0.0f;
                }
            }
        }
    }

    void BuildStraightArrow(const RoadMarkupStroke& stroke, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        if (stroke.points.empty()) {
            return;
        }
        const RoadDecalPoint center = stroke.points.front();
        const float fx = std::cos(stroke.rotation);
        const float fz = std::sin(stroke.rotation);
        const float rx = -fz;
        const float rz = fx;
        const float width = std::max(0.2f, stroke.width);
        const float length = std::max(0.8f, stroke.length);

        auto p0 = LocalPoint(center, rx, rz, fx, fz, -0.15f * width, -0.50f * length);
        auto p1 = LocalPoint(center, rx, rz, fx, fz, -0.15f * width, 0.10f * length);
        auto p2 = LocalPoint(center, rx, rz, fx, fz, +0.15f * width, 0.10f * length);
        auto p3 = LocalPoint(center, rx, rz, fx, fz, +0.15f * width, -0.50f * length);
        auto tip = LocalPoint(center, rx, rz, fx, fz, 0.00f, 0.50f * length);
        auto hl = LocalPoint(center, rx, rz, fx, fz, -0.50f * width, 0.10f * length);
        auto hr = LocalPoint(center, rx, rz, fx, fz, +0.50f * width, 0.10f * length);

        std::vector<RoadDecalPoint> shape = {p0, p1, p2, p3, tip, hl, hr};
        ConformPointsToTerrain(shape);
        EmitQuad(shape[0], shape[1], shape[2], shape[3], color, outVerts);
        EmitTriangle(shape[5], shape[4], shape[6], color, outVerts);
    }

    void BuildTurnArrow(const RoadMarkupStroke& stroke, bool left, bool withStraight, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        if (withStraight) {
            BuildStraightArrow(stroke, color, outVerts);
        }
        const RoadDecalPoint center = stroke.points.front();
        const float fx = std::cos(stroke.rotation);
        const float fz = std::sin(stroke.rotation);
        const float rx = -fz;
        const float rz = fx;
        const float sign = left ? -1.0f : 1.0f;

        std::vector<RoadDecalPoint> path = {
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.35f * stroke.length),
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.05f * stroke.length),
            LocalPoint(center, rx, rz, fx, fz, sign * 0.35f * stroke.length, 0.25f * stroke.length),
        };
        BuildLine(path, std::max(0.08f, stroke.width * 0.30f), color, false, 0.0f, 0.0f, outVerts);

        RoadMarkupStroke head = stroke;
        head.points = {path.back()};
        head.rotation += left ? -1.57f : 1.57f;
        head.length = std::max(0.5f, stroke.length * 0.35f);
        head.width = std::max(0.4f, stroke.width * 0.90f);
        BuildStraightArrow(head, color, outVerts);
    }

    void BuildUTurnArrow(const RoadMarkupStroke& stroke, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        const RoadDecalPoint center = stroke.points.front();
        const float fx = std::cos(stroke.rotation);
        const float fz = std::sin(stroke.rotation);
        const float rx = -fz;
        const float rz = fx;

        std::vector<RoadDecalPoint> path = {
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.45f * stroke.length),
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.10f * stroke.length),
        };
        const float radius = std::max(0.4f, stroke.length * 0.28f);
        for (int i = 0; i <= 8; ++i) {
            const float t = static_cast<float>(i) / 8.0f;
            const float ang = -0.2f * 3.1415926f + (1.35f * 3.1415926f) * t;
            path.push_back(LocalPoint(center, rx, rz, fx, fz,
                                      -0.18f * stroke.length + std::cos(ang) * radius,
                                      +0.08f * stroke.length + std::sin(ang) * radius));
        }
        BuildLine(path, std::max(0.08f, stroke.width * 0.30f), color, false, 0.0f, 0.0f, outVerts);

        RoadMarkupStroke head = stroke;
        head.points = {path.back()};
        head.rotation += 3.1415926f;
        head.length = std::max(0.5f, stroke.length * 0.35f);
        head.width = std::max(0.4f, stroke.width * 0.90f);
        BuildStraightArrow(head, color, outVerts);
    }

    void BuildCrosswalk(const RoadMarkupStroke& stroke,
                        float stripe,
                        float gap,
                        bool ladderRails,
                        DWORD color,
                        std::vector<RoadDecalVertex>& outVerts)
    {
        const auto& start = stroke.points[0];
        const auto& end = stroke.points[1];
        float tx = 0.0f;
        float tz = 0.0f;
        float length = 0.0f;
        if (!GetDirectionXZ(start, end, tx, tz, length)) {
            return;
        }
        const float nx = -tz;
        const float nz = tx;
        const float depth = std::max(stripe, stroke.length);
        const float firstOffset = -0.5f * depth;
        const int stripes = std::max(1, static_cast<int>(std::floor((depth + gap) / (stripe + gap))));

        for (int i = 0; i < stripes; ++i) {
            const float offset = firstOffset + i * (stripe + gap) + stripe * 0.5f;
            const RoadDecalPoint a{start.x + nx * offset, start.y, start.z + nz * offset, false};
            const RoadDecalPoint b{end.x + nx * offset, end.y, end.z + nz * offset, false};
            BuildLine({a, b}, stripe, color, false, 0.0f, 0.0f, outVerts);
        }

        if (ladderRails) {
            const RoadDecalPoint l0{start.x + nx * firstOffset, start.y, start.z + nz * firstOffset, false};
            const RoadDecalPoint l1{start.x + nx * (firstOffset + depth), start.y, start.z + nz * (firstOffset + depth), false};
            const RoadDecalPoint r0{end.x + nx * firstOffset, end.y, end.z + nz * firstOffset, false};
            const RoadDecalPoint r1{end.x + nx * (firstOffset + depth), end.y, end.z + nz * (firstOffset + depth), false};
            BuildLine({l0, l1}, stripe * 0.5f, color, false, 0.0f, 0.0f, outVerts);
            BuildLine({r0, r1}, stripe * 0.5f, color, false, 0.0f, 0.0f, outVerts);
        }
    }
}

const RoadMarkupProperties& GetRoadMarkupProperties(RoadMarkupType type)
{
    return FindProps(type);
}

RoadMarkupCategory GetMarkupCategory(RoadMarkupType type)
{
    return FindProps(type).category;
}

const std::vector<RoadMarkupType>& GetRoadMarkupTypesForCategory(RoadMarkupCategory category)
{
    static const std::vector<RoadMarkupType> laneTypes = {
        RoadMarkupType::SolidWhiteLine,
        RoadMarkupType::DashedWhiteLine,
        RoadMarkupType::SolidYellowLine,
        RoadMarkupType::DashedYellowLine,
        RoadMarkupType::DoubleSolidYellow,
        RoadMarkupType::SolidWhiteEdgeLine,
    };
    static const std::vector<RoadMarkupType> arrowTypes = {
        RoadMarkupType::ArrowStraight,
        RoadMarkupType::ArrowLeft,
        RoadMarkupType::ArrowRight,
        RoadMarkupType::ArrowLeftRight,
        RoadMarkupType::ArrowStraightLeft,
        RoadMarkupType::ArrowStraightRight,
        RoadMarkupType::ArrowUTurn,
    };
    static const std::vector<RoadMarkupType> crossingTypes = {
        RoadMarkupType::ZebraCrosswalk,
        RoadMarkupType::LadderCrosswalk,
        RoadMarkupType::ContinentalCrosswalk,
        RoadMarkupType::StopBar,
    };
    static const std::vector<RoadMarkupType> zoneTypes = {
        RoadMarkupType::YieldTriangle,
        RoadMarkupType::ParkingSpace,
        RoadMarkupType::BikeSymbol,
        RoadMarkupType::BusLane,
    };
    static const std::vector<RoadMarkupType> textTypes = {
        RoadMarkupType::TextStop,
        RoadMarkupType::TextSlow,
        RoadMarkupType::TextSchool,
        RoadMarkupType::TextBusOnly,
    };

    switch (category) {
    case RoadMarkupCategory::LaneDivider:
        return laneTypes;
    case RoadMarkupCategory::DirectionalArrow:
        return arrowTypes;
    case RoadMarkupCategory::Crossing:
        return crossingTypes;
    case RoadMarkupCategory::ZoneMarking:
        return zoneTypes;
    case RoadMarkupCategory::TextLabel:
    default:
        return textTypes;
    }
}

void EnsureDefaultRoadMarkupLayer()
{
    if (!gRoadMarkupLayers.empty()) {
        gActiveLayerIndex = std::clamp(gActiveLayerIndex, 0, static_cast<int>(gRoadMarkupLayers.size()) - 1);
        return;
    }
    gRoadMarkupLayers.push_back({1, "Layer 1", {}, true, false, 0});
    gActiveLayerIndex = 0;
}

RoadMarkupLayer* GetActiveRoadMarkupLayer()
{
    EnsureDefaultRoadMarkupLayer();
    if (gActiveLayerIndex < 0 || gActiveLayerIndex >= static_cast<int>(gRoadMarkupLayers.size())) {
        return nullptr;
    }
    return &gRoadMarkupLayers[static_cast<size_t>(gActiveLayerIndex)];
}

bool AddRoadMarkupLayer(const std::string& name)
{
    EnsureDefaultRoadMarkupLayer();
    if (gRoadMarkupLayers.size() >= 10) {
        return false;
    }
    uint32_t id = 1;
    for (const auto& layer : gRoadMarkupLayers) {
        id = (std::max)(id, layer.id + 1);
    }
    gRoadMarkupLayers.push_back({id, name.empty() ? "Layer" : name, {}, true, false, static_cast<int>(gRoadMarkupLayers.size())});
    gActiveLayerIndex = static_cast<int>(gRoadMarkupLayers.size() - 1);
    return true;
}

void DeleteActiveRoadMarkupLayer()
{
    EnsureDefaultRoadMarkupLayer();
    if (gRoadMarkupLayers.size() <= 1) {
        gRoadMarkupLayers[0].strokes.clear();
        return;
    }
    gRoadMarkupLayers.erase(gRoadMarkupLayers.begin() + gActiveLayerIndex);
    gActiveLayerIndex = std::clamp(gActiveLayerIndex, 0, static_cast<int>(gRoadMarkupLayers.size()) - 1);
}

bool AddRoadMarkupStrokeToActiveLayer(const RoadMarkupStroke& stroke)
{
    auto* layer = GetActiveRoadMarkupLayer();
    if (!layer || layer->locked || layer->strokes.size() >= 100) {
        return false;
    }
    auto stored = stroke;
    stored.layerId = layer->id;
    layer->strokes.push_back(stored);
    return true;
}

void UndoLastRoadMarkupStroke()
{
    auto* layer = GetActiveRoadMarkupLayer();
    if (!layer || layer->strokes.empty()) {
        return;
    }
    layer->strokes.pop_back();
}

void ClearAllRoadMarkupStrokes()
{
    EnsureDefaultRoadMarkupLayer();
    for (auto& layer : gRoadMarkupLayers) {
        layer.strokes.clear();
    }
}

size_t GetTotalRoadMarkupStrokeCount()
{
    size_t total = 0;
    for (const auto& layer : gRoadMarkupLayers) {
        total += layer.strokes.size();
    }
    return total;
}

void RebuildRoadDecalGeometry()
{
    EnsureDefaultRoadMarkupLayer();
    gRoadDecalVertices.clear();
    for (const auto& layer : gRoadMarkupLayers) {
        if (!layer.visible) {
            continue;
        }
        for (const auto& stroke : layer.strokes) {
            BuildStrokeVertices(stroke, gRoadDecalVertices);
        }
    }
}

void DrawRoadDecals()
{
    if (gRoadDecalVertices.empty() && gRoadDecalActiveVertices.empty() && gRoadDecalPreviewVertices.empty()) {
        return;
    }

    auto* imguiService = gImGuiServiceForD3DOverlay.load(std::memory_order_acquire);
    if (!imguiService) {
        return;
    }

    IDirect3DDevice7* device = nullptr;
    IDirectDraw7* dd = nullptr;
    if (!imguiService->AcquireD3DInterfaces(&device, &dd)) {
        return;
    }

    if (dd) {
        dd->Release();
    }
    if (!device) {
        return;
    }

    {
        RoadDecalStateGuard state(device);
        device->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
        device->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRENDERSTATE_ALPHAREF, 0);
        device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRENDERSTATE_ZBIAS, kRoadDecalZBias);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        DrawVertexBuffer(device, gRoadDecalVertices);
        DrawVertexBuffer(device, gRoadDecalActiveVertices);
        DrawVertexBuffer(device, gRoadDecalPreviewVertices);
    }

    device->Release();
}

void SetRoadDecalActiveStroke(const RoadMarkupStroke* stroke)
{
    gRoadDecalActiveVertices.clear();
    if (stroke) {
        BuildStrokeVertices(*stroke, gRoadDecalActiveVertices);
    }
}

void SetRoadDecalPreviewSegment(bool enabled, const RoadMarkupStroke& stroke)
{
    gRoadDecalPreviewVertices.clear();
    if (enabled) {
        BuildStrokeVertices(stroke, gRoadDecalPreviewVertices);
    }
}

bool SaveMarkupsToFile(const char* filepath)
{
    if (!filepath || !filepath[0]) {
        return false;
    }
    EnsureDefaultRoadMarkupLayer();
    std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    if (!WriteValue(out, kMarkupFileMagic) || !WriteValue(out, kMarkupFileVersion)) {
        return false;
    }

    const uint32_t layerCount = static_cast<uint32_t>(gRoadMarkupLayers.size());
    if (!WriteValue(out, layerCount)) {
        return false;
    }
    for (const auto& layer : gRoadMarkupLayers) {
        const uint32_t nameLen = static_cast<uint32_t>(layer.name.size());
        const uint8_t visible = layer.visible ? 1 : 0;
        const uint8_t locked = layer.locked ? 1 : 0;
        const uint32_t strokeCount = static_cast<uint32_t>(layer.strokes.size());
        if (!WriteValue(out, layer.id) ||
            !WriteValue(out, nameLen) ||
            !WriteValue(out, visible) ||
            !WriteValue(out, locked) ||
            !WriteValue(out, layer.renderOrder) ||
            !WriteValue(out, strokeCount)) {
            return false;
        }
        out.write(layer.name.data(), nameLen);
        if (!out.good()) {
            return false;
        }
        for (const auto& stroke : layer.strokes) {
            const uint32_t type = static_cast<uint32_t>(stroke.type);
            const uint32_t pointCount = static_cast<uint32_t>(stroke.points.size());
            const uint8_t dashed = stroke.dashed ? 1 : 0;
            const uint8_t strokeVisible = stroke.visible ? 1 : 0;
            if (!WriteValue(out, type) ||
                !WriteValue(out, pointCount) ||
                !WriteValue(out, stroke.width) ||
                !WriteValue(out, stroke.length) ||
                !WriteValue(out, stroke.rotation) ||
                !WriteValue(out, dashed) ||
                !WriteValue(out, stroke.dashLength) ||
                !WriteValue(out, stroke.gapLength) ||
                !WriteValue(out, stroke.color) ||
                !WriteValue(out, stroke.opacity) ||
                !WriteValue(out, strokeVisible) ||
                !WriteValue(out, stroke.layerId)) {
                return false;
            }
            for (const auto& point : stroke.points) {
                const uint8_t hardCorner = point.hardCorner ? 1 : 0;
                if (!WriteValue(out, point.x) ||
                    !WriteValue(out, point.y) ||
                    !WriteValue(out, point.z) ||
                    !WriteValue(out, hardCorner)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool LoadMarkupsFromFile(const char* filepath)
{
    if (!filepath || !filepath[0]) {
        return false;
    }
    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!ReadValue(in, magic) || !ReadValue(in, version)) {
        return false;
    }
    if (magic != kMarkupFileMagic || version != kMarkupFileVersion) {
        return false;
    }

    uint32_t layerCount = 0;
    if (!ReadValue(in, layerCount)) {
        return false;
    }

    std::vector<RoadMarkupLayer> layers;
    layers.reserve(layerCount);

    for (uint32_t i = 0; i < layerCount; ++i) {
        RoadMarkupLayer layer{};
        uint32_t nameLen = 0;
        uint8_t visible = 0;
        uint8_t locked = 0;
        uint32_t strokeCount = 0;
        if (!ReadValue(in, layer.id) ||
            !ReadValue(in, nameLen) ||
            !ReadValue(in, visible) ||
            !ReadValue(in, locked) ||
            !ReadValue(in, layer.renderOrder) ||
            !ReadValue(in, strokeCount)) {
            return false;
        }
        layer.visible = visible != 0;
        layer.locked = locked != 0;
        layer.name.resize(nameLen);
        in.read(layer.name.data(), nameLen);
        if (!in.good()) {
            return false;
        }
        layer.strokes.reserve(strokeCount);
        for (uint32_t s = 0; s < strokeCount; ++s) {
            RoadMarkupStroke stroke{};
            uint32_t type = 0;
            uint32_t pointCount = 0;
            uint8_t dashed = 0;
            uint8_t strokeVisible = 0;
            if (!ReadValue(in, type) ||
                !ReadValue(in, pointCount) ||
                !ReadValue(in, stroke.width) ||
                !ReadValue(in, stroke.length) ||
                !ReadValue(in, stroke.rotation) ||
                !ReadValue(in, dashed) ||
                !ReadValue(in, stroke.dashLength) ||
                !ReadValue(in, stroke.gapLength) ||
                !ReadValue(in, stroke.color) ||
                !ReadValue(in, stroke.opacity) ||
                !ReadValue(in, strokeVisible) ||
                !ReadValue(in, stroke.layerId)) {
                return false;
            }
            stroke.type = static_cast<RoadMarkupType>(type);
            stroke.dashed = dashed != 0;
            stroke.visible = strokeVisible != 0;
            stroke.points.reserve(pointCount);
            for (uint32_t p = 0; p < pointCount; ++p) {
                RoadDecalPoint point{};
                uint8_t hardCorner = 0;
                if (!ReadValue(in, point.x) ||
                    !ReadValue(in, point.y) ||
                    !ReadValue(in, point.z) ||
                    !ReadValue(in, hardCorner)) {
                    return false;
                }
                point.hardCorner = hardCorner != 0;
                stroke.points.push_back(point);
            }
            layer.strokes.push_back(stroke);
        }
        layers.push_back(std::move(layer));
    }

    gRoadMarkupLayers = std::move(layers);
    EnsureDefaultRoadMarkupLayer();
    RebuildRoadDecalGeometry();
    return true;
}

namespace
{
    void BuildStrokeVertices(const RoadMarkupStroke& stroke, std::vector<RoadDecalVertex>& outVerts)
    {
        if (!stroke.visible || stroke.points.empty()) {
            return;
        }

        const auto& props = FindProps(stroke.type);
        const DWORD color = ApplyOpacity(stroke.color != 0 ? stroke.color : props.defaultColor, stroke.opacity);
        const float dashLength = std::max(0.05f, stroke.dashLength);
        const float gapLength = std::max(0.0f, stroke.gapLength);

        switch (props.category) {
        case RoadMarkupCategory::LaneDivider:
            if (stroke.points.size() < 2) {
                return;
            }
            if (stroke.type == RoadMarkupType::DoubleSolidYellow) {
                std::vector<RoadDecalPoint> p1 = stroke.points;
                std::vector<RoadDecalPoint> p2 = stroke.points;
                const float offset = (stroke.width + kDoubleYellowSpacing) * 0.5f;
                for (size_t i = 0; i < stroke.points.size(); ++i) {
                    const auto& prev = stroke.points[(i == 0) ? i : i - 1];
                    const auto& next = stroke.points[(i + 1 < stroke.points.size()) ? i + 1 : i];
                    float tx = 0.0f;
                    float tz = 0.0f;
                    float len = 0.0f;
                    if (!GetDirectionXZ(prev, next, tx, tz, len)) {
                        continue;
                    }
                    const float nx = -tz;
                    const float nz = tx;
                    p1[i].x += nx * offset;
                    p1[i].z += nz * offset;
                    p2[i].x -= nx * offset;
                    p2[i].z -= nz * offset;
                }
                BuildLine(p1, stroke.width, color, false, dashLength, gapLength, outVerts);
                BuildLine(p2, stroke.width, color, false, dashLength, gapLength, outVerts);
            } else {
                const bool dashed = stroke.dashed ||
                                    stroke.type == RoadMarkupType::DashedWhiteLine ||
                                    stroke.type == RoadMarkupType::DashedYellowLine;
                BuildLine(stroke.points, stroke.width, color, dashed, dashLength, gapLength, outVerts);
            }
            break;

        case RoadMarkupCategory::DirectionalArrow:
            switch (stroke.type) {
            case RoadMarkupType::ArrowStraight:
                BuildStraightArrow(stroke, color, outVerts);
                break;
            case RoadMarkupType::ArrowLeft:
                BuildTurnArrow(stroke, true, false, color, outVerts);
                break;
            case RoadMarkupType::ArrowRight:
                BuildTurnArrow(stroke, false, false, color, outVerts);
                break;
            case RoadMarkupType::ArrowLeftRight:
                BuildTurnArrow(stroke, true, false, color, outVerts);
                BuildTurnArrow(stroke, false, false, color, outVerts);
                break;
            case RoadMarkupType::ArrowStraightLeft:
                BuildTurnArrow(stroke, true, true, color, outVerts);
                break;
            case RoadMarkupType::ArrowStraightRight:
                BuildTurnArrow(stroke, false, true, color, outVerts);
                break;
            case RoadMarkupType::ArrowUTurn:
                BuildUTurnArrow(stroke, color, outVerts);
                break;
            default:
                break;
            }
            break;

        case RoadMarkupCategory::Crossing:
            if (stroke.points.size() < 2) {
                return;
            }
            switch (stroke.type) {
            case RoadMarkupType::ZebraCrosswalk:
                BuildCrosswalk(stroke, 0.5f, 0.5f, false, color, outVerts);
                break;
            case RoadMarkupType::LadderCrosswalk:
                BuildCrosswalk(stroke, 0.5f, 0.5f, true, color, outVerts);
                break;
            case RoadMarkupType::ContinentalCrosswalk:
                BuildCrosswalk(stroke, 0.8f, 0.8f, false, color, outVerts);
                break;
            case RoadMarkupType::StopBar:
                BuildLine(stroke.points, std::max(0.1f, stroke.width), color, false, 0.0f, 0.0f, outVerts);
                break;
            default:
                break;
            }
            break;

        case RoadMarkupCategory::ZoneMarking:
        case RoadMarkupCategory::TextLabel:
            break;
        }
    }

    void DrawVertexBuffer(IDirect3DDevice7* device, const std::vector<RoadDecalVertex>& verts)
    {
        if (verts.empty()) {
            return;
        }

        const HRESULT hr = device->DrawPrimitive(D3DPT_TRIANGLELIST,
                                                 D3DFVF_XYZ | D3DFVF_DIFFUSE,
                                                 const_cast<RoadDecalVertex*>(verts.data()),
                                                 static_cast<DWORD>(verts.size()),
                                                 D3DDP_WAIT);
        if (FAILED(hr)) {
            LOG_WARN("RoadMarkup: DrawPrimitive failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        }
    }

    template<typename T>
    bool WriteValue(std::ofstream& out, const T& value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return out.good();
    }

    template<typename T>
    bool ReadValue(std::ifstream& in, T& value)
    {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return in.good();
    }
}
