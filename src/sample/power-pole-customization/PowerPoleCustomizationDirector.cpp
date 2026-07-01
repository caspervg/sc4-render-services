// Custom power-pole cable attach points: lets a pole prop exemplar declare, per connection
// direction, how many cables it carries and exactly where each one attaches on the model,
// instead of being limited to the engine's two baked 4-point tables (_cardinalConnectionPoints /
// _diagonalConnectionPoints). See docs/sc4-powerline-tool-re.md for the full reverse-engineering
// trail behind every address and struct offset below.
//
// Data-driven surface: a pole prop exemplar opts in through attachment geometry, family-wide wire
// appearance, or optional per-wire appearance arrays. Exemplars defining none of these properties
// are untouched and retain the original cardinal/diagonal table and wire rendering.
//
// ATTACH-POINT FORMAT (Float32Array), one property per connection direction:
//   [x0,y0,z0, x1,y1,z1, ..., xN,yN,zN]
// The point count is inferred from the array length and clamped to the ini-configurable maximum.
// A legacy [pointCount, xyz...] prefix is accepted for compatibility with the original PoC draft.
//
// WIRE APPEARANCE PROPERTIES:
//   Wire Width Scale / Wire Sag Scale / Wire Maximum Sag are simple family-wide Float32 defaults.
//   Optional plural Float32Array properties override those values by wire index. Short arrays are
//   allowed; missing entries inherit the family-wide value. Attachment and override arrays use the
//   same wire ordering, matching vanilla's index-based strand pairing.
//
// Status: hook sites and data structures below are real, address-confirmed (see "Hook sites"
// section and docs/sc4-powerline-tool-re.md SS7-14). The cable-count/strand-build algorithm is
// written out in full and ACTIVE.
//
// First attempt (2026-07-01) registered extra strands as cSC4PowerLineOccupant city objects --
// confirmed in-game (docs SS13) to run with no crash but NOT make DrawPowerlines render anything,
// since rendering reads polyline data cached directly in each pole's own tConnection entry,
// independent of any cSC4PowerLineOccupant's existence. The current approach clears and rebuilds
// the complete regular-polyline list in each active tConnection, using the same erase, control-
// point, tessellation, and vector-insert helpers as vanilla. A separate validated inline lookup
// supplies per-wire width while leaving the rest of DrawPowerlines intact. This revision still
// requires its in-game verification pass -- see "Known gaps" at the bottom of this file.
//
// TEMPORARY PoC-ONLY ADDITION: a "polelinetest" cheat code (search kCheatPoleLineTest) that
// bypasses exemplar properties so the strand-count/rebuild path can be tested in isolation with
// zero new art/exemplars -- place or reload a power pole after toggling the cheat.

#include "cIGZCOM.h"
#include "cIGZCheatCodeManager.h"
#include "cIGZMessage2.h"
#include "cIGZMessage2Standard.h"
#include "cIGZPersistResourceManager.h"
#include "cISC4App.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cISCExemplarPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cIGZVariant.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cRZMessage2COMDirector.h"
#include "GZServPtrs.h"

#include "mini/ini.h"

#include "service/decal/RelativeCallPatch.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    constexpr uint32_t kDirectorID = 0xB07E11E5; // arbitrary, must not collide with another director in this Plugins folder
    constexpr uint16_t kSupportedGameVersion = 641;
    constexpr uint32_t kCheatCodeMessageType = 0x230E27AC; // fixed SC4 message type, same constant every sample uses

    // ------------------------------------------------------------------
    // TEMPORARY PoC-ONLY cheat code: "polelinetest". See docs/sc4-powerline-tool-re.md's PoC plan
    // (step 4). The production exemplar path is implemented but still needs its first in-game
    // validation pass. This cheat keeps geometry/count rebuilding testable independently: while
    // active, every pole that goes through
    // InitConnectionPointsHook (i.e. newly placed or reloaded poles) gets a synthetic multi-point
    // override built from ITS OWN already-baked vanilla attach point (duplicated N times, no new
    // art/exemplar needed). This isolates testing of the strand-count/append path (AppendPolylineToConnection)
    // from the exemplar-reading path. Toggle off, then delete this whole block (and the
    // gSyntheticTestModeEnabled branch in InitConnectionPointsHook) once in-game verification is done.
    // ------------------------------------------------------------------
    constexpr uint32_t kCheatPoleLineTest = 0xB07E1000;
    constexpr uint32_t kSyntheticTestPointCount = 6; // > vanilla's 4, so it's obvious if extras render
    bool gSyntheticTestModeEnabled = false;

    // ------------------------------------------------------------------
    // Hook sites -- SimCity 4.exe 1.1.641 absolute addresses.
    // All confirmed via search_instructions cross-referencing in the Mac/Windows Ghidra session;
    // see docs/sc4-powerline-tool-re.md SS7 for the full evidence trail per address.
    // ------------------------------------------------------------------

    // cSC4PowerPoleOccupant::InitConnectionPoints -- picks which attach-point table this pole uses.
    // Three confirmed call sites; redirect all three so every path that (re)builds a pole's table
    // goes through our hook, not just the common one.
    constexpr uintptr_t kInitConnectionPoints = 0x00649b30;
    constexpr uintptr_t kInitConnectionPoints_CallSite_Init = 0x0064ceaa; // new-pole initialization path
    constexpr uintptr_t kInitConnectionPoints_CallSite_SetDefaultExemplar = 0x0064d94e;
    constexpr uintptr_t kInitConnectionPoints_CallSite_Read = 0x0064ef09; // save-game load path

    // cSC4PowerPoleOccupant::AddConnection -- builds the strand polylines for a brand-new connection.
    // Called from cSC4PowerLineTool::PlacePoles at exactly 4 call sites (2 logical calls x 2 args-swapped).
    constexpr uintptr_t kAddConnection = 0x0064e3e0;
    constexpr std::array<uintptr_t, 4> kAddConnectionCallSites = {
        0x00651d9c, 0x00651dad, 0x00651f86, 0x00651f93,
    };

    // cSC4PowerPoleOccupant::UpdateConnection -- rebuilds strands when a pole's exemplar changes
    // (e.g. zone-driven model swap). Called from SetDefaultExemplar at 2 call sites.
    constexpr uintptr_t kUpdateConnection = 0x0064d510;
    constexpr std::array<uintptr_t, 2> kUpdateConnectionCallSites = {
        0x0064d968, 0x0064d970,
    };

    // ------------------------------------------------------------------
    // cSC4PowerPoleOccupant field offsets (Windows). See docs/sc4-powerline-tool-re.md SS9 --
    // only offsets independently confirmed by a real call site, not guessed by Mac-offset-by-analogy.
    // ------------------------------------------------------------------
    constexpr uint32_t kOccupant_ConnectionPointTablePtr = 0xe4; // InitConnectionPoints writes here
    constexpr uint32_t kOccupant_ConnectionsBegin = 0xac;        // confirmed via DrawPowerlines (clean decompile)
    constexpr uint32_t kOccupant_ConnectionsEnd = 0xb0;
    constexpr uint32_t kOccupant_DirectionFlags = 0xb8;          // confirmed via CreatePowerPole
    // SetPosition (0x0064c540) is invoked through the occupant subobject at completeObject+0x8.
    // Its raw [this+0x98/+0x9c/+0xa0] stores therefore resolve to these complete-object offsets.
    // GetLineConnectionPoints (0x00649ba0) independently reads the same +0xa0/+0xa4/+0xa8 triple.
    constexpr uint32_t kOccupant_PosX = 0xa0;
    constexpr uint32_t kOccupant_PosZ = 0xa8;                    // posY is the intervening +0xa4

    constexpr uint32_t kVanillaPointsPerDirection = 4; // engine's hardcoded baked-table arity
    constexpr uint32_t kVanillaDirectionStrideBytes = 0x30; // 4 points x 12 bytes, per the baked tables
    constexpr float kCellSize = 16.0f; // matches this project's existing convention (TerrainDiagonalFixDirector.cpp)

    // ------------------------------------------------------------------
    // tConnection -- one entry in a pole's connections vector (kOccupant_ConnectionsBegin/End),
    // 0x2c bytes each. Confirmed via DrawPowerlines' clean decompile: polyline-list/buoy-list
    // fields and the 0x2c stride. This is the struct DrawPowerlines actually renders from -- NOT
    // cSC4PowerLineOccupant (confirmed insufficient in-game, 2026-07-01, docs SS13).
    // UpdateConnection's raw Windows instructions independently confirm that +0x00 is the other
    // pole pointer and +0x09 is the active/render-owning flag. +0xc/+0x10 are cell coordinates,
    // not pole pointers.
    // ------------------------------------------------------------------
    constexpr uint32_t kConnection_OtherPole = 0x00;
    constexpr uint32_t kConnection_IsRenderOwner = 0x09;
    constexpr uint32_t kConnection_PolylinesBegin = 0x14; // vector<vector<cS3DVector3>>
    constexpr uint32_t kConnection_PolylinesEnd = 0x18;
    constexpr uint32_t kConnection_Size = 0x2c;

    // DrawPowerlines resolves the vanilla zoom-dependent width pointer at this instruction range.
    // Replace that lookup with a helper which returns either the vanilla pointer or a precomputed
    // per-wire scaled value. The overwritten bytes and boundaries are confirmed against 1.1.641.
    constexpr uintptr_t kWireWidthLookupPatchSite = 0x0064b855;
    constexpr size_t kWireWidthLookupPatchSize = 23;
    constexpr std::array<uint8_t, kWireWidthLookupPatchSize> kWireWidthLookupExpectedBytes = {
        0x8b, 0x44, 0x24, 0x14,                         // mov eax,[esp+14h] (occupant)
        0x8b, 0x88, 0xc4, 0x00, 0x00, 0x00,             // mov ecx,[eax+0c4h] (zoom)
        0x8b, 0x15, 0x98, 0x67, 0xb4, 0x00,             // mov edx,[00b46798h]
        0xd9, 0x54, 0x24, 0x78,                         // fst dword ptr [esp+78h]
        0x8d, 0x04, 0x8a,                               // lea eax,[edx+ecx*4]
    };
    constexpr uintptr_t kPowerLineWidthFactorsPtr = 0x00b46798;
    constexpr size_t kPowerLineZoomCount = 5;

    // Bezier control-point solver (2 endpoints -> 2 control points + a length-based tessellation
    // count), and the segment tessellator it feeds. Both confirmed via clean, self-contained
    // decompiles (no register-spill ambiguity, unlike AddConnection's own messy decompile).
    // GetControlPoints ends in `RET 0x24` (callee cleans 9*4 stack bytes) -- __stdcall, not
    // __cdecl. Confirmed in-game (2026-07-01): declaring it __cdecl double-cleaned the stack on
    // every call (caller AND callee both popping), causing a delayed __report_gsfailure.
    constexpr uintptr_t kGetControlPoints = 0x0064a6c0;
    using GetControlPointsFn = void(__stdcall*)(float x1, float y1, float z1, float x2, float y2, float z2,
                                                 float* outControl1, float* outControl2, int32_t* outCount);

    // Writes `count` tessellated points starting at byte address `outFirstPoint`, given the 4 bezier
    // control points (p0, ctrl1, ctrl2, p1) each as a 3-float pointer. Confirmed standalone.
    constexpr uintptr_t kTessellateBezierSegment = 0x006493b0;
    using TessellateBezierSegmentFn = void(__cdecl*)(const float* p0, const float* ctrl1, const float* ctrl2,
                                                       const float* p1, uintptr_t outFirstPoint, int32_t count);

    // vector<vector<cS3DVector3>>::insert(pos, count, value) -- confirmed via its own
    // std_vector_cS3DVector3_copy_construct helper (deep-copies begin/end of a vector<cS3DVector3>,
    // proving the OUTER vector's element type is itself a vector, not a raw point). Deep-copies
    // `value` rather than taking ownership, so a stack-local {begin,end,cap} view is a safe `value`
    // to pass -- no need to match this build's exact allocator/heap behavior for the source.
    constexpr uintptr_t kPolylineListInsert = 0x0064d020;
    using PolylineListInsertFn = void(__thiscall*)(void* polylinesVector, void* position, const void* value,
                                                     uint32_t unused, uint32_t count, uint8_t noTailRelocate);

    // vector<vector<cS3DVector3>>::erase(begin,end), used by vanilla UpdateConnection to destroy
    // every inner vector and reset the outer vector's end pointer before rebuilding it.
    constexpr uintptr_t kPolylineListErase = 0x0064cf40;
    using PolylineListEraseFn = void*(__thiscall*)(void* polylinesVector, void* begin, void* end);

    // cSC4NetworkRoutines::Get0To3Direction -- confirmed identical logic to the Mac twin
    // (byte-for-byte equivalent branch structure, verified by decompiling both).
    constexpr uintptr_t kGet0To3Direction = 0x0061f4c0;
    using Get0To3DirectionFn = uint8_t(__cdecl*)(uint32_t x1, uint32_t z1, uint32_t x2, uint32_t z2);

    // cSC4PowerLineOccupant (the wire/strand object) has its full vtable layout, ctor, and
    // SetConnectedPoles confirmed and documented in docs/sc4-powerline-tool-re.md SS9/SS10/SS13a --
    // no longer used by this file. Registering a strand as a cSC4PowerLineOccupant is a real,
    // working, non-crashing city-object registration (queryable/demolishable/save-load-capable),
    // but confirmed in-game (2026-07-01, docs SS13) to NOT make DrawPowerlines render it --
    // rendering reads polyline data cached directly in each pole's own tConnection entry instead
    // (see kConnection_PolylinesBegin below), independent of any cSC4PowerLineOccupant's existence.

    // ------------------------------------------------------------------
    // Custom exemplar property IDs. The full 0xB22A0000-0xB22A000F block was checked against the
    // vendored new_properties.xml registry (2026-07-01); no property in that registry uses the
    // block. Unregistered third-party properties can never be ruled out globally, but these IDs
    // have no known registry conflict and are intentionally kept contiguous for this feature.
    // ------------------------------------------------------------------
    constexpr uint32_t kPropAttachPointsDir0 = 0xB22A0000;
    constexpr uint32_t kPropAttachPointsDir1 = 0xB22A0001;
    constexpr uint32_t kPropAttachPointsDir2 = 0xB22A0002;
    constexpr uint32_t kPropAttachPointsDir3 = 0xB22A0003;
    constexpr uint32_t kPropWireWidthScale = 0xB22A0004;
    constexpr uint32_t kPropWireSagScale = 0xB22A0005;
    constexpr uint32_t kPropWireMaximumSag = 0xB22A0006;
    constexpr uint32_t kPropWireWidthScales = 0xB22A0007;
    constexpr uint32_t kPropWireSagScales = 0xB22A0008;
    constexpr uint32_t kPropWireMaximumSags = 0xB22A0009;
    constexpr uint32_t kPropFoundationHalfExtent = 0xB22A000A;
    constexpr uint32_t kPropFoundationFloorTextureId = 0xB22A000B;
    constexpr uint32_t kPropFoundationWallTextureId = 0xB22A000C;
    constexpr std::array<uint32_t, 4> kAttachPointProperties = {
        kPropAttachPointsDir0, kPropAttachPointsDir1, kPropAttachPointsDir2, kPropAttachPointsDir3,
    };

    // ------------------------------------------------------------------
    // Foundation pad (flat floor + 2 retaining walls, drawn under every pole to hide terrain
    // z-fighting on slopes). Confirmed via Ghidra (2026-07-02): cSC4PowerPoleOccupant::CreateFloor
    // (0x0064bfd0), called from SetPosition (0x0064c611) and lazily from Draw (0x0064c81f), builds a
    // flat quad from a unit square scaled by two hardcoded floats read directly from memory:
    // DAT_00a94d50 (10.0) * DAT_00a84d2c (0.5) = 5.0 half-extent -> fixed 10m x 10m pad, not read
    // from any exemplar property. cSC4PowerPoleOccupant::CreateWalls (0x0064c470) has no independent
    // size of its own -- FUN_0064c2c0 (its per-wall-quad builder) reads the floor's own vertex buffer
    // at occupant+0xe8 directly (confirmed via clean decompile), so rescaling that buffer after
    // vanilla CreateFloor runs is sufficient; CreateWalls needs no patch of its own.
    // ------------------------------------------------------------------
    constexpr uintptr_t kCreateFloor = 0x0064bfd0;
    constexpr uintptr_t kCreateFloor_CallSite_SetPosition = 0x0064c611;
    constexpr uintptr_t kCreateFloor_CallSite_Draw = 0x0064c81f;
    // FUN_0064c6c0 -- almost certainly cSC4PowerPoleOccupant::ChangeZoomLevel (not yet renamed in
    // Ghidra): writes this+0xc0/this+0xc4 from param_1+0x18/+0x14 immediately before this call,
    // exactly matching this file's existing doc note ("ChangeZoomLevel writes *(this+0xc0) =
    // drawContext->zoom"). A THIRD, previously-missed CreateFloor caller -- confirmed via
    // get_xrefs_to, not assumed. This is why the pad reverted to vanilla size specifically on
    // zoom/pan: crossing an LOD/zoom boundary re-triggers LoadModel+CreateFloor+CreateWalls through
    // this path, bypassing the two sites patched first.
    constexpr uintptr_t kCreateFloor_CallSite_ChangeZoomLevel = 0x0064c7de;
    constexpr uint32_t kOccupant_FloorVertsBegin = 0xe8; // vector<sVF_V3F_C4UB_T2F> begin ptr
    constexpr uint32_t kOccupant_FloorVertsEnd = 0xec;
    constexpr uint32_t kFloorVertexStride = 0x18; // 6 floats/vertex: x,y,z,color,u,v -- confirmed via FUN_0064c2c0
    constexpr float kVanillaFoundationHalfExtent = 5.0f; // DAT_00a94d50(10.0) * DAT_00a84d2c(0.5), confirmed via memory read
    constexpr float kMaxFoundationHalfExtent = 50.0f; // sanity clamp, matches this file's other property-clamping convention

    // Floor/wall texture-hash lookup sites inside cSC4PowerPoleOccupant::Draw (0x0064c800). Each is a
    // bare `MOV EAX, imm32` (5 bytes: B8 + LE imm32), read twice per texture -- once to compute the
    // hash-bucket index (DIV), once inside the chain-walk comparison. Both instances of a pair MUST
    // return the identical value or the bucket lookup mismatches its own index. Confirmed via raw
    // memory read against 1.1.641 (not just disassembly text).
    constexpr uintptr_t kFloorTextureHashSite1 = 0x0064c9e7;
    constexpr uintptr_t kFloorTextureHashSite2 = 0x0064c9f5;
    constexpr uintptr_t kWallTextureHashSite1 = 0x0064ca76;
    constexpr uintptr_t kWallTextureHashSite2 = 0x0064ca84;
    constexpr uint32_t kVanillaFloorTextureId = 0x0912220E;
    constexpr uint32_t kVanillaWallTextureId = 0x08080004;

    // ------------------------------------------------------------------
    // Data-driven settings (SC4PowerPoleCustomization.ini).
    // ------------------------------------------------------------------
    struct Settings {
        bool enabled = true;
        uint32_t maxPointsPerDirection = 8; // raise to allow denser cable bundles than vanilla's 4
    };

    Settings gSettings;

    // ------------------------------------------------------------------
    // Per-pole attach-point override -- the data-driven core of this mod.
    //
    // Deliberately NOT stored inside cSC4PowerPoleOccupant itself: the engine allocates these
    // objects through a fixed-size operator new (object size confirmed 292 bytes on Windows, see
    // docs SS9), and several "unk_*" fields past our recovered struct boundary are still unmapped.
    // Growing the live object is a real risk (see also the project's existing fixed-pool-growth
    // bug notes for this engine's allocators); a side table keyed by occupant pointer carries the
    // override data without touching engine memory layout at all.
    // ------------------------------------------------------------------
    struct AttachPoint {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct PoleAttachOverride {
        std::array<std::vector<AttachPoint>, 4> perDirection; // index = connection direction 0-3
        float defaultWidthScale = 1.0f;
        float defaultSagScale = 1.0f;
        float defaultMaximumSag = std::numeric_limits<float>::infinity();
        std::vector<float> perWireWidthScales;
        std::vector<float> perWireSagScales;
        std::vector<float> perWireMaximumSags;
        bool hasWidthCustomization = false;
        bool hasSagCustomization = false;
        float foundationHalfExtent = kVanillaFoundationHalfExtent;
        bool hasFoundationHalfExtent = false;
        uint32_t foundationFloorTextureId = kVanillaFloorTextureId;
        bool hasFloorTexture = false;
        uint32_t foundationWallTextureId = kVanillaWallTextureId;
        bool hasWallTexture = false;

        [[nodiscard]] uint32_t PointCount(const uint32_t direction) const {
            return direction < 4 ? static_cast<uint32_t>(perDirection[direction].size()) : 0;
        }

        [[nodiscard]] const AttachPoint* Point(const uint32_t direction, const uint32_t index) const {
            if (direction >= 4 || index >= perDirection[direction].size()) {
                return nullptr;
            }
            return &perDirection[direction][index];
        }

        [[nodiscard]] bool HasAttachPointCustomization() const {
            return std::ranges::any_of(perDirection, [](const auto& points) { return !points.empty(); });
        }

        [[nodiscard]] float WidthScale(const uint32_t index) const {
            return index < perWireWidthScales.size() ? perWireWidthScales[index] : defaultWidthScale;
        }

        [[nodiscard]] float SagScale(const uint32_t index) const {
            return index < perWireSagScales.size() ? perWireSagScales[index] : defaultSagScale;
        }

        [[nodiscard]] float MaximumSag(const uint32_t index) const {
            return index < perWireMaximumSags.size() ? perWireMaximumSags[index] : defaultMaximumSag;
        }
    };

    // this(cSC4PowerPoleOccupant*) -> override. Entries exist only when at least one valid attach-
    // point or wire-appearance property is present. Everything else stays entirely vanilla.
    std::unordered_map<void*, PoleAttachOverride> gOverrides;

    struct PolylineWidthOverride {
        void* owner = nullptr;
        std::array<float, kPowerLineZoomCount> widthByZoom{};
    };

    // Keyed by the inner vector's point-buffer address. Outer vector reallocation deep-copies the
    // inner vectors, so registrations are made only after the full connection rebuild is complete.
    std::unordered_map<const void*, PolylineWidthOverride> gPolylineWidthOverrides;

    // ------------------------------------------------------------------
    // Property reading. cISCPropertyHolder doesn't have a typed array getter (SCPropertyUtil only
    // covers scalars), so float arrays go through cISCProperty/cIGZVariant directly.
    // ------------------------------------------------------------------
    bool TryReadAttachPointProperty(const cISCPropertyHolder* holder, const uint32_t propertyId, std::vector<AttachPoint>& outPoints) {
        outPoints.clear();
        if (holder == nullptr || !holder->HasProperty(propertyId)) {
            return false;
        }

        const cISCProperty* const prop = holder->GetProperty(propertyId);
        const cIGZVariant* const variant = prop ? prop->GetPropertyValue() : nullptr;
        if (variant == nullptr || variant->GetType() != cIGZVariant::Float32Array) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} is present but not a Float32 array; ignoring.", propertyId);
            return false;
        }

        const uint32_t valueCount = variant->GetCount();
        if (valueCount < 3) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} has {} values, expected one or more XYZ triples; ignoring.", propertyId, valueCount);
            return false;
        }

        const float* const values = variant->RefFloat32();
        uint32_t firstCoordinate = 0;
        uint32_t availableCount = 0;
        uint32_t requestedCount = 0;
        if (valueCount % 3 == 0) {
            availableCount = valueCount / 3;
            requestedCount = availableCount;
        } else if (valueCount >= 4 && (valueCount - 1) % 3 == 0 && std::isfinite(values[0]) &&
                   values[0] >= 1.0f && std::floor(values[0]) == values[0]) {
            // Compatibility with the original PoC draft: [count, xyz...]. New properties should
            // use plain XYZ triples; new_properties.xml enforces groups of three.
            firstCoordinate = 1;
            availableCount = (valueCount - 1) / 3;
            requestedCount = static_cast<uint32_t>(values[0]);
        } else {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} has {} values, expected 3*N XYZ values; ignoring.", propertyId, valueCount);
            return false;
        }

        const uint32_t pointCount = std::clamp(std::min(requestedCount, availableCount), 1u, gSettings.maxPointsPerDirection);

        outPoints.reserve(pointCount);
        for (uint32_t i = 0; i < pointCount; ++i) {
            const float* const xyz = values + firstCoordinate + i * 3;
            if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
                LOG_WARN("PowerPoleCustomization: property 0x{:08X} contains a non-finite coordinate; ignoring.", propertyId);
                outPoints.clear();
                return false;
            }
            outPoints.push_back(AttachPoint{xyz[0], xyz[1], xyz[2]});
        }
        return true;
    }

    bool TryReadNonNegativeFloatProperty(const cISCPropertyHolder* holder, const uint32_t propertyId, float& outValue) {
        if (holder == nullptr || !holder->HasProperty(propertyId)) {
            return false;
        }
        const cISCProperty* const prop = holder->GetProperty(propertyId);
        const cIGZVariant* const variant = prop ? prop->GetPropertyValue() : nullptr;
        float value = 0.0f;
        if (variant == nullptr || variant->GetType() != cIGZVariant::Float32 ||
            !variant->GetValFloat32(value) || !std::isfinite(value) || value < 0.0f) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} must be a non-negative Float32; ignoring.", propertyId);
            return false;
        }
        outValue = value;
        return true;
    }

    // Texture-ID properties are raw resource keys, not appearance scalars: no amount of range
    // clamping here can guarantee the ID resolves to a loaded texture at Draw() time. An unresolved
    // ID walks the engine's texture-cache hash chain to a null bucket and is dereferenced with no
    // null check (confirmed via disassembly of cSC4PowerPoleOccupant::Draw) -- a crash, not a
    // silently-transparent pad. Warn loudly so authors don't assume "unknown ID = invisible".
    bool TryReadUint32Property(const cISCPropertyHolder* holder, const uint32_t propertyId, uint32_t& outValue) {
        if (holder == nullptr || !holder->HasProperty(propertyId)) {
            return false;
        }
        const cISCProperty* const prop = holder->GetProperty(propertyId);
        const cIGZVariant* const variant = prop ? prop->GetPropertyValue() : nullptr;
        uint32_t value = 0;
        if (variant == nullptr || variant->GetType() != cIGZVariant::Uint32 || !variant->GetValUint32(value)) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} must be a Uint32; ignoring.", propertyId);
            return false;
        }
        outValue = value;
        return true;
    }

    bool TryReadNonNegativeFloatArray(const cISCPropertyHolder* holder, const uint32_t propertyId,
                                      std::vector<float>& outValues) {
        outValues.clear();
        if (holder == nullptr || !holder->HasProperty(propertyId)) {
            return false;
        }
        const cISCProperty* const prop = holder->GetProperty(propertyId);
        const cIGZVariant* const variant = prop ? prop->GetPropertyValue() : nullptr;
        if (variant == nullptr || variant->GetType() != cIGZVariant::Float32Array || variant->GetCount() == 0) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} must be a non-empty Float32 array; ignoring.", propertyId);
            return false;
        }

        const uint32_t count = std::min(variant->GetCount(), gSettings.maxPointsPerDirection);
        const float* const values = variant->RefFloat32();
        outValues.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!std::isfinite(values[i]) || values[i] < 0.0f) {
                LOG_WARN("PowerPoleCustomization: property 0x{:08X} contains a negative or non-finite value; ignoring.", propertyId);
                outValues.clear();
                return false;
            }
            outValues.push_back(values[i]);
        }
        return true;
    }

    // Builds (or refreshes) this pole's customization from its current default exemplar. Property
    // lookup includes cohort inheritance, so family-wide appearance defaults can live on a shared
    // cohort while individual prop exemplars define only their local attachment geometry.
    bool TryBuildOverride(cISCResExemplar* exemplar, PoleAttachOverride& out) {
        if (exemplar == nullptr) {
            return false;
        }

        const cISCPropertyHolder* const holder = exemplar->AsISCPropertyHolder();
        bool any = false;
        for (uint32_t direction = 0; direction < 4; ++direction) {
            if (TryReadAttachPointProperty(holder, kAttachPointProperties[direction], out.perDirection[direction])) {
                any = true;
            }
        }

        if (TryReadNonNegativeFloatProperty(holder, kPropWireWidthScale, out.defaultWidthScale)) {
            out.hasWidthCustomization = true;
            any = true;
        }
        if (TryReadNonNegativeFloatArray(holder, kPropWireWidthScales, out.perWireWidthScales)) {
            out.hasWidthCustomization = true;
            any = true;
        }
        if (TryReadNonNegativeFloatProperty(holder, kPropWireSagScale, out.defaultSagScale)) {
            out.hasSagCustomization = true;
            any = true;
        }
        if (TryReadNonNegativeFloatProperty(holder, kPropWireMaximumSag, out.defaultMaximumSag)) {
            out.hasSagCustomization = true;
            any = true;
        }
        if (TryReadNonNegativeFloatArray(holder, kPropWireSagScales, out.perWireSagScales)) {
            out.hasSagCustomization = true;
            any = true;
        }
        if (TryReadNonNegativeFloatArray(holder, kPropWireMaximumSags, out.perWireMaximumSags)) {
            out.hasSagCustomization = true;
            any = true;
        }

        if (TryReadNonNegativeFloatProperty(holder, kPropFoundationHalfExtent, out.foundationHalfExtent)) {
            out.foundationHalfExtent = std::min(out.foundationHalfExtent, kMaxFoundationHalfExtent);
            out.hasFoundationHalfExtent = true;
            any = true;
        }
        if (TryReadUint32Property(holder, kPropFoundationFloorTextureId, out.foundationFloorTextureId)) {
            out.hasFloorTexture = true;
            any = true;
            LOG_WARN("PowerPoleCustomization: custom floor texture ID 0x{:08X} is unvalidated -- an ID with "
                     "no loaded texture resource can crash the game (null dereference in the engine's texture "
                     "cache lookup, not a transparent pad). Verify it resolves to a real loaded texture.",
                     out.foundationFloorTextureId);
        }
        if (TryReadUint32Property(holder, kPropFoundationWallTextureId, out.foundationWallTextureId)) {
            out.hasWallTexture = true;
            any = true;
            LOG_WARN("PowerPoleCustomization: custom wall texture ID 0x{:08X} is unvalidated -- same crash "
                     "risk as the floor texture ID.", out.foundationWallTextureId);
        }
        return any;
    }

    // TEMPORARY PoC-ONLY. See the block comment above kCheatPoleLineTest. Duplicates this pole's
    // own already-baked vanilla attach point (read raw, before position translation -- matches
    // what GetAttachPoint()/PoleAttachOverride expect to store) kSyntheticTestPointCount times per
    // direction. Requires the real InitConnectionPoints call to have already run for this pole
    // (true in InitConnectionPointsHook, which calls `original(occupant)` first).
    //
    // Y offset deliberately large (not a subtle +0.5/index): appended strands rendering with no
    // errors and no crash but "not visible" (2026-07-01) is ambiguous between "nothing is actually
    // drawing" and "it's drawing but subtly overlapping the vanilla wires" -- a huge, obvious offset
    // turns that into a clean yes/no (a wire floating tens of units in the air is unmissable if it
    // renders at all). Shrink back down once that's answered.
    PoleAttachOverride BuildSyntheticTestOverride(void* occupant) {
        PoleAttachOverride result;
        auto* const base = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionPointTablePtr);
        if (base == nullptr) {
            return result;
        }

        for (uint32_t direction = 0; direction < 4; ++direction) {
            const auto* const vanillaPoint = reinterpret_cast<const float*>(base + (direction & 3) * kVanillaDirectionStrideBytes);
            result.perDirection[direction].reserve(kSyntheticTestPointCount);
            for (uint32_t i = 0; i < kSyntheticTestPointCount; ++i) {
                result.perDirection[direction].push_back(AttachPoint{
                    vanillaPoint[0], vanillaPoint[1] + static_cast<float>(i) * 20.0f, vanillaPoint[2]});
            }
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Vanilla attach-point read, reimplemented for the case where a pole has no override -- exact
    // same arithmetic as the original GetLineConnectionPoints (inlined into AddConnection /
    // UpdateConnection on Windows, so it has no address of its own to call into): table base is
    // read from this+kOccupant_ConnectionPointTablePtr, point N of direction D is the cS3DVector3
    // at (D & 3) * 0x30 + N * 0xc, translated by this pole's world position.
    // ------------------------------------------------------------------
    AttachPoint ReadVanillaAttachPoint(void* occupant, const uint32_t direction, const uint32_t index) {
        auto* const base = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionPointTablePtr);
        const auto* const point = reinterpret_cast<const float*>(
            base + (direction & 3) * kVanillaDirectionStrideBytes + index * 0xc);

        const auto* const position = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_PosX);
        return AttachPoint{point[0] + position[0], point[1] + position[1], point[2] + position[2]};
    }

    // Number of strands this pole exposes in a given direction: override count if one exists,
    // otherwise the vanilla fixed arity. This is the function that replaces the engine's hardcoded
    // "4" -- everywhere AddConnection/UpdateConnection looped a literal 4 times, they should now
    // loop std::min(StrandCount(poleA, dir), StrandCount(poleB, dir)) times instead.
    uint32_t StrandCount(void* occupant, const uint32_t direction) {
        const auto it = gOverrides.find(occupant);
        if (it == gOverrides.end()) {
            return kVanillaPointsPerDirection;
        }
        const uint32_t count = it->second.PointCount(direction);
        return count > 0 ? count : kVanillaPointsPerDirection;
    }

    AttachPoint GetAttachPoint(void* occupant, const uint32_t direction, const uint32_t index) {
        const auto it = gOverrides.find(occupant);
        if (it != gOverrides.end()) {
            if (const AttachPoint* const p = it->second.Point(direction, index)) {
                const auto* const position = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_PosX);
                return AttachPoint{p->x + position[0], p->y + position[1], p->z + position[2]};
            }
        }
        return ReadVanillaAttachPoint(occupant, direction, index);
    }

    // ------------------------------------------------------------------
    // Finds the active/render-owning tConnection for this exact pole pair. Windows UpdateConnection
    // performs the same scan: entry+0x00 must equal otherPole and entry+0x09 must be nonzero. Only
    // that side contains the polylines DrawPowerlines visits; the reciprocal inactive entry normally
    // has an empty polyline vector and must not be topped up.
    // ------------------------------------------------------------------
    void* FindConnectionEntry(void* pole, void* otherPole) {
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(pole) + kOccupant_ConnectionsBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(pole) + kOccupant_ConnectionsEnd);
        for (uint8_t* entry = begin; entry != end; entry += kConnection_Size) {
            if (*reinterpret_cast<void**>(entry + kConnection_OtherPole) == otherPole &&
                *reinterpret_cast<const uint8_t*>(entry + kConnection_IsRenderOwner) != 0) {
                return entry;
            }
        }
        return nullptr;
    }

    const PoleAttachOverride* FindAppearanceOverride(void* preferredPole, void* otherPole) {
        const auto preferred = gOverrides.find(preferredPole);
        if (preferred != gOverrides.end() &&
            (preferred->second.hasWidthCustomization || preferred->second.hasSagCustomization)) {
            return &preferred->second;
        }
        const auto other = gOverrides.find(otherPole);
        if (other != gOverrides.end() &&
            (other->second.hasWidthCustomization || other->second.hasSagCustomization)) {
            return &other->second;
        }
        return nullptr;
    }

    bool HasAttachPointCustomization(void* occupant, const uint32_t direction) {
        const auto it = gOverrides.find(occupant);
        return it != gOverrides.end() && it->second.PointCount(direction) > 0;
    }

    void AdjustControlPointSag(float* controlPoint, const AttachPoint& a, const AttachPoint& b,
                               const float sagScale, const float maximumSag) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        const float lengthSquared = dx * dx + dy * dy + dz * dz;
        if (lengthSquared <= std::numeric_limits<float>::epsilon()) {
            return;
        }

        const float along = ((controlPoint[0] - a.x) * dx + (controlPoint[1] - a.y) * dy +
                             (controlPoint[2] - a.z) * dz) / lengthSquared;
        const float straightLineY = a.y + dy * along;
        const float vanillaSag = std::max(0.0f, straightLineY - controlPoint[1]);
        const float scaledSag = std::min(vanillaSag * sagScale, maximumSag);
        controlPoint[1] = straightLineY - scaledSag;
    }

    // Builds one tessellated Bezier polyline between two attach points and appends it into the
    // already-resolved active tConnection. Vanilla still supplies its horizontal control geometry
    // and tessellation count; only the vertical sag component is scaled/clamped per wire.
    void AppendPolylineToConnection(void* entry, const AttachPoint& a, const AttachPoint& b,
                                    const float sagScale, const float maximumSag) {
        float control1[3];
        float control2[3];
        int32_t pointCount = 0;
        const auto getControlPoints = reinterpret_cast<GetControlPointsFn>(kGetControlPoints);
        getControlPoints(a.x, a.y, a.z, b.x, b.y, b.z, control1, control2, &pointCount);
        AdjustControlPointSag(control1, a, b, sagScale, maximumSag);
        AdjustControlPointSag(control2, a, b, sagScale, maximumSag);
        pointCount = std::clamp(pointCount, 2, 64); // vanilla treats this as total points, including p0

        const float p0[3] = {a.x, a.y, a.z};
        const float p1[3] = {b.x, b.y, b.z};
        std::vector<float> points(static_cast<size_t>(pointCount) * 3);
        points[0] = a.x;
        points[1] = a.y;
        points[2] = a.z;
        const auto tessellate = reinterpret_cast<TessellateBezierSegmentFn>(kTessellateBezierSegment);
        tessellate(p0, control1, control2, p1, reinterpret_cast<uintptr_t>(points.data() + 3), pointCount - 1);

        const void* view[3] = {points.data(), points.data() + points.size(), points.data() + points.size()};
        auto* const polylines = reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin;
        void* const insertPos = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesEnd);
        const auto insert = reinterpret_cast<PolylineListInsertFn>(kPolylineListInsert);
        insert(polylines, insertPos, view, 0, 1, 1);
    }

    void ForgetConnectionWidths(void* entry) {
        if (entry == nullptr) {
            return;
        }
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesEnd);
        for (uint8_t* polyline = begin; polyline != end; polyline += 12) {
            const void* const points = *reinterpret_cast<void* const*>(polyline);
            if (points != nullptr) {
                gPolylineWidthOverrides.erase(points);
            }
        }
    }

    void ForgetOccupantWidths(void* occupant) {
        std::erase_if(gPolylineWidthOverrides,
                      [occupant](const auto& item) { return item.second.owner == occupant; });
    }

    void ClearConnectionPolylines(void* entry) {
        ForgetConnectionWidths(entry);
        auto* const begin = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin);
        auto* const end = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesEnd);
        const auto erase = reinterpret_cast<PolylineListEraseFn>(kPolylineListErase);
        erase(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin, begin, end);
    }

    void RegisterConnectionWidths(void* occupant, void* entry, const PoleAttachOverride* appearance) {
        ForgetConnectionWidths(entry);
        if (appearance == nullptr || !appearance->hasWidthCustomization) {
            return;
        }

        const float* const vanillaWidths = *reinterpret_cast<float**>(kPowerLineWidthFactorsPtr);
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesEnd);
        uint32_t wireIndex = 0;
        for (uint8_t* polyline = begin; polyline != end; polyline += 12, ++wireIndex) {
            const float scale = appearance->WidthScale(wireIndex);
            if (scale == 1.0f) {
                continue;
            }
            const void* const points = *reinterpret_cast<void* const*>(polyline);
            if (points == nullptr) {
                continue;
            }
            PolylineWidthOverride value;
            value.owner = occupant;
            for (size_t zoom = 0; zoom < kPowerLineZoomCount; ++zoom) {
                value.widthByZoom[zoom] = vanillaWidths[zoom] * scale;
            }
            gPolylineWidthOverrides[points] = value;
        }
    }

    const float* __cdecl ResolveWireWidthPointer(void* occupant, const void* polyline) noexcept {
        const float* const vanillaWidths = *reinterpret_cast<float**>(kPowerLineWidthFactorsPtr);
        const uint32_t zoom = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(occupant) + 0xc4);
        const void* const points = polyline ? *reinterpret_cast<void* const*>(polyline) : nullptr;
        const auto it = points ? gPolylineWidthOverrides.find(points) : gPolylineWidthOverrides.end();
        if (it != gPolylineWidthOverrides.end() && it->second.owner == occupant && zoom < kPowerLineZoomCount) {
            return &it->second.widthByZoom[zoom];
        }
        return vanillaWidths + zoom;
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    // Called from 0x0064b855. At entry, EDI points to the current outer polyline-vector element and
    // the complete pole occupant is at caller ESP+0x14. CALL pushes a return address, hence +0x18
    // here. Reproduce the overwritten FST at caller ESP+0x78 as callee ESP+0x7c.
    __declspec(naked) void WireWidthLookupHook() {
        __asm {
            mov eax, dword ptr [esp + 0x18]
            push edi
            push eax
            call ResolveWireWidthPointer
            add esp, 8
            fst dword ptr [esp + 0x7c]
            ret
        }
    }
#else
#error Power-pole wire-width hook requires 32-bit MSVC.
#endif

    class InlineCallPatch final {
    public:
        bool Install() {
            if (installed_) {
                return true;
            }
            auto* const site = reinterpret_cast<uint8_t*>(kWireWidthLookupPatchSite);
            if (std::memcmp(site, kWireWidthLookupExpectedBytes.data(), kWireWidthLookupPatchSize) != 0) {
                LOG_ERROR("PowerPoleCustomization: wire-width patch bytes at 0x{:08X} do not match 1.1.641; not patching.",
                          static_cast<uint32_t>(kWireWidthLookupPatchSite));
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(site, kWireWidthLookupPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for wire-width patch (error {}).", GetLastError());
                return false;
            }
            std::memcpy(original_.data(), site, original_.size());
            const auto relative = static_cast<int32_t>(
                reinterpret_cast<intptr_t>(&WireWidthLookupHook) -
                (static_cast<intptr_t>(kWireWidthLookupPatchSite) + 5));
            site[0] = 0xe8;
            std::memcpy(site + 1, &relative, sizeof(relative));
            std::fill(site + 5, site + kWireWidthLookupPatchSize, static_cast<uint8_t>(0x90));
            FlushInstructionCache(GetCurrentProcess(), site, kWireWidthLookupPatchSize);
            DWORD ignored = 0;
            VirtualProtect(site, kWireWidthLookupPatchSize, oldProtect, &ignored);
            installed_ = true;
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const site = reinterpret_cast<uint8_t*>(kWireWidthLookupPatchSite);
            DWORD oldProtect = 0;
            if (VirtualProtect(site, kWireWidthLookupPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                std::memcpy(site, original_.data(), original_.size());
                FlushInstructionCache(GetCurrentProcess(), site, kWireWidthLookupPatchSize);
                DWORD ignored = 0;
                VirtualProtect(site, kWireWidthLookupPatchSize, oldProtect, &ignored);
            }
            installed_ = false;
        }

        [[nodiscard]] bool IsInstalled() const noexcept { return installed_; }

    private:
        std::array<uint8_t, kWireWidthLookupPatchSize> original_{};
        bool installed_ = false;
    };

    // Direction between two poles, via the confirmed Get0To3Direction. Windows AddConnection reads
    // cached cell coordinates at complete-object +0xc8/+0xcc, but recomputing them from the confirmed
    // position triple mirrors SetPosition's calculation and avoids depending on cache freshness.
    uint32_t CellCoord(void* occupant, const uint32_t posOffset) {
        const float pos = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(occupant) + posOffset);
        return static_cast<uint32_t>(pos / kCellSize);
    }

    uint32_t DirectionBetween(void* poleA, void* poleB) {
        const auto get0To3Direction = reinterpret_cast<Get0To3DirectionFn>(kGet0To3Direction);
        return get0To3Direction(
            CellCoord(poleA, kOccupant_PosX), CellCoord(poleA, kOccupant_PosZ),
            CellCoord(poleB, kOccupant_PosX), CellCoord(poleB, kOccupant_PosZ));
    }

    // Rebuilds the complete regular-polyline list whenever either endpoint customizes attachment
    // geometry/count or sag. Width-only customization keeps vanilla geometry and merely registers
    // per-polyline width values. The active/render-owning side determines appearance when two pole
    // families disagree; if it has no appearance properties, the other endpoint supplies them.
    void ApplyConnectionCustomization(void* occupant, void* otherPole) {
        const uint32_t direction = DirectionBetween(occupant, otherPole);
        const uint32_t count = std::min(StrandCount(occupant, direction), StrandCount(otherPole, direction));
        void* const entry = FindConnectionEntry(occupant, otherPole);
        if (entry == nullptr) {
            return; // reciprocal non-rendering side, or no live connection for this pair
        }

        const PoleAttachOverride* const appearance = FindAppearanceOverride(occupant, otherPole);
        const bool rebuildGeometry = HasAttachPointCustomization(occupant, direction) ||
                                     HasAttachPointCustomization(otherPole, direction) ||
                                     (appearance != nullptr && appearance->hasSagCustomization);
        if (rebuildGeometry) {
            ClearConnectionPolylines(entry);
            for (uint32_t i = 0; i < count; ++i) {
                const AttachPoint a = GetAttachPoint(occupant, direction, i);
                const AttachPoint b = GetAttachPoint(otherPole, direction, i);
                const float sagScale = appearance ? appearance->SagScale(i) : 1.0f;
                const float maximumSag = appearance ? appearance->MaximumSag(i) :
                                                      std::numeric_limits<float>::infinity();
                AppendPolylineToConnection(entry, a, b, sagScale, maximumSag);
            }
            LOG_INFO("PowerPoleCustomization: rebuilt {} strands in direction {} for connection {} -> {}.",
                     count, direction, occupant, otherPole);
        }
        RegisterConnectionWidths(occupant, entry, appearance);
    }

    // ------------------------------------------------------------------
    // Foundation pad size. CreateFloor always builds the vanilla 5.0 half-extent quad; we then
    // rescale its 4 corners in place around the pole's XZ position (leaving Y, color, and UV
    // untouched). CreateWalls runs unmodified afterward and reads this same buffer (FUN_0064c2c0
    // indexes occupant+0xe8 directly), so a 0 half-extent collapses floor AND walls to a degenerate
    // point with no separate texture-ID risk -- the safe way to get "no visible foundation".
    // ------------------------------------------------------------------
    void RescaleFoundationFloor(void* occupant, const float halfExtent) {
        const float scale = halfExtent / kVanillaFoundationHalfExtent;
        if (scale == 1.0f) {
            return;
        }
        const auto* const position = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_PosX);
        const float posX = position[0];
        const float posZ = position[2]; // kOccupant_PosZ - kOccupant_PosX spans posY in between

        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_FloorVertsBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_FloorVertsEnd);
        for (uint8_t* vertex = begin; vertex + kFloorVertexStride <= end; vertex += kFloorVertexStride) {
            auto* const xyz = reinterpret_cast<float*>(vertex);
            xyz[0] = posX + (xyz[0] - posX) * scale;
            xyz[2] = posZ + (xyz[2] - posZ) * scale;
        }
    }

    // ------------------------------------------------------------------
    // Foundation texture IDs. Draw() looks up the floor/wall texture via a hash-bucket chain walk
    // keyed by a hardcoded 32-bit constant read twice per texture (see the 4 patch-site constants
    // above); returning a different value per call desyncs the bucket index from the comparison, so
    // both hook instances for a given texture must return the same result -- resolved once here per
    // occupant lookup, called from 2 independent patch sites each.
    // ------------------------------------------------------------------
    uint32_t __cdecl GetFloorTextureHashOverride(void* occupant) noexcept {
        const auto it = gOverrides.find(occupant);
        return (it != gOverrides.end() && it->second.hasFloorTexture) ? it->second.foundationFloorTextureId
                                                                       : kVanillaFloorTextureId;
    }

    uint32_t __cdecl GetWallTextureHashOverride(void* occupant) noexcept {
        const auto it = gOverrides.find(occupant);
        return (it != gOverrides.end() && it->second.hasWallTexture) ? it->second.foundationWallTextureId
                                                                      : kVanillaWallTextureId;
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    // Both patch sites replace `MOV EAX, imm32` (5 bytes) with `CALL rel32` (5 bytes) -- a 1:1 size
    // swap, so no surrounding instructions move. The occupant pointer is live in EBX at every one of
    // the 4 sites (confirmed via disassembly: EBX is read again later at 0x0064ca36/+0x38 in Draw's
    // own body, so it cannot have been repurposed by the compiler in between). ECX/EDX must come back
    // unchanged -- EDX in particular is 0 going into the immediately-following DIV and must stay that
    // way, which a plain push/pop pair guarantees regardless of what our __cdecl callee does to it.
    __declspec(naked) void FloorTextureHashHook() {
        __asm {
            push ecx
            push edx
            push ebx
            call GetFloorTextureHashOverride
            add esp, 4
            pop edx
            pop ecx
            ret
        }
    }

    __declspec(naked) void WallTextureHashHook() {
        __asm {
            push ecx
            push edx
            push ebx
            call GetWallTextureHashOverride
            add esp, 4
            pop edx
            pop ecx
            ret
        }
    }
#else
#error Power-pole foundation-texture hook requires 32-bit MSVC.
#endif

    // Generic 5-byte `MOV EAX, imm32` -> `CALL rel32` patch, one instance per hash-lookup site. Kept
    // separate from InlineCallPatch (used for the 23-byte wire-width site) since the byte count and
    // expected-opcode check differ.
    class Imm32CallPatch final {
    public:
        bool Install(const uintptr_t site, const uint32_t expectedImm, void* hookFn, const char* name) {
            site_ = site;
            auto* const p = reinterpret_cast<uint8_t*>(site);
            const std::array<uint8_t, 5> expected = {
                0xB8,
                static_cast<uint8_t>(expectedImm), static_cast<uint8_t>(expectedImm >> 8),
                static_cast<uint8_t>(expectedImm >> 16), static_cast<uint8_t>(expectedImm >> 24),
            };
            if (std::memcmp(p, expected.data(), expected.size()) != 0) {
                LOG_ERROR("PowerPoleCustomization: {} bytes at 0x{:08X} do not match 1.1.641; not patching.",
                          name, static_cast<uint32_t>(site));
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for {} (error {}).", name, GetLastError());
                return false;
            }
            std::memcpy(original_.data(), p, original_.size());
            const auto relative = static_cast<int32_t>(
                reinterpret_cast<intptr_t>(hookFn) - (static_cast<intptr_t>(site) + 5));
            p[0] = 0xe8;
            std::memcpy(p + 1, &relative, sizeof(relative));
            FlushInstructionCache(GetCurrentProcess(), p, 5);
            DWORD ignored = 0;
            VirtualProtect(p, 5, oldProtect, &ignored);
            installed_ = true;
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const p = reinterpret_cast<uint8_t*>(site_);
            DWORD oldProtect = 0;
            if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                std::memcpy(p, original_.data(), original_.size());
                FlushInstructionCache(GetCurrentProcess(), p, 5);
                DWORD ignored = 0;
                VirtualProtect(p, 5, oldProtect, &ignored);
            }
            installed_ = false;
        }

        [[nodiscard]] bool IsInstalled() const noexcept { return installed_; }

    private:
        uintptr_t site_ = 0;
        std::array<uint8_t, 5> original_{};
        bool installed_ = false;
    };

    // ------------------------------------------------------------------
    // Hook bodies.
    // ------------------------------------------------------------------
    using InitConnectionPointsFn = void(__thiscall*)(void* occupant);
    using AddConnectionFn = void(__thiscall*)(void* occupant, void* otherPole, void* lineInfoVector);
    using UpdateConnectionFn = void(__thiscall*)(void* occupant, void* otherPole);
    using CreateFloorFn = void(__fastcall*)(void* occupant, void* /*unused edx*/);

    void __fastcall CreateFloorHook(void* occupant, void* edxUnused) {
        const auto original = reinterpret_cast<CreateFloorFn>(kCreateFloor);
        original(occupant, edxUnused);
        if (!gSettings.enabled) {
            return;
        }
        const auto it = gOverrides.find(occupant);
        if (it != gOverrides.end() && it->second.hasFoundationHalfExtent) {
            RescaleFoundationFloor(occupant, it->second.foundationHalfExtent);
        }
    }

    void __fastcall InitConnectionPointsHook(void* occupant, void* /*edx*/) {
        const auto original = reinterpret_cast<InitConnectionPointsFn>(kInitConnectionPoints);
        original(occupant); // always run vanilla first: it still owns model load + default-table selection
        ForgetOccupantWidths(occupant);

        if (!gSettings.enabled) {
            gOverrides.erase(occupant);
            return;
        }

        // TEMPORARY PoC-ONLY: bypasses exemplar data so geometry/count rebuilding can still be
        // tested with vanilla assets. Delete once the production path has completed its in-game run.
        if (gSyntheticTestModeEnabled) {
            gOverrides[occupant] = BuildSyntheticTestOverride(occupant);
            LOG_INFO("PowerPoleCustomization: [TEST MODE] injected {}-point synthetic override for pole {}.",
                     kSyntheticTestPointCount, occupant);
            return;
        }

        // cSC4PowerPoleOccupant::QueryInterface delegates through cSC4Occupant to
        // cSCExemplarPropertyHolder. Windows 1.1.641 confirms IID 0x0AC2B5F7 returns the adjusted
        // public cISCExemplarPropertyHolder interface whose +0x10 slot is GetDefaultExemplar().
        cRZAutoRefCount<cISCExemplarPropertyHolder> propertyHolder;
        cISCResExemplar* exemplar = nullptr;
        auto* const unknown = reinterpret_cast<cIGZUnknown*>(occupant);
        if (unknown->QueryInterface(GZIID_cISCExemplarPropertyHolder, propertyHolder.AsPPVoid())) {
            exemplar = propertyHolder->GetDefaultExemplar(); // borrowed; holder keeps it alive here
        } else {
            LOG_WARN("PowerPoleCustomization: pole {} did not expose cISCExemplarPropertyHolder.", occupant);
        }

        PoleAttachOverride candidate;
        if (TryBuildOverride(exemplar, candidate)) {
            gOverrides[occupant] = std::move(candidate);
        } else {
            gOverrides.erase(occupant);
        }
    }

    void __fastcall AddConnectionHook(void* occupant, void* /*edx*/, void* otherPole, void* lineInfoVector) {
        if (gSettings.enabled) {
            ForgetConnectionWidths(FindConnectionEntry(occupant, otherPole));
        }
        const auto original = reinterpret_cast<AddConnectionFn>(kAddConnection);
        original(occupant, otherPole, lineInfoVector);

        if (!gSettings.enabled) {
            return;
        }

        ApplyConnectionCustomization(occupant, otherPole);
    }

    void __fastcall UpdateConnectionHook(void* occupant, void* /*edx*/, void* otherPole) {
        if (gSettings.enabled) {
            ForgetConnectionWidths(FindConnectionEntry(occupant, otherPole));
        }
        const auto original = reinterpret_cast<UpdateConnectionFn>(kUpdateConnection);
        original(occupant, otherPole);
        if (gSettings.enabled) {
            ApplyConnectionCustomization(occupant, otherPole);
        }
    }

    // ------------------------------------------------------------------
    // Settings (SC4PowerPoleCustomization.ini, same layout convention as SC4TerrainDiagonalFix.ini).
    // ------------------------------------------------------------------
    bool ParseBool(const std::string& value, const bool defaultValue) {
        std::string text(value);
        std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
        return defaultValue;
    }

    uint32_t ParseUInt32(const std::string& value, const uint32_t defaultValue) {
        uint32_t result = defaultValue;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        return (ec == std::errc() && ptr == value.data() + value.size()) ? result : defaultValue;
    }

    std::filesystem::path GetDllDirectoryPath() {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetDllDirectoryPath), &module)) {
            return {};
        }
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
            return {};
        }
        return std::filesystem::path(path).parent_path();
    }

    void LoadSettings() {
        gSettings = {};
        const auto settingsPath = GetDllDirectoryPath() / "SC4PowerPoleCustomization.ini";
        mINI::INIFile file(settingsPath.string());
        mINI::INIStructure ini;
        if (!file.read(ini) || !ini.has("SC4PowerPoleCustomization")) {
            LOG_INFO("PowerPoleCustomization: using default settings; no readable {}.", settingsPath.string());
            return;
        }

        const auto section = ini.get("SC4PowerPoleCustomization");
        if (section.has("Enabled")) {
            gSettings.enabled = ParseBool(section.get("Enabled"), gSettings.enabled);
        }
        if (section.has("MaxPointsPerDirection")) {
            gSettings.maxPointsPerDirection = std::clamp(
                ParseUInt32(section.get("MaxPointsPerDirection"), gSettings.maxPointsPerDirection), 1u, 15u);
        }

        LOG_INFO("PowerPoleCustomization: settings enabled={}, maxPointsPerDirection={}",
                 gSettings.enabled, gSettings.maxPointsPerDirection);
    }
}

class PowerPoleCustomizationDirector final : public cRZMessage2COMDirector {
public:
    PowerPoleCustomizationDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZMessage2COMDirector::OnStart(pCOM);
        Logger::Initialize("SC4PowerPoleCustomization", "");
        LoadSettings();
        mpFrameWork->AddHook(this);
        return true;
    }

    bool PostAppInit() override {
        const uint16_t version = VersionDetection::GetInstance().GetGameVersion();
        if (version != kSupportedGameVersion) {
            LOG_WARN("PowerPoleCustomization: game version {} unsupported (addresses target {}); not patching.",
                     version, kSupportedGameVersion);
            return true;
        }

        if (!gSettings.enabled) {
            LOG_INFO("PowerPoleCustomization: disabled via ini; not patching.");
            return true;
        }

        size_t installed = 0;
        installed += InstallCallSitePatch(initConnectionPointsPatches_[0], "InitConnectionPoints@Init",
                                           kInitConnectionPoints_CallSite_Init, kInitConnectionPoints,
                                           &InitConnectionPointsHook);
        installed += InstallCallSitePatch(initConnectionPointsPatches_[1], "InitConnectionPoints@SetDefaultExemplar",
                                           kInitConnectionPoints_CallSite_SetDefaultExemplar, kInitConnectionPoints,
                                           &InitConnectionPointsHook);
        installed += InstallCallSitePatch(initConnectionPointsPatches_[2], "InitConnectionPoints@Read",
                                           kInitConnectionPoints_CallSite_Read, kInitConnectionPoints,
                                           &InitConnectionPointsHook);

        for (size_t i = 0; i < kAddConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(addConnectionPatches_[i], "AddConnection@PlacePoles",
                                               kAddConnectionCallSites[i], kAddConnection, &AddConnectionHook);
        }
        for (size_t i = 0; i < kUpdateConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(updateConnectionPatches_[i], "UpdateConnection@SetDefaultExemplar",
                                               kUpdateConnectionCallSites[i], kUpdateConnection, &UpdateConnectionHook);
        }
        installed += wireWidthLookupPatch_.Install() ? 1 : 0;

        installed += InstallCallSitePatch(createFloorPatches_[0], "CreateFloor@SetPosition",
                                           kCreateFloor_CallSite_SetPosition, kCreateFloor, &CreateFloorHook);
        installed += InstallCallSitePatch(createFloorPatches_[1], "CreateFloor@Draw",
                                           kCreateFloor_CallSite_Draw, kCreateFloor, &CreateFloorHook);
        installed += InstallCallSitePatch(createFloorPatches_[2], "CreateFloor@ChangeZoomLevel",
                                           kCreateFloor_CallSite_ChangeZoomLevel, kCreateFloor, &CreateFloorHook);
        installed += floorTextureHashPatch1_.Install(kFloorTextureHashSite1, kVanillaFloorTextureId,
                                                       &FloorTextureHashHook, "FloorTextureHash@1") ? 1 : 0;
        installed += floorTextureHashPatch2_.Install(kFloorTextureHashSite2, kVanillaFloorTextureId,
                                                       &FloorTextureHashHook, "FloorTextureHash@2") ? 1 : 0;
        installed += wallTextureHashPatch1_.Install(kWallTextureHashSite1, kVanillaWallTextureId,
                                                      &WallTextureHashHook, "WallTextureHash@1") ? 1 : 0;
        installed += wallTextureHashPatch2_.Install(kWallTextureHashSite2, kVanillaWallTextureId,
                                                      &WallTextureHashHook, "WallTextureHash@2") ? 1 : 0;

        LOG_INFO("PowerPoleCustomization: installed {} of {} patches.", installed,
                 initConnectionPointsPatches_.size() + kAddConnectionCallSites.size() +
                 kUpdateConnectionCallSites.size() + 1 + createFloorPatches_.size() + 4);

        // TEMPORARY PoC-ONLY cheat. See the block comment above kCheatPoleLineTest.
        const cISC4AppPtr app;
        if (app) {
            if (auto* const cheats = app->GetCheatCodeManager()) {
                if (cheats->RegisterCheatCode(kCheatPoleLineTest, cRZBaseString("polelinetest"))) {
                    cheats->AddNotification2(this, 0);
                    cheatManager_ = cheats;
                    LOG_INFO("PowerPoleCustomization: [TEST MODE] cheat code registered (polelinetest) "
                             "-- toggles synthetic {}-point overrides for newly-placed/reloaded poles.",
                             kSyntheticTestPointCount);
                } else {
                    LOG_WARN("PowerPoleCustomization: failed to register cheat code polelinetest.");
                }
            }
        }

        return true;
    }

    bool PostAppShutdown() override {
        if (cheatManager_) {
            cheatManager_->UnregisterCheatCode(kCheatPoleLineTest);
            cheatManager_->RemoveNotification2(this, 0);
            cheatManager_.Reset();
        }
        for (auto& patch : initConnectionPointsPatches_) patch.Uninstall();
        for (auto& patch : addConnectionPatches_) patch.Uninstall();
        for (auto& patch : updateConnectionPatches_) patch.Uninstall();
        wireWidthLookupPatch_.Uninstall();
        for (auto& patch : createFloorPatches_) patch.Uninstall();
        floorTextureHashPatch1_.Uninstall();
        floorTextureHashPatch2_.Uninstall();
        wallTextureHashPatch1_.Uninstall();
        wallTextureHashPatch2_.Uninstall();
        gPolylineWidthOverrides.clear();
        gOverrides.clear();
        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }
        return true;
    }

    bool DoMessage(cIGZMessage2* pMsg) override {
        if (pMsg) {
            auto* const stdMsg = static_cast<cIGZMessage2Standard*>(pMsg);
            if (stdMsg->GetType() == kCheatCodeMessageType &&
                static_cast<uint32_t>(stdMsg->GetData1()) == kCheatPoleLineTest) {
                gSyntheticTestModeEnabled = !gSyntheticTestModeEnabled;
                LOG_INFO("PowerPoleCustomization: [TEST MODE] {} -- place or reload a power pole now "
                         "to see it take effect (only applies at InitConnectionPoints time).",
                         gSyntheticTestModeEnabled ? "ENABLED" : "disabled");
            }
        }
        return true;
    }

private:
    static size_t InstallCallSitePatch(TerrainDecal::RelativeCallPatch& patch, const char* name,
                                        const uintptr_t callSite, const uintptr_t expectedTarget, void* hookFn) {
        patch.Configure(name, callSite, hookFn);
        if (!patch.Install()) {
            LOG_ERROR("PowerPoleCustomization: failed to install {} at 0x{:08X}.", name, static_cast<uint32_t>(callSite));
            return 0;
        }
        if (patch.GetOriginalTarget() != expectedTarget) {
            LOG_ERROR("PowerPoleCustomization: {} call site 0x{:08X} targets 0x{:08X}, expected 0x{:08X}; reverting.",
                       name, static_cast<uint32_t>(callSite), static_cast<uint32_t>(patch.GetOriginalTarget()),
                       static_cast<uint32_t>(expectedTarget));
            patch.Uninstall();
            return 0;
        }
        return 1;
    }

    std::array<TerrainDecal::RelativeCallPatch, 3> initConnectionPointsPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 4> addConnectionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 2> updateConnectionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 3> createFloorPatches_{};
    Imm32CallPatch floorTextureHashPatch1_{};
    Imm32CallPatch floorTextureHashPatch2_{};
    Imm32CallPatch wallTextureHashPatch1_{};
    Imm32CallPatch wallTextureHashPatch2_{};
    InlineCallPatch wireWidthLookupPatch_{};
    cRZAutoRefCount<cIGZCheatCodeManager> cheatManager_; // TEMPORARY PoC-ONLY, see kCheatPoleLineTest
};

static PowerPoleCustomizationDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}

// ------------------------------------------------------------------
// Current verification state:
//   1. CLOSED: all three InitConnectionPoints call sites are hooked, including 0x0064ceaa inside
//      cSC4PowerPoleOccupant::Init, the new-pole creation path.
//   2. CLOSED statically, runtime validation pending: public cISCExemplarPropertyHolder access is
//      confirmed through cSC4PowerPoleOccupant::QueryInterface and replaces the old nullptr stub.
//   3. CLOSED: Get0To3Direction confirmed at 0x0061f4c0. ApplyConnectionCustomization computes real
//      direction and strand counts.
//   4. CLOSED (superseded): cSC4PowerLineOccupant's vtable was fully mapped and used to register
//      extra strands as city objects (see docs SS9/SS10). Confirmed in-game (SS13) that this alone
//      doesn't render anything -- removed from the render path. The vtable mapping itself remains
//      documented in docs/sc4-powerline-tool-re.md if city-object/save-load registration is wanted
//      again later; the code for it was deleted from this file (kept unused code out).
//   5. IMPLEMENTED, awaiting in-game validation: full 1-N geometry and sag rebuild in the active
//      tConnection regular-polyline vector, including counts below vanilla's fixed four.
//   6. IMPLEMENTED, awaiting in-game validation: per-wire width lookup at 0x0064b855. The patch is
//      byte-validated before installation and falls back to vanilla width for unregistered wires.
// ------------------------------------------------------------------
