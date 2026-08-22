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
// written out in full, ACTIVE, and validated in-game (2026-07-19) together with per-wire width,
// FAR drag/routing, and the Tab/Shift-Tab style switch.
//
// First attempt (2026-07-01) registered extra strands as cSC4PowerLineOccupant city objects --
// confirmed in-game (docs SS13) to run with no crash but NOT make DrawPowerlines render anything,
// since rendering reads polyline data cached directly in each pole's own tConnection entry,
// independent of any cSC4PowerLineOccupant's existence. The current approach clears and rebuilds
// the complete regular-polyline list in each active tConnection, using the same erase, control-
// point, tessellation, and vector-insert helpers as vanilla. A separate validated inline lookup
// supplies per-wire width while leaving the rest of DrawPowerlines intact.

#include "cGZPersistResourceKey.h"
#include "cIGZCOM.h"
#include "cIGZCheatCodeManager.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZWin.h"
#include "cISC4App.h"
#include "cISC4View3DWin.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cISCExemplarPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cIGZVariant.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cRZMessage2COMDirector.h"
#include "GZServPtrs.h"

// Optional ImGui status overlay. imgui.dll is delay-loaded (see this target's CMake), so this DLL
// still loads and every pole feature keeps working when SC4RenderServices / imgui.dll is absent --
// the overlay simply never registers. Nothing here may call an ImGui:: symbol unless the ImGui
// system service was successfully acquired.
#include "public/cIGZImGuiService.h"
#include "public/ImGuiServiceIds.h"
#include "imgui.h"

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
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    constexpr uint32_t kDirectorID = 0xB07E11E5; // arbitrary, must not collide with another director in this Plugins folder
    constexpr uint16_t kSupportedGameVersion = 641;
    constexpr uint32_t kCheatCodeMessageType = 0x230E27AC; // fixed SC4 message type, same constant every sample uses
    constexpr uint32_t kSC4MessagePostCityInit = 0x26D31EC1; // emitted after all saved occupants are loaded
    constexpr uint32_t kSC4MessagePreCityShutdown = 0x26D31EC2; // same constant DateJumper/RenderServices use

    // Experimental, deliberately runtime-scoped switch: while enabled, the lot developer's
    // ClearLotBlockingObjects loop leaves power-pole and hidden power-line occupants alone.
    constexpr uint32_t kCheatKeepWires = 0xB07E1002;
    bool gKeepWiresOnZoneChange = false;
    constexpr uint32_t kExemplarTypeId = 0x6534284A;
    constexpr uint32_t kVanillaPowerPoleGroupId = 0x088E1962;

    // No exception may unwind through a SimCity call frame. Hook bodies catch customization-side
    // failures and use this best-effort logger (which is itself noexcept even if formatting fails).
    void LogCurrentHookException(const char* const hookName) noexcept {
        try {
            throw;
        } catch (const std::exception& ex) {
            try {
                LOG_ERROR("PowerPoleCustomization: {} customization failed: {}", hookName, ex.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR("PowerPoleCustomization: {} customization failed with an unknown exception.",
                          hookName);
            } catch (...) {
            }
        }
    }

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

    // cSC4PowerPoleOccupant::~cSC4PowerPoleOccupant (complete-object destructor, confirmed
    // 2026-07-19: same 7-vtable reset pattern as the Mac dtor at 0x002594ec, destroys the
    // tConnection vector at +0xac/+0xb0, calls the cSC4Occupant base dtor on +0x8). Its sole
    // caller is the scalar deleting destructor at 0x0064e380, which then returns the 292-byte
    // object to the fixed pool at 0x00b0c0c8. Hooking that one call site lets this DLL drop its
    // side-table entries for a pole the moment the engine destroys it -- without this, demolished
    // poles leave stale gOverrides/gPolylineWidthOverrides entries behind and the pool's address
    // reuse can briefly pair a new pole with a dead pole's customization.
    constexpr uintptr_t kDestructor = 0x0064dfc0;
    constexpr uintptr_t kDestructor_CallSite_DeletingDtor = 0x0064e383;

    // cSC4PowerPoleOccupant::LoadModel passes its mask-derived 0/1 quarter-turn to the RKT1 model
    // selector here. The first argument is the model-resource-key vector at completeObject+0x8,
    // which lets the hook recover the pole and apply an exemplar-controlled XOR correction before
    // the selector folds camera rotation and pole rotation modulo four.
    constexpr uintptr_t kGetModelInstanceID = 0x00497180;
    constexpr uintptr_t kGetModelInstanceID_CallSite_LoadModel = 0x0064ae08;
    constexpr uint32_t kOccupant_ModelResourceKeys = 0x08;

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

    // cSC4PowerPoleOccupant::RemoveConnection. All six direct Windows callers were confirmed with
    // the paired Ghidra projects: BreakAllConnections, two PlacePoles branches, two zone-change
    // branches, and the terrain-height update path. Removal compacts the 0x2c-byte tConnection
    // vector, invalidating every address-keyed side-table entry at and after the erased element.
    constexpr uintptr_t kRemoveConnection = 0x0064da00;
    constexpr std::array<uintptr_t, 6> kRemoveConnectionCallSites = {
        0x0064db09, 0x006518df, 0x00651a27, 0x0065279f, 0x006527a9, 0x006ee04a,
    };
    // BreakAllConnections removes the reciprocal side through RemoveConnection above, but clears
    // its own non-erased entry through this lower-level helper. Hook that one call too so teardown
    // that does not immediately destroy the pole cannot leave stale point-buffer registrations.
    constexpr uintptr_t kRemoveConnectionEntry = 0x0064a560;
    constexpr uintptr_t kRemoveConnectionEntry_CallSite_BreakAll = 0x0064db12;

    // ------------------------------------------------------------------
    // cSC4PowerPoleOccupant field offsets (Windows). See docs/sc4-powerline-tool-re.md SS9 --
    // only offsets independently confirmed by a real call site, not guessed by Mac-offset-by-analogy.
    // ------------------------------------------------------------------
    constexpr uint32_t kOccupant_ConnectionPointTablePtr = 0xe4; // InitConnectionPoints writes here
    constexpr uint32_t kOccupant_ConnectionsBegin = 0xac;        // confirmed via DrawPowerlines (clean decompile)
    constexpr uint32_t kOccupant_ConnectionsEnd = 0xb0;
    constexpr uint32_t kOccupant_DirectionFlags = 0xb8;          // confirmed via CreatePowerPole
    constexpr uintptr_t kCardinalConnectionPoints = 0x00B0C0D8;
    constexpr uintptr_t kDiagonalConnectionPoints = 0x00B0C198;
    constexpr uint32_t kCardinalDirectionFlags = 0x5; // X | Z
    constexpr uint32_t kDiagonalDirectionFlags = 0xa; // DP | DN
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
    // pole pointer and +0x09 is the render-owning flag. AddConnection initializes +0x08 to zero, so
    // it is not a live marker; inner RemoveConnection at 0x0064a560 nulls +0x00 when disconnecting.
    // +0xc/+0x10 are cell coordinates, not pole pointers.
    // ------------------------------------------------------------------
    constexpr uint32_t kConnection_OtherPole = 0x00;
    constexpr uint32_t kConnection_IsRenderOwner = 0x09;
    constexpr uint32_t kConnection_PolylinesBegin = 0x14; // vector<vector<cS3DVector3>>
    constexpr uint32_t kConnection_PolylinesEnd = 0x18;
    // Second vector<vector<cS3DVector3>> (confirmed 2026-07-19 via AddConnection's decompile and
    // DrawPowerlines' reads at [entry+0x20]/[entry+0x24]): coarse 4-point copies of each strand,
    // built by AddConnection ONLY when the span crosses water (terrain service vtbl+0x88 test) and
    // drawn with the 0xC9AF0FCD texture. Same element layout and the same insert/erase helpers as
    // the regular list, with a fixed tessellation count of 3 segments after the start point.
    constexpr uint32_t kConnection_WaterPolylinesBegin = 0x20;
    constexpr uint32_t kConnection_WaterPolylinesEnd = 0x24;
    constexpr int32_t kWaterPolylinePointCount = 4; // start + 3 tessellated, matching vanilla
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

    // Complete vector<vector<cS3DVector3>> destructor. Windows RemoveConnection calls this exact
    // helper for both tConnection lists at 0x0064da89/0x0064da91. It destroys every inner point
    // vector and frees the outer allocation, which lets us build engine-owned temporary lists and
    // commit them with a no-throw three-pointer swap.
    constexpr uintptr_t kPolylineListDestructor = 0x004638b0;
    using PolylineListDestructorFn = void(__fastcall*)(void* polylinesVector);

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
    // Custom exemplar property IDs. The original 0xB22A0000-0xB22A000F block was checked against
    // the vendored new_properties.xml registry on 2026-07-01 and the later IDs through 0xB22A0013
    // were checked as they were added; none is used by that registry. Unregistered third-party
    // properties can never be ruled out globally, but these IDs have no known registry conflict
    // and are intentionally kept contiguous for this feature.
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
    // Optional signed Float32, in degrees: the in-plane heading (0 = x axis, 90 = z axis) the
    // custom attach points in this exemplar were authored for. Absent = attach points are in their
    // direction's nominal 0/45/90/135-degree basis (vanilla behavior). Only meaningful together with
    // custom attach-point properties; a pole using the vanilla baked table always uses the nominal
    // basis regardless of this value. See docs/far-power-lines-design.md "Two real wrinkles".
    constexpr uint32_t kPropAttachBasisDegrees = 0xB22A000D;
    // Optional Bool. XORs the mask-derived 0/1 RKT1 quarter-turn. This is useful when two
    // perpendicular FAR headings share one logical model but SC4 rotates the wrong member of the
    // pair. Absent/false preserves vanilla model selection exactly.
    constexpr uint32_t kPropInvertModelQuarterTurn = 0xB22A000E;
    // Optional Uint32 texture IDs replacing the wire strand texture (vanilla 0xAA9DA78E) and the
    // buoy/marker-ball texture (vanilla 0xC9AF0FCD) on every connection this pole render-owns.
    // Validated and engine-registered exactly like the foundation textures (FSH group 0x1ABE787D).
    constexpr uint32_t kPropWireTextureId = 0xB22A000F;
    constexpr uint32_t kPropWireBuoyTextureId = 0xB22A0010;
    // Optional Bool. Rotates the foundation pad (floor + walls) in the ground plane so it follows
    // the pole's span bearing instead of staying axis-aligned -- the rotation is the minimal
    // deviation from the nearest 90-degree multiple, so walls keep their vanilla sides. Applied
    // only when every live connection of the pole agrees on one bearing (junctions stay vanilla).
    constexpr uint32_t kPropFoundationFollowSpan = 0xB22A0011;
    // Optional non-negative Float32: scales the water-crossing buoy/ball quads (1.0 = vanilla).
    constexpr uint32_t kPropWaterBuoySizeScale = 0xB22A0012;
    // Optional Uint32: points per water-crossing strand polyline; the engine draws points-1 balls,
    // so vanilla's 4 points = 3 balls. Clamped to [2, 17] (1-16 balls).
    constexpr uint32_t kPropWaterBuoyPointCount = 0xB22A0013;
    // Model-local coordinates beyond this are not meaningful in an SC4 city and can overflow the
    // engine's float Bezier arithmetic even though each source value is individually finite.
    constexpr float kMaxAttachCoordinateMagnitude = 1'000'000.0f;
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
    // cSC4PowerPoleOccupant::CreateWalls -- __thiscall, no args, called immediately after
    // CreateFloor at every vanilla site (confirmed at SetPosition 0x0064c616). It rebuilds the two
    // wall quads from the floor's own vertex buffer, so any floor transform applied before it runs
    // (scale AND rotation) carries into the walls with no separate wall patch.
    constexpr uintptr_t kCreateWalls = 0x0064c470;
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

    // Engine power-pole texture registry (confirmed 2026-07-19 on the WINDOWS binary via StaticInit
    // 0x0064cc20 / ChangeZoomLevel 0x0064c6c0 / Draw 0x0064c800 -- the container layout below is
    // read from Windows disassembly, NOT assumed from the Mac binary's gnu hashtable): a
    // hash_map<textureId, cS3DTextureBinding*> rooted at 0x00b46784 with the bucket-pointer array
    // at [0x00b46788]..[0x00b4678c], node layout {+0 next, +4 id, +8 binding}, bucket index =
    // id % bucketCount. StaticInit seeds it with exactly the four vanilla wire/floor/wall IDs.
    // Draw's bucket walk has NO null check, so any ID absent from this map crashes -- a custom ID
    // must therefore (a) resolve to a real FSH resource and (b) be inserted into this map before
    // the first Draw that uses it. Insertion goes through the engine's own map insert function
    // (0x004f8d30, __thiscall, arg = pointer to the 32-bit key, returns the value slot -- the exact
    // call StaticInit itself makes four times), then is verified by re-finding the node with the
    // same read-only bucket walk Draw performs; verification failure falls back to the vanilla
    // texture instead of trusting the insert. ChangeZoomLevel reloads the binding of every
    // registered entry whenever the zoom global at 0x00b0c0a8 differs from the draw context's
    // zoom, so storing an impossible zoom right after inserting forces the engine itself to load
    // the new texture on the next frame -- no manual texture-manager call needed.
    constexpr uintptr_t kPoleTextureRegistry = 0x00b46784;
    constexpr uintptr_t kPoleTextureRegistryInsert = 0x004f8d30;
    constexpr uintptr_t kPoleTextureBucketsBegin = 0x00b46788;
    constexpr uintptr_t kPoleTextureBucketsEnd = 0x00b4678c;
    constexpr uintptr_t kPoleTextureZoomGlobal = 0x00b0c0a8;
    constexpr uint32_t kFshTypeId = 0x7ab50e44;
    constexpr uint32_t kPoleTextureGroupId = 0x1abe787d; // group ChangeZoomLevel loads bindings from

    // Wire texture lookup sites inside DrawPowerlines (0x0064b750). Each is a single 5-byte
    // `MOV ESI, imm32` (opcode 0xBE) whose value feeds BOTH the bucket DIV and the chain-walk
    // compare of the texture-registry walk -- unlike Draw's floor/wall pair-sites, one patch per
    // texture suffices. At both sites EBP holds the current tConnection entry (confirmed: the
    // preceding instructions read the render-owner flag at [EBP+0x9] and the buoy vector at
    // [EBP+0x20]/[EBP+0x24]), which is exactly the key the DLL's connection-texture map uses.
    // 0xAA9DA78E textures the strand polylines; 0xC9AF0FCD textures the buoy/marker balls.
    constexpr uintptr_t kWireStrandTextureSite = 0x0064b7ba;
    constexpr uintptr_t kWireBuoyTextureSite = 0x0064b9e3;
    constexpr uint32_t kVanillaWireStrandTextureId = 0xAA9DA78E;
    constexpr uint32_t kVanillaWireBuoyTextureId = 0xC9AF0FCD;
    constexpr uint8_t kMovEaxOpcode = 0xB8;
    constexpr uint8_t kMovEsiOpcode = 0xBE;

    // Water-crossing buoy/ball rendering inside DrawPowerlines (confirmed 2026-07-19): one
    // billboarded quad is drawn per INTERIOR point of each water polyline (points-1 balls per
    // strand), with half-size g_afPowerLineWidthFactors[zoom] * 4.0f (the constant at 0x00a8e17c).
    // The zoom-table read at 0x0064ba6e -- `MOV EAX,[0x00b46798]; FLD [EAX+EDX*4];
    // LEA EAX,[EAX+EDX*4]`, 11 bytes -- is the single chokepoint: everything after re-reads the
    // size through EAX. At that instruction EDX holds the zoom and EDI the current inner polyline
    // vector, so the hook can key a per-polyline-buffer override exactly like the wire-width hook
    // and return a pointer to either the vanilla slot or a pre-scaled value.
    constexpr uintptr_t kWaterBallSizeSite = 0x0064ba6e;
    constexpr uint32_t kMinWaterPolylinePoints = 2;  // 1 ball per strand
    constexpr uint32_t kMaxWaterPolylinePoints = 17; // 16 balls per strand

    // ------------------------------------------------------------------
    // Multiple pole "styles" (docs/sc4-powerline-tool-re.md SS "B. Multiple pole types/families").
    // Data-model + style-switching resolver. Style switching is driven in-game by the Tab/Shift-Tab
    // OnKeyDown hook (see OnKeyDownHook / CycleStyle), gated on the power tool being active.
    //
    // cSC4PowerLineTool::CreatePowerPole (Windows 0x00650140) reads the pole instance for a
    // direction mask from the vanilla global g_dwPowerPoleForDirectionsFlag[16] (0x00B467B0,
    // confirmed docs SS7/SS8) at exactly 2 sites -- merge-branch and new-pole branch, both the
    // identical 7-byte `MOV EAX,[EDI*4+0xB467B0]` (confirmed via raw memory read, not just
    // disassembly text). Both patched to call a resolver that passes through to the vanilla array
    // when no custom style is active (index 0), or looks up the active style's table otherwise,
    // falling back to vanilla per-entry for any direction mask a sparse custom section doesn't
    // define. EDI (the direction mask) is untouched by the hook -- it's read again immediately after
    // both call sites (OR'd into the connection's stored direction flags), so must survive.
    // ------------------------------------------------------------------
    constexpr uintptr_t kPowerPoleForDirectionsFlag = 0x00B467B0; // vanilla 16xuint32 table
    constexpr uintptr_t kPoleStyleLookupSite1 = 0x00650177; // merge-branch (existing pole at cell)
    constexpr uintptr_t kPoleStyleLookupSite2 = 0x006501cd; // new-pole branch

    // ------------------------------------------------------------------
    // Per-style inter-pole distance. cSC4PowerLineTool::DeterminePolePositions (Windows 0x00650840,
    // docs/sc4-powerline-tool-re.md SS16) forces a new pole once the current drag step exceeds the
    // tool's "Max cells between power poles" field, read at `*(tool+0x3a4) <= (curStep - lastPoleStep)`.
    // Vanilla loads that field once from the Utilities exemplar (property 0x098B25C8 = 10, docs SS2);
    // it's a plain tool-instance member, and the placement loop is its only reader. The hook saves the
    // field, writes the active style's value, runs vanilla, then restores -- fully scoped, so no
    // persistent per-tool cache is needed. Called from 3 sites (preview, alt-preview, commit/PlacePoles).
    // ------------------------------------------------------------------
    constexpr uintptr_t kDeterminePolePositions = 0x00650840;
    constexpr std::array<uintptr_t, 3> kDeterminePolePositionsCallSites = {
        0x00652063, 0x006522bf, 0x006529c2,
    };
    constexpr uint32_t kTool_MaxCellsBetweenPoles = 0x3a4; // cSC4PowerLineTool instance field
    constexpr uint32_t kMaxInterPoleDistance = 50; // sanity clamp; vanilla is 10

    // cSC4PowerLineTool::PlacePoles has exactly three Windows direct callers (Ghidra xrefs). The
    // call at 0x0065206c is recursive inside PlacePoles' connection loop, so clearing there would
    // break later iterations of the outer pass. Only the two external callers are transaction ends.
    constexpr uintptr_t kPlacePoles = 0x00651560;
    constexpr std::array<uintptr_t, 2> kPlacePolesTransactionCallSites = {
        0x006522c8, 0x006529cb,
    };

    // ------------------------------------------------------------------
    // FAR (fractional-angle) power-line dragging -- validated in-game 2026-07-19. See
    // docs/sc4-powerline-tool-re.md SS17 for the complete evidence trail.
    //
    // cSC4NetworkTool::DrawNetworkLine (0x0063af40) is virtual (slot +0x48) and has NO direct call
    // sites -- every network tool dispatches it through its own vtable. cSC4PowerLineTool's ctor
    // (0x006503c0) installs primary vtable 0x00aa9f30, whose DrawNetworkLine slot is the dword at
    // 0x00aa9f78. Swapping that one dword hooks the power tool ONLY -- road/rail/street vtables
    // keep their own untouched slots, so no runtime tool-identity check is needed.
    //
    // The hook, when a FAR ratio is active and the drag is not axis/45-aligned, snaps the cursor
    // to the nearest whole number of FAR periods, then synthesizes the dragged-step/cell arrays:
    // one step per FAR period containing that period's complete supercover tile range, plus a final
    // single-cell step for the last node, with this+0x74 == this+0x70 (all steps "straight", so
    // GetPrimaryCell returns each step's first cell). The dense cell ranges make vanilla's blue
    // terrain preview continuous while DeterminePolePositionsHook scopes
    // this+0x3a4 = 1 for the same drag, so vanilla DeterminePolePositions forces a pole at every
    // step's primary cell -- exactly the FAR nodes -- and nothing else (docs SS17.C).
    //
    // Allocation: the game's step/cell vectors use the game allocator, so the hook first runs the
    // ORIGINAL DrawNetworkLine across the complete city diagonal (vanilla performs all allocation),
    // then rewrites the POD contents in place and sets the end pointers. A map diagonal allocates
    // enough cells for every legal in-city 4-connected supercover, unlike the old fake straight
    // drag whose allocation was too small for long FAR paths. Any capacity mismatch falls back.
    // ------------------------------------------------------------------
    constexpr uintptr_t kDrawNetworkLine = 0x0063af40;
    constexpr uintptr_t kPowerToolDrawNetworkLineSlot = 0x00aa9f78; // dword inside vtable 0x00aa9f30
    constexpr uint32_t kTool_StepsBegin = 0x54;   // vector<tDraggedStep> begin ptr (stride 0xc)
    constexpr uint32_t kTool_StepsEnd = 0x58;
    constexpr uint32_t kTool_StepsCapacity = 0x5c;
    constexpr uint32_t kTool_CellsBegin = 0x60;   // vector<SC4Point<uint>> begin ptr (stride 8)
    constexpr uint32_t kTool_CellsEnd = 0x64;
    constexpr uint32_t kTool_CellsCapacity = 0x68;
    constexpr uint32_t kTool_DiagonalFlag = 0x6c; // byte: drag has a diagonal segment
    constexpr uint32_t kTool_TotalSteps = 0x70;
    constexpr uint32_t kTool_StraightSteps = 0x74;
    constexpr uint32_t kTool_FirstDirection = 0x7c;  // cardinal direction used by neighbor connection logic
    constexpr uint32_t kTool_SecondDirection = 0x80;
    constexpr uint32_t kTool_CityCellsX = 0x240;     // city dimensions in cells
    constexpr uint32_t kTool_CityCellsZ = 0x244;

    // cSC4PowerLineTool::UpdateOnZoneChange (Windows twin confirmed by the runtime shutdown stack
    // and Mac symbol/call-shape match). It deliberately removes both sides of every connection
    // crossing the changed zone rectangle at 0x0065279f/0x006527a9, then attempts to reposition
    // and rebuild it. Bypass the complete transaction while keepwires is enabled.
    constexpr uintptr_t kUpdateOnZoneChange = 0x006522f0;
    constexpr uintptr_t kUpdateOnZoneChangeResume = 0x006522f7;

    // ------------------------------------------------------------------
    // Tab / Shift-Tab pole-style switch. Keyboard input for every network tool is dispatched by the
    // shared cSC4ViewInputControlNetworkTool (vtable 0x00aab008, confirmed by decompile). Its
    // cISC4ViewInputControl::OnKeyDown is interface slot 14 = byte 0x38 -> the dword at 0x00aab040
    // holds the OnKeyDown body 0x00661e90. Vanilla OnKeyDown only reacts to Escape (0x1b), so Tab is
    // unclaimed here. Because the vtable is shared across road/rail/street/power, the slot patch is
    // NOT auto-scoped the way the DrawNetworkLine patch was -- it must runtime-gate on the active
    // network type. The input control stores the current network type at +0x50 (fed to
    // SL::NetworkManager in Init); the power line tool is network type 5 (cSC4PowerLineTool ctor
    // calls the cSC4NetworkTool base with 5). That enum -- not a vtable address -- is the runtime
    // discriminator: stable across rebuilds, and verified in-game. (The +0x4c subtool pointer is a
    // shared cSC4NetworkToolUI, NOT the power tool object, so a vtable compare there was wrong.)
    // See docs/power-line-style-ui-design.md.
    constexpr uintptr_t kNetworkToolInputControlVtable = 0x00aab008;
    constexpr uintptr_t kOnKeyDownSlot = 0x00aab040;   // vtable + 0x38 (interface slot 14)
    constexpr uintptr_t kOnKeyDown = 0x00661e90;       // cSC4ViewInputControlNetworkTool::OnKeyDown
    constexpr uintptr_t kInputControlIsOnTop = 0x005fb190; // vanilla OnKeyDown's first focus check
    constexpr uint32_t kIC_NetworkType = 0x50;         // current network type on the input control
    constexpr uint32_t kNetworkTypePower = 5;
    constexpr uint32_t kVkTab = 0x09;
    static_assert(kOnKeyDownSlot == kNetworkToolInputControlVtable + 0x38);

    // View3D acquisition (for overlay visibility only; same window IDs plop-and-paint uses).
    constexpr uint32_t kGZWin_WinSC4App = 0x6104489A;
    constexpr uint32_t kGZWin_SC4View3DWin = 0x9a47b417;

    // ImGui status overlay panel id (this DLL's 0xB07E**** block).
    constexpr uint32_t kStatusPanelId = 0xB07E2000;

    struct FarRatio {
        uint32_t run;  // cells along the major axis per period
        uint32_t rise; // cells along the minor axis per period
    };
    // arctan(rise/run): FAR-1.5 = 33.69, FAR-2 = 26.57, FAR-3 = 18.43, FAR-6 = 9.46 degrees.
    constexpr std::array<FarRatio, 4> kFarRatios = {{
        {3, 2}, {2, 1}, {3, 1}, {6, 1},
    }};

    // A FAR span is undirected, so per ratio exactly 4 distinct pole headings exist: the 1:n slope
    // tilted off the x axis with its minor step in +z (XP) or -z (XN), and the mirrored n:1 pair off
    // the z axis with its minor step in +x (ZP) or -x (ZN). See docs/far-power-lines-design.md.
    // Kept in this order so a style/table index is `ratioIndex * kFarOrientCount + orient`.
    enum FarOrient : uint32_t { kFarOrientXP = 0, kFarOrientXN = 1, kFarOrientZP = 2, kFarOrientZN = 3 };
    constexpr uint32_t kFarOrientCount = 4;
    constexpr uint32_t kFarHeadingCount = static_cast<uint32_t>(kFarRatios.size()) * kFarOrientCount; // 16

    // Human-readable tags for debug logging (order matches kFarRatios / the FarOrient enum).
    constexpr std::array<std::string_view, 4> kFarRatioLabels = {{"1.5", "2", "3", "6"}};
    constexpr std::string_view FarOrientName(const uint32_t orient) {
        switch (orient) {
            case kFarOrientXP: return "XP";
            case kFarOrientXN: return "XN";
            case kFarOrientZP: return "ZP";
            case kFarOrientZN: return "ZN";
            default: return "??";
        }
    }

    bool gFarDragActive = false;      // latest DrawNetworkLine call produced a FAR path
    uint32_t gFarDragRun = 0;         // major-axis cells per period of that FAR path
    uint32_t gFarDragRatioIndex = 0;  // index into kFarRatios for that FAR path
    uint32_t gFarDragOrient = kFarOrientXP; // FarOrient of that FAR path

    // Automatic heading selection considers the two regular headings bounding the first octant
    // (axis and 45-degree diagonal) plus every supported FAR ratio. The remaining octants are
    // reflections of this one. Candidate 0 is the nearest axis, 1..N are kFarRatios, and N+1 is
    // the diagonal. A small angular hysteresis prevents integer cursor movement from flickering
    // between adjacent headings at their exact midpoint during a drag.
    constexpr uint32_t kAxisSnapCandidate = 0;
    constexpr uint32_t kDiagonalSnapCandidate = static_cast<uint32_t>(kFarRatios.size()) + 1;
    constexpr float kDiagonalAngleRadians = 0.78539816339744830962f;
    constexpr float kSnapHysteresisRadians = 0.01745329251994329577f; // 1 degree
    bool gFarSnapAnchorValid = false;
    uint32_t gFarSnapAnchorX = 0;
    uint32_t gFarSnapAnchorZ = 0;
    uint32_t gFarSnapCandidate = kAxisSnapCandidate;

    struct PoleStyle {
        std::string configName;
        std::string name;
        std::array<uint32_t, 16> instanceByDirectionMask{};
        std::array<bool, 16> hasMask{};
        // FAR headings, indexed by `ratioIndex * kFarOrientCount + orient`. Selected during a FAR
        // drag by the exact (ratio, orientation); direction masks cannot express a FAR heading.
        std::array<uint32_t, kFarHeadingCount> farInstanceByHeading{};
        std::array<bool, kFarHeadingCount> hasFarHeading{};
        uint32_t maxCellsBetweenPoles = 0; // 0 = inherit vanilla; else cells between poles
        bool hasMaxCells = false;
    };

    // Index 0 = vanilla (no override). gStyles[gActiveStyleIndex - 1] for gActiveStyleIndex >= 1.
    std::vector<PoleStyle> gStyles;
    uint32_t gActiveStyleIndex = 0;
    uint32_t gLastObservedVanillaInterPoleDistance = 10;

    // The optional [PowerPoles.FAR] section: a style-independent fallback consulted for FAR headings
    // when the active style doesn't define the exact key. Only its farInstanceByHeading is used; it
    // is never a cyclable style and never enters gStyles.
    PoleStyle gFarDefaultStyle;
    bool gHasFarDefault = false;

    // ------------------------------------------------------------------
    // Data-driven settings (SC4PowerPoleCustomization.ini).
    // ------------------------------------------------------------------
    struct Settings {
        bool enabled = true;
        uint32_t maxPointsPerDirection = 8; // raise to allow denser cable bundles than vanilla's 4
    };

    Settings gSettings;
    // Kept separate from the INI setting: hooks are installed before lifecycle subscriptions are
    // proven. If startup rollback cannot reclaim a site, every surviving callback must still take
    // its vanilla path while this DLL remains loaded.
    bool gRuntimeHooksEnabled = false;

    bool RuntimeCustomizationEnabled() noexcept {
        return gRuntimeHooksEnabled && gSettings.enabled;
    }

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
        uint32_t wireTextureId = kVanillaWireStrandTextureId;
        bool hasWireTexture = false;
        uint32_t wireBuoyTextureId = kVanillaWireBuoyTextureId;
        bool hasBuoyTexture = false;
        bool foundationFollowSpan = false;
        float waterBuoySizeScale = 1.0f;
        bool hasWaterBuoySize = false;
        uint32_t waterBuoyPointCount = 0; // 0 = vanilla count
        bool hasWaterBuoyCount = false;
        // In-plane heading (radians, normalized to [0, pi)) the custom attach points were baked for.
        // Only consulted for directions that actually carry custom attach points.
        float attachBasisRadians = 0.0f;
        bool hasAttachBasis = false;

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

    // The Read hook runs while the object stream is still constructing other poles, so rebuilding
    // a saved connection there can observe an endpoint whose exemplar has not been initialized yet.
    // Queue those poles and rehydrate them on kSC4MessagePostCityInit, after the whole city is live.
    std::unordered_set<void*> gPendingLoadedPoles;

    struct PolylineWidthOverride {
        void* owner = nullptr;
        std::array<float, kPowerLineZoomCount> widthByZoom{};
    };

    // Keyed by the inner vector's point-buffer address. Outer vector reallocation deep-copies the
    // inner vectors, so registrations are made only after the full connection rebuild is complete.
    std::unordered_map<const void*, PolylineWidthOverride> gPolylineWidthOverrides;

    // Per-connection wire/buoy texture override, keyed by the tConnection entry address DrawPowerlines
    // holds in EBP at both texture-lookup sites. Entry addresses move when a pole's connections
    // vector reallocates, so RegisterOccupantConnectionTextures() erases-by-owner and re-registers
    // every render-owner entry of the pole after connection additions, updates, removals, and saved-
    // city rehydration. This covers both vector reallocation while growing and entry compaction
    // while shrinking.
    struct ConnectionTextureOverride {
        void* owner = nullptr;
        uint32_t strandTextureId = kVanillaWireStrandTextureId;
        uint32_t buoyTextureId = kVanillaWireBuoyTextureId;
    };
    std::unordered_map<const void*, ConnectionTextureOverride> gConnectionTextureOverrides;

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
        if (values == nullptr) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} returned a null Float32 array; "
                     "ignoring.", propertyId);
            return false;
        }
        uint32_t firstCoordinate = 0;
        uint32_t availableCount = 0;
        uint32_t requestedCount = 0;
        if (valueCount % 3 == 0) {
            availableCount = valueCount / 3;
            requestedCount = availableCount;
        } else if (valueCount >= 4 && (valueCount - 1) % 3 == 0 && std::isfinite(values[0]) &&
                   values[0] >= 1.0f && std::floor(values[0]) == values[0] &&
                   static_cast<double>(values[0]) <=
                       static_cast<double>(std::numeric_limits<uint32_t>::max())) {
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
            if (std::fabs(xyz[0]) > kMaxAttachCoordinateMagnitude ||
                std::fabs(xyz[1]) > kMaxAttachCoordinateMagnitude ||
                std::fabs(xyz[2]) > kMaxAttachCoordinateMagnitude) {
                LOG_WARN("PowerPoleCustomization: property 0x{:08X} contains a coordinate outside "
                         "+/-{} meters; ignoring.", propertyId, kMaxAttachCoordinateMagnitude);
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

    // Like the above but accepts any finite value (used for the signed attach-basis angle).
    bool TryReadFiniteFloatProperty(const cISCPropertyHolder* holder, const uint32_t propertyId, float& outValue) {
        if (holder == nullptr || !holder->HasProperty(propertyId)) {
            return false;
        }
        const cISCProperty* const prop = holder->GetProperty(propertyId);
        const cIGZVariant* const variant = prop ? prop->GetPropertyValue() : nullptr;
        float value = 0.0f;
        if (variant == nullptr || variant->GetType() != cIGZVariant::Float32 ||
            !variant->GetValFloat32(value) || !std::isfinite(value)) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} must be a finite Float32; ignoring.", propertyId);
            return false;
        }
        outValue = value;
        return true;
    }

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

    bool TryReadBoolProperty(const cISCPropertyHolder* holder, const uint32_t propertyId, bool& outValue) {
        if (holder == nullptr || !holder->HasProperty(propertyId)) {
            return false;
        }
        const cISCProperty* const prop = holder->GetProperty(propertyId);
        const cIGZVariant* const variant = prop ? prop->GetPropertyValue() : nullptr;
        bool value = false;
        if (variant == nullptr || variant->GetType() != cIGZVariant::Bool || !variant->GetValBool(value)) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} must be a Bool; ignoring.", propertyId);
            return false;
        }
        outValue = value;
        return true;
    }

    // ------------------------------------------------------------------
    // Custom foundation-texture registration. Draw() resolves texture IDs through the engine's
    // power-pole texture registry (see the kPoleTextureRegistry constants block); an ID missing
    // from that registry is a guaranteed null dereference, and StaticInit only ever registers the
    // four vanilla IDs. A custom ID is therefore accepted only after (1) its FSH resource is
    // proven to exist and (2) it has been inserted into the registry and re-found by the same
    // bucket walk Draw performs. Any failure keeps the pole on the vanilla texture.
    // ------------------------------------------------------------------
    std::unordered_set<uint32_t> gRegisteredPoleTextures; // IDs this DLL inserted this session

    // Read-only re-find using the exact node layout Draw's own walk uses. The returned pointer is
    // the node's +0x08 binding slot; Draw dereferences that slot without a null check.
    uint32_t* FindPoleTextureRegistrySlot(const uint32_t textureId) noexcept {
        const uintptr_t bucketsBegin = reinterpret_cast<uintptr_t>(
            *reinterpret_cast<uint8_t* const*>(kPoleTextureBucketsBegin));
        const uintptr_t bucketsEnd = reinterpret_cast<uintptr_t>(
            *reinterpret_cast<uint8_t* const*>(kPoleTextureBucketsEnd));
        if (bucketsBegin == 0 || bucketsEnd <= bucketsBegin) {
            return nullptr;
        }
        const uintptr_t span = bucketsEnd - bucketsBegin;
        constexpr uintptr_t kMaxPlausibleBucketCount = 1u << 20;
        if (span % sizeof(uint32_t) != 0) {
            return nullptr;
        }
        const uintptr_t bucketCount = span / sizeof(uint32_t);
        if (bucketCount == 0 || bucketCount > kMaxPlausibleBucketCount) {
            return nullptr;
        }
        uint8_t* node = reinterpret_cast<uint8_t* const*>(bucketsBegin)[textureId % bucketCount];
        while (node != nullptr && *reinterpret_cast<const uint32_t*>(node + 4) != textureId) {
            node = *reinterpret_cast<uint8_t* const*>(node);
        }
        return node == nullptr ? nullptr : reinterpret_cast<uint32_t*>(node + 8);
    }

    uint32_t ReadyPoleTextureOrVanilla(const uint32_t textureId,
                                       const uint32_t vanillaTextureId) noexcept {
        if (!RuntimeCustomizationEnabled() || textureId == vanillaTextureId) {
            return vanillaTextureId;
        }
        const uint32_t* const slot = FindPoleTextureRegistrySlot(textureId);
        return slot != nullptr && *slot != 0 ? textureId : vanillaTextureId;
    }

    bool EnsurePoleTextureRegistered(const uint32_t textureId, const char* const use) {
        if (textureId == kVanillaFloorTextureId || textureId == kVanillaWallTextureId ||
            textureId == kVanillaWireStrandTextureId || textureId == kVanillaWireBuoyTextureId) {
            return true; // the four vanilla IDs are registered by StaticInit itself
        }

        // A node inserted by another DLL is not proof that the referenced FSH exists. Validate the
        // resource first; otherwise forcing a reload can still leave a null binding for Draw to
        // dereference.
        const cIGZPersistResourceManagerPtr resourceManager;
        const cGZPersistResourceKey key(kFshTypeId, kPoleTextureGroupId, textureId);
        if (!resourceManager || !resourceManager->TestForKey(key)) {
            LOG_WARN("PowerPoleCustomization: {} texture ID 0x{:08X} has no FSH resource "
                     "(type 0x{:08X}, group 0x{:08X}); keeping the vanilla texture. Custom pad "
                     "textures must ship as FSH in that exact group.",
                     use, textureId, kFshTypeId, kPoleTextureGroupId);
            return false;
        }

        // Always re-check the live engine map. It is rebuilt between cities while this DLL and its
        // process-wide cache remain loaded; trusting gRegisteredPoleTextures alone can therefore
        // hand DrawPowerlines an absent ID and trigger its unchecked null dereference.
        if (uint32_t* const existingSlot = FindPoleTextureRegistrySlot(textureId)) {
            if (*existingSlot == 0) {
                // Another DLL may have inserted the key without loading its binding. Draw blindly
                // dereferences the binding pointer, so force the engine's all-texture reload before
                // accepting the ID even though the hash node itself already exists.
                *reinterpret_cast<uint32_t*>(kPoleTextureZoomGlobal) =
                    std::numeric_limits<uint32_t>::max();
            }
            gRegisteredPoleTextures.insert(textureId);
            return true;
        }
        gRegisteredPoleTextures.erase(textureId);

        using RegistryInsertFn = uint32_t*(__thiscall*)(void* map, const uint32_t* key);
        uint32_t* const insertedSlot = reinterpret_cast<RegistryInsertFn>(kPoleTextureRegistryInsert)(
            reinterpret_cast<void*>(kPoleTextureRegistry), &textureId);
        uint32_t* const verifiedSlot = FindPoleTextureRegistrySlot(textureId);
        if (insertedSlot == nullptr || verifiedSlot == nullptr) {
            LOG_ERROR("PowerPoleCustomization: engine texture-registry insert for {} texture ID "
                      "0x{:08X} could not be verified; keeping the vanilla texture.", use, textureId);
            return false;
        }
        *verifiedSlot = 0; // fresh node: no binding yet (StaticInit zeroes its own slots likewise)
        // Force the engine to reload every registered binding (including this one) on the next
        // Draw. StaticInit allocates exactly five width entries and Draw indexes them directly, so
        // the valid internal zoom domain is 0..4; UINT32_MAX cannot equal a real draw-context zoom.
        *reinterpret_cast<uint32_t*>(kPoleTextureZoomGlobal) = std::numeric_limits<uint32_t>::max();
        gRegisteredPoleTextures.insert(textureId);
        LOG_INFO("PowerPoleCustomization: registered {} texture ID 0x{:08X} with the engine's "
                 "power-pole texture registry.", use, textureId);
        return true;
    }

    bool ShouldInvertModelQuarterTurn(void* occupant) {
        cRZAutoRefCount<cISCExemplarPropertyHolder> propertyHolder;
        auto* const unknown = reinterpret_cast<cIGZUnknown*>(occupant);
        if (!unknown->QueryInterface(GZIID_cISCExemplarPropertyHolder, propertyHolder.AsPPVoid())) {
            return false;
        }

        cISCResExemplar* const exemplar = propertyHolder->GetDefaultExemplar();
        const cISCPropertyHolder* const holder = exemplar ? exemplar->AsISCPropertyHolder() : nullptr;
        bool invert = false;
        return TryReadBoolProperty(holder, kPropInvertModelQuarterTurn, invert) && invert;
    }

    using GetModelInstanceIDFn = uint32_t(__cdecl*)(void* resourceKeys, uint32_t zoom,
                                                    uint32_t cameraRotation, uint32_t quarterTurn,
                                                    bool useExplicitRotation, uint32_t* outType,
                                                    uint32_t* outGroup);

    uint32_t __cdecl GetModelInstanceIDHook(void* resourceKeys, const uint32_t zoom,
                                           const uint32_t cameraRotation, uint32_t quarterTurn,
                                           const bool useExplicitRotation, uint32_t* const outType,
                                           uint32_t* const outGroup) {
        auto* const occupant = reinterpret_cast<uint8_t*>(resourceKeys) - kOccupant_ModelResourceKeys;
        try {
            if (RuntimeCustomizationEnabled() && ShouldInvertModelQuarterTurn(occupant)) {
                quarterTurn ^= 1u;
            }
        } catch (...) {
            LogCurrentHookException("GetModelInstanceID");
        }

        const auto original = reinterpret_cast<GetModelInstanceIDFn>(kGetModelInstanceID);
        return original(resourceKeys, zoom, cameraRotation, quarterTurn, useExplicitRotation, outType, outGroup);
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
        if (values == nullptr) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} returned a null Float32 array; "
                     "ignoring.", propertyId);
            return false;
        }
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
        // Texture IDs are accepted only once the FSH resource is proven to exist and the ID is
        // registered with the engine's texture registry -- otherwise Draw() would null-deref on
        // the unknown ID (see EnsurePoleTextureRegistered). Failure falls back to vanilla.
        if (TryReadUint32Property(holder, kPropFoundationFloorTextureId, out.foundationFloorTextureId)) {
            if (EnsurePoleTextureRegistered(out.foundationFloorTextureId, "floor")) {
                out.hasFloorTexture = true;
                any = true;
            } else {
                out.foundationFloorTextureId = kVanillaFloorTextureId;
            }
        }
        if (TryReadUint32Property(holder, kPropFoundationWallTextureId, out.foundationWallTextureId)) {
            if (EnsurePoleTextureRegistered(out.foundationWallTextureId, "wall")) {
                out.hasWallTexture = true;
                any = true;
            } else {
                out.foundationWallTextureId = kVanillaWallTextureId;
            }
        }
        if (TryReadUint32Property(holder, kPropWireTextureId, out.wireTextureId)) {
            if (EnsurePoleTextureRegistered(out.wireTextureId, "wire strand")) {
                out.hasWireTexture = true;
                any = true;
            } else {
                out.wireTextureId = kVanillaWireStrandTextureId;
            }
        }
        if (TryReadUint32Property(holder, kPropWireBuoyTextureId, out.wireBuoyTextureId)) {
            if (EnsurePoleTextureRegistered(out.wireBuoyTextureId, "wire buoy")) {
                out.hasBuoyTexture = true;
                any = true;
            } else {
                out.wireBuoyTextureId = kVanillaWireBuoyTextureId;
            }
        }
        if (TryReadBoolProperty(holder, kPropFoundationFollowSpan, out.foundationFollowSpan) &&
            out.foundationFollowSpan) {
            any = true;
        }
        if (TryReadNonNegativeFloatProperty(holder, kPropWaterBuoySizeScale, out.waterBuoySizeScale)) {
            out.hasWaterBuoySize = true;
            any = true;
        }
        if (TryReadUint32Property(holder, kPropWaterBuoyPointCount, out.waterBuoyPointCount)) {
            out.waterBuoyPointCount = std::clamp(out.waterBuoyPointCount,
                                                 kMinWaterPolylinePoints, kMaxWaterPolylinePoints);
            out.hasWaterBuoyCount = true;
            any = true;
        }

        // Attach basis is only meaningful alongside custom attach points, so it deliberately does
        // NOT set `any`: a pole that declares a basis but no attach points has nothing to rotate and
        // stays fully vanilla. Stored normalized to [0, pi) to match ComputeSpanBearing's range.
        float basisDegrees = 0.0f;
        if (TryReadFiniteFloatProperty(holder, kPropAttachBasisDegrees, basisDegrees)) {
            // Do the conversion in double: every finite Float32 degree value remains finite in
            // double, while multiplying a very large Float32 in its own precision can overflow.
            constexpr double pi = 3.14159265358979323846;
            double basisRadians = std::fmod(static_cast<double>(basisDegrees) * (pi / 180.0), pi);
            if (basisRadians < 0.0f) {
                basisRadians += pi;
            }
            out.attachBasisRadians = static_cast<float>(basisRadians);
            out.hasAttachBasis = true;
        }
        return any;
    }

    // ------------------------------------------------------------------
    // Attach-point yaw alignment. Vanilla stores attach offsets baked for exactly 4 nominal span
    // orientations (Get0To3Direction) and applies them with a flat translation add -- no rotation
    // matrix (docs SS6). For any span whose true bearing deviates from its direction's nominal
    // angle (every FAR span, and any future free-angle span), the baked offset is rotated around
    // the vertical axis by that deviation so cables meet the crossarm along the real span
    // direction. Deviation is 0 for all vanilla-aligned spans, so behavior there is unchanged.
    // ------------------------------------------------------------------
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kYawEpsilonRadians = 0.01f; // ~0.57 degrees; below this, skip the rebuild

    // Undirected nominal xz-plane angle per Get0To3Direction result: 0 = x axis, 1 = the
    // sign-agreeing diagonal (+x,+z), 2 = z axis, 3 = the opposing diagonal.
    constexpr std::array<float, 4> kNominalDirectionAngle = {0.0f, kPi / 4.0f, kPi / 2.0f, 3.0f * kPi / 4.0f};

    // Undirected in-plane bearing of the span: atan2(dz, dx) normalized to [0, pi). Both endpoints
    // of the undirected span agree on it. Returns false (and leaves the bearing at 0) when the two
    // poles coincide.
    bool ComputeSpanBearing(void* poleA, void* poleB, float& outBearing) {
        const auto* const posA = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(poleA) + kOccupant_PosX);
        const auto* const posB = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(poleB) + kOccupant_PosX);
        const float dx = posB[0] - posA[0];
        const float dz = posB[2] - posA[2];
        if (dx == 0.0f && dz == 0.0f) {
            outBearing = 0.0f;
            return false;
        }
        float bearing = std::atan2(dz, dx);
        if (bearing < 0.0f) {
            bearing += kPi; // undirected line angle in [0, pi)
        }
        outBearing = bearing;
        return true;
    }

    // Minimal undirected rotation: wrap a yaw deviation into (-pi/2, pi/2].
    float WrapYaw(float delta) {
        while (delta > kPi / 2.0f) delta -= kPi;
        while (delta <= -kPi / 2.0f) delta += kPi;
        return delta;
    }

    // The heading (radians, [0, pi)) this pole's attach offsets were baked for, in this direction.
    // A pole that both supplies custom attach points for the direction AND declares an attach basis
    // uses that basis; every other case -- vanilla baked table, or no declared basis -- uses the
    // direction's nominal 0/45/90/135-degree angle, so cables stay aligned to the table they
    // actually came from. This is what lets a FAR-authored pole and a fallback pole share one span.
    float AttachBasisAngle(void* pole, const uint32_t direction) {
        const auto it = gOverrides.find(pole);
        if (it != gOverrides.end() && it->second.hasAttachBasis && it->second.PointCount(direction) > 0) {
            return it->second.attachBasisRadians;
        }
        return kNominalDirectionAngle[direction & 3];
    }

    AttachPoint RotateOffsetAroundY(const AttachPoint& offset, const float yaw) {
        if (yaw == 0.0f) {
            return offset;
        }
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        return AttachPoint{offset.x * c - offset.z * s, offset.y, offset.x * s + offset.z * c};
    }

    // ------------------------------------------------------------------
    // Vanilla attach-point read, reimplemented for the case where a pole has no override -- exact
    // same arithmetic as the original GetLineConnectionPoints (inlined into AddConnection /
    // UpdateConnection on Windows, so it has no address of its own to call into): table base is
    // read from this+kOccupant_ConnectionPointTablePtr, point N of direction D is the cS3DVector3
    // at (D & 3) * 0x30 + N * 0xc, translated by this pole's world position -- with the one
    // deliberate addition that the model-local offset is yaw-rotated first (see above).
    // ------------------------------------------------------------------
    AttachPoint ReadVanillaAttachPoint(void* occupant, const uint32_t direction, const uint32_t index,
                                       const float yawDelta) {
        auto* const base = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionPointTablePtr);
        const auto* const point = reinterpret_cast<const float*>(
            base + (direction & 3) * kVanillaDirectionStrideBytes + index * 0xc);

        const AttachPoint local = RotateOffsetAroundY(AttachPoint{point[0], point[1], point[2]}, yawDelta);
        const auto* const position = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_PosX);
        return AttachPoint{local.x + position[0], local.y + position[1], local.z + position[2]};
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

    AttachPoint GetAttachPoint(void* occupant, const uint32_t direction, const uint32_t index,
                               const float yawDelta) {
        const auto it = gOverrides.find(occupant);
        if (it != gOverrides.end()) {
            if (const AttachPoint* const p = it->second.Point(direction, index)) {
                const AttachPoint local = RotateOffsetAroundY(*p, yawDelta);
                const auto* const position = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_PosX);
                return AttachPoint{local.x + position[0], local.y + position[1], local.z + position[2]};
            }
        }
        return ReadVanillaAttachPoint(occupant, direction, index, yawDelta);
    }

    // ------------------------------------------------------------------
    // Finds the render-owning tConnection for this exact pole pair. Windows UpdateConnection
    // performs the same scan: entry+0x00 must equal otherPole and entry+0x09 must be nonzero.
    // BreakAll's inner removal nulls +0x00 before its loop continues. Only the render-owning side
    // contains the polylines DrawPowerlines visits; the reciprocal entry normally has an empty
    // polyline vector.
    // ------------------------------------------------------------------
    void* FindConnectionEntry(void* pole, void* otherPole) {
        if (pole == nullptr || otherPole == nullptr) {
            return nullptr;
        }
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

    bool AdjustControlPointSag(float* controlPoint, const AttachPoint& a, const AttachPoint& b,
                               const float sagScale, const float maximumSag) {
        // Exemplar values are finite Float32s, but products of otherwise-valid extreme values can
        // still overflow Float32. Keep the derivation in double and reject a result that cannot be
        // represented safely instead of feeding INF/NaN into the engine tessellator.
        const double dx = static_cast<double>(b.x) - a.x;
        const double dy = static_cast<double>(b.y) - a.y;
        const double dz = static_cast<double>(b.z) - a.z;
        const double lengthSquared = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(lengthSquared)) {
            return false;
        }
        if (lengthSquared <= std::numeric_limits<double>::epsilon()) {
            return true;
        }

        const double along = ((static_cast<double>(controlPoint[0]) - a.x) * dx +
                              (static_cast<double>(controlPoint[1]) - a.y) * dy +
                              (static_cast<double>(controlPoint[2]) - a.z) * dz) / lengthSquared;
        const double straightLineY = static_cast<double>(a.y) + dy * along;
        const double vanillaSag = std::max(0.0, straightLineY - controlPoint[1]);
        const double scaledSag = std::min(vanillaSag * static_cast<double>(sagScale),
                                          static_cast<double>(maximumSag));
        const double adjustedY = straightLineY - scaledSag;
        if (!std::isfinite(adjustedY) ||
            adjustedY < -std::numeric_limits<float>::max() ||
            adjustedY > std::numeric_limits<float>::max()) {
            return false;
        }
        controlPoint[1] = static_cast<float>(adjustedY);
        return true;
    }

    // Builds one tessellated Bezier polyline without touching the live tConnection. All replacement
    // polylines are prepared this way before the old engine vectors are cleared, so a bad_alloc or
    // invalid derived point leaves the original geometry intact.
    bool BuildPolylinePoints(const AttachPoint& a, const AttachPoint& b, const float sagScale,
                             const float maximumSag, std::vector<float>& points) {
        float control1[3];
        float control2[3];
        int32_t pointCount = 0;
        const auto getControlPoints = reinterpret_cast<GetControlPointsFn>(kGetControlPoints);
        getControlPoints(a.x, a.y, a.z, b.x, b.y, b.z, control1, control2, &pointCount);
        if (!AdjustControlPointSag(control1, a, b, sagScale, maximumSag) ||
            !AdjustControlPointSag(control2, a, b, sagScale, maximumSag)) {
            return false;
        }
        pointCount = std::clamp(pointCount, 2, 64); // vanilla treats this as total points, including p0

        const float p0[3] = {a.x, a.y, a.z};
        const float p1[3] = {b.x, b.y, b.z};
        points.assign(static_cast<size_t>(pointCount) * 3, 0.0f);
        points[0] = a.x;
        points[1] = a.y;
        points[2] = a.z;
        const auto tessellate = reinterpret_cast<TessellateBezierSegmentFn>(kTessellateBezierSegment);
        tessellate(p0, control1, control2, p1, reinterpret_cast<uintptr_t>(points.data() + 3), pointCount - 1);
        return std::ranges::all_of(points, [](const float value) { return std::isfinite(value); });
    }

    // Water-crossing variant. The engine draws one buoy ball at every point EXCEPT the last, so a
    // plain 0..1 tessellation (vanilla's own layout) puts the first ball exactly on pole A's
    // attach node. The list's only consumer is that ball loop, so the points are free to place:
    // for B balls (totalPoints - 1), tessellate B+2 uniform points along the sagged bezier and
    // drop the t=0 one -- the stored points sit at t = k/(B+1) for k = 1..B+1, the engine draws
    // balls at the B strictly-interior positions, and neither pole ever wears a ball.
    bool BuildWaterPolylinePoints(const AttachPoint& a, const AttachPoint& b, const float sagScale,
                                  const float maximumSag, const int32_t totalPoints,
                                  std::vector<float>& points) {
        const int32_t balls = std::clamp(totalPoints - 1, 1, 62);
        float control1[3];
        float control2[3];
        int32_t ignoredCount = 0;
        const auto getControlPoints = reinterpret_cast<GetControlPointsFn>(kGetControlPoints);
        getControlPoints(a.x, a.y, a.z, b.x, b.y, b.z, control1, control2, &ignoredCount);
        if (!AdjustControlPointSag(control1, a, b, sagScale, maximumSag) ||
            !AdjustControlPointSag(control2, a, b, sagScale, maximumSag)) {
            return false;
        }

        const float p0[3] = {a.x, a.y, a.z};
        const float p1[3] = {b.x, b.y, b.z};
        // balls + 2 uniform points including both endpoints; index 0 (= pole A) is discarded.
        std::vector<float> uniform(static_cast<size_t>(balls + 2) * 3);
        uniform[0] = a.x;
        uniform[1] = a.y;
        uniform[2] = a.z;
        const auto tessellate = reinterpret_cast<TessellateBezierSegmentFn>(kTessellateBezierSegment);
        tessellate(p0, control1, control2, p1, reinterpret_cast<uintptr_t>(uniform.data() + 3), balls + 1);

        points.assign(uniform.begin() + 3, uniform.end()); // balls + 1 points
        return std::ranges::all_of(points, [](const float value) { return std::isfinite(value); });
    }

    using EnginePolylineList = std::array<void*, 3>; // begin, end, capacity (12 bytes on x86)
    static_assert(sizeof(void*) == 4 && sizeof(EnginePolylineList) == 12,
                  "Power-pole engine vector layout requires the 32-bit target.");

    // Appends a fully prepared point array into an engine-owned vector. The confirmed insert helper
    // deep-copies the temporary {begin,end,cap} view; no allocator ownership crosses the boundary.
    void AppendPreparedPolyline(void* polylines, const std::vector<float>& points) {
        const float* const pointsBegin = points.empty() ? nullptr : points.data();
        const float* const pointsEnd = pointsBegin == nullptr ? nullptr : pointsBegin + points.size();
        const void* view[3] = {pointsBegin, pointsEnd, pointsEnd};
        void* const insertPos = reinterpret_cast<void**>(polylines)[1];
        const auto insert = reinterpret_cast<PolylineListInsertFn>(kPolylineListInsert);
        insert(polylines, insertPos, view, 0, 1, 1);
    }

    void DestroyEnginePolylineList(EnginePolylineList& list) noexcept {
        const auto destroy = reinterpret_cast<PolylineListDestructorFn>(kPolylineListDestructor);
        destroy(list.data());
        list.fill(nullptr);
    }

    void SwapEnginePolylineList(void* entry, const uint32_t listBeginOffset,
                                EnginePolylineList& replacement) noexcept {
        auto** const live = reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(entry) + listBeginOffset);
        for (size_t i = 0; i < replacement.size(); ++i) {
            std::swap(live[i], replacement[i]);
        }
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

    void ForgetOccupantTextures(void* occupant) {
        std::erase_if(gConnectionTextureOverrides,
                      [occupant](const auto& item) { return item.second.owner == occupant; });
    }

    // Water-crossing buoy/ball size overrides, keyed like the wire-width overrides by the inner
    // polyline's point-buffer address (EDI's [+0] at the patched size read). Values are the
    // vanilla per-zoom width factors pre-multiplied by the pole's buoy size scale, so the patched
    // read stays a single pointer swap.
    struct WaterBallSizeOverride {
        void* owner = nullptr;
        std::array<float, kPowerLineZoomCount> sizeByZoom{};
    };
    std::unordered_map<const void*, WaterBallSizeOverride> gWaterBallSizeOverrides;

    void ForgetConnectionWaterBallSizes(void* entry) {
        if (entry == nullptr) {
            return;
        }
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_WaterPolylinesBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_WaterPolylinesEnd);
        for (uint8_t* polyline = begin; polyline != end; polyline += 12) {
            const void* const points = *reinterpret_cast<void* const*>(polyline);
            if (points != nullptr) {
                gWaterBallSizeOverrides.erase(points);
            }
        }
    }

    void ForgetOccupantWaterBallSizes(void* occupant) {
        std::erase_if(gWaterBallSizeOverrides,
                      [occupant](const auto& item) { return item.second.owner == occupant; });
    }

    bool TryScalePowerLineWidths(const float* const vanillaWidths, const float scale,
                                 std::array<float, kPowerLineZoomCount>& outValues) noexcept {
        if (vanillaWidths == nullptr || !std::isfinite(scale) || scale < 0.0f) {
            return false;
        }
        for (size_t zoom = 0; zoom < kPowerLineZoomCount; ++zoom) {
            const double scaled = static_cast<double>(vanillaWidths[zoom]) * scale;
            if (!std::isfinite(vanillaWidths[zoom]) || vanillaWidths[zoom] < 0.0f ||
                !std::isfinite(scaled) || scaled > std::numeric_limits<float>::max()) {
                return false;
            }
            outValues[zoom] = static_cast<float>(scaled);
        }
        return true;
    }

    void RegisterConnectionWaterBallSizes(void* occupant, void* entry, const float sizeScale) {
        ForgetConnectionWaterBallSizes(entry);
        if (sizeScale == 1.0f) {
            return;
        }
        const float* const vanillaWidths = *reinterpret_cast<float**>(kPowerLineWidthFactorsPtr);
        std::array<float, kPowerLineZoomCount> scaledSizes{};
        if (!TryScalePowerLineWidths(vanillaWidths, sizeScale, scaledSizes)) {
            LOG_WARN("PowerPoleCustomization: buoy size scale {} for pole {} overflows the engine's "
                     "Float32 size range; using vanilla buoy sizes.", sizeScale, occupant);
            return;
        }
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_WaterPolylinesBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_WaterPolylinesEnd);
        for (uint8_t* polyline = begin; polyline != end; polyline += 12) {
            const void* const points = *reinterpret_cast<void* const*>(polyline);
            if (points == nullptr) {
                continue;
            }
            WaterBallSizeOverride value;
            value.owner = occupant;
            value.sizeByZoom = scaledSizes;
            gWaterBallSizeOverrides[points] = value;
        }
    }

    const float* __cdecl ResolveWaterBallSizePointer(const void* polyline, const uint32_t zoom) noexcept {
        const float* const vanillaWidths = *reinterpret_cast<float**>(kPowerLineWidthFactorsPtr);
        if (!RuntimeCustomizationEnabled()) {
            return vanillaWidths + (zoom < kPowerLineZoomCount ? zoom : kPowerLineZoomCount - 1);
        }
        const void* const points = polyline ? *reinterpret_cast<void* const*>(polyline) : nullptr;
        const auto it = points ? gWaterBallSizeOverrides.find(points) : gWaterBallSizeOverrides.end();
        if (it != gWaterBallSizeOverrides.end() && zoom < kPowerLineZoomCount) {
            return &it->second.sizeByZoom[zoom];
        }
        return vanillaWidths + (zoom < kPowerLineZoomCount ? zoom : kPowerLineZoomCount - 1);
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    // Replaces the 11 bytes at 0x0064ba6e (MOV EAX,[0x00b46798]; FLD [EAX+EDX*4];
    // LEA EAX,[EAX+EDX*4]). On entry EDX = zoom, EDI = current inner water-polyline vector; on
    // return EAX must point at the size base value with that value pushed on the FPU stack (the
    // code that follows both uses ST0 and re-reads [EAX] several times). ECX is dead here
    // (overwritten at 0x0064ba7f) but preserved anyway alongside EDX.
    __declspec(naked) void WaterBallSizeHook() {
        __asm {
            push ecx
            push edx
            push edx
            push edi
            call ResolveWaterBallSizePointer
            add esp, 8
            pop edx
            pop ecx
            fld dword ptr [eax]
            ret
        }
    }
#else
#error Power-pole water-ball-size hook requires 32-bit MSVC.
#endif

    // (Re)registers wire/buoy texture overrides for every active render-owner tConnection entry in
    // this pole's vector. Called after connection mutation and saved-city rehydration, so registered
    // entry addresses are always current. The texture pair comes from this pole's exemplar; a pole
    // without wire texture properties inherits the other endpoint's pair per connection, mirroring
    // FindAppearanceOverride's precedence.
    void RegisterOccupantConnectionTextures(void* occupant) {
        ForgetOccupantTextures(occupant);
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionsBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionsEnd);
        const auto self = gOverrides.find(occupant);
        const bool selfHasTextures = self != gOverrides.end() &&
                                     (self->second.hasWireTexture || self->second.hasBuoyTexture);
        for (uint8_t* entry = begin; entry != end; entry += kConnection_Size) {
            if (*reinterpret_cast<void**>(entry + kConnection_OtherPole) == nullptr ||
                *reinterpret_cast<const uint8_t*>(entry + kConnection_IsRenderOwner) == 0) {
                continue; // DrawPowerlines only visits render-owner entries
            }
            const PoleAttachOverride* source = selfHasTextures ? &self->second : nullptr;
            if (source == nullptr) {
                void* const otherPole = *reinterpret_cast<void**>(entry + kConnection_OtherPole);
                const auto other = gOverrides.find(otherPole);
                if (other != gOverrides.end() &&
                    (other->second.hasWireTexture || other->second.hasBuoyTexture)) {
                    source = &other->second;
                }
            }
            if (source == nullptr) {
                continue; // fully vanilla pair; the lookup hook falls back by itself
            }
            ConnectionTextureOverride value;
            value.owner = occupant;
            value.strandTextureId = source->hasWireTexture ? source->wireTextureId
                                                           : kVanillaWireStrandTextureId;
            value.buoyTextureId = source->hasBuoyTexture ? source->wireBuoyTextureId
                                                          : kVanillaWireBuoyTextureId;
            gConnectionTextureOverrides[entry] = value;
        }
    }

    uint32_t __cdecl GetWireStrandTextureOverride(const void* entry) noexcept {
        const auto it = gConnectionTextureOverrides.find(entry);
        const uint32_t requested = it != gConnectionTextureOverrides.end()
            ? it->second.strandTextureId : kVanillaWireStrandTextureId;
        return ReadyPoleTextureOrVanilla(requested, kVanillaWireStrandTextureId);
    }

    uint32_t __cdecl GetWireBuoyTextureOverride(const void* entry) noexcept {
        const auto it = gConnectionTextureOverrides.find(entry);
        const uint32_t requested = it != gConnectionTextureOverrides.end()
            ? it->second.buoyTextureId : kVanillaWireBuoyTextureId;
        return ReadyPoleTextureOrVanilla(requested, kVanillaWireBuoyTextureId);
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    // Both sites replace the 5-byte `MOV ESI, imm32` with `CALL rel32`. At each site EBP holds the
    // current tConnection entry, ECX the bucket-array base, and EDI the in-progress bucket count --
    // ECX/EDI must survive (consumed by the SAR/DIV that follow); EAX/EDX are clobbered by the
    // original code immediately after (MOV EAX,ESI / XOR EDX,EDX), so only the result register ESI
    // matters beyond preservation.
    __declspec(naked) void WireStrandTextureHook() {
        __asm {
            push ecx
            push edx
            push edi
            push ebp
            call GetWireStrandTextureOverride
            add esp, 4
            mov esi, eax
            pop edi
            pop edx
            pop ecx
            ret
        }
    }

    __declspec(naked) void WireBuoyTextureHook() {
        __asm {
            push ecx
            push edx
            push edi
            push ebp
            call GetWireBuoyTextureOverride
            add esp, 4
            mov esi, eax
            pop edi
            pop edx
            pop ecx
            ret
        }
    }
#else
#error Power-pole wire-texture hooks require 32-bit MSVC.
#endif

    bool CommitPreparedConnectionGeometry(
        void* entry, const bool includeWater,
        const std::vector<std::vector<float>>& newRegular,
        const std::vector<std::vector<float>>& newWater) {
        if (includeWater && newWater.size() != newRegular.size()) {
            LOG_ERROR("PowerPoleCustomization: internal regular/water strand-count mismatch; "
                      "keeping the live geometry untouched.");
            return false;
        }

        EnginePolylineList preparedRegular{};
        EnginePolylineList preparedWater{};
        try {
            for (size_t i = 0; i < newRegular.size(); ++i) {
                AppendPreparedPolyline(preparedRegular.data(), newRegular[i]);
                if (includeWater) {
                    AppendPreparedPolyline(preparedWater.data(), newWater[i]);
                }
            }
        } catch (...) {
            LogCurrentHookException("connection geometry preparation");
            DestroyEnginePolylineList(preparedWater);
            DestroyEnginePolylineList(preparedRegular);
            return false;
        }

        // All game-heap allocation finished while the live connection was untouched. The commit is
        // now only pointer swaps; the temporaries receive the old lists and destroy them afterward.
        ForgetConnectionWidths(entry);
        if (includeWater) {
            ForgetConnectionWaterBallSizes(entry);
        }
        SwapEnginePolylineList(entry, kConnection_PolylinesBegin, preparedRegular);
        if (includeWater) {
            SwapEnginePolylineList(entry, kConnection_WaterPolylinesBegin, preparedWater);
        }
        DestroyEnginePolylineList(preparedWater);
        DestroyEnginePolylineList(preparedRegular);
        return true;
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
            if (!TryScalePowerLineWidths(vanillaWidths, scale, value.widthByZoom)) {
                LOG_WARN("PowerPoleCustomization: wire-width scale {} for pole {} wire {} overflows "
                         "the engine's Float32 width range; using vanilla width.",
                         scale, occupant, wireIndex);
                continue;
            }
            gPolylineWidthOverrides[points] = value;
        }
    }

    const float* __cdecl ResolveWireWidthPointer(void* occupant, const void* polyline) noexcept {
        const float* const vanillaWidths = *reinterpret_cast<float**>(kPowerLineWidthFactorsPtr);
        const uint32_t zoom = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(occupant) + 0xc4);
        if (!RuntimeCustomizationEnabled()) {
            return vanillaWidths + (zoom < kPowerLineZoomCount ? zoom : kPowerLineZoomCount - 1);
        }
        const void* const points = polyline ? *reinterpret_cast<void* const*>(polyline) : nullptr;
        const auto it = points ? gPolylineWidthOverrides.find(points) : gPolylineWidthOverrides.end();
        if (it != gPolylineWidthOverrides.end() && it->second.owner == occupant && zoom < kPowerLineZoomCount) {
            return &it->second.widthByZoom[zoom];
        }
        return vanillaWidths + (zoom < kPowerLineZoomCount ? zoom : kPowerLineZoomCount - 1);
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

    bool TryComputeRelativeCall(const uintptr_t site, const void* target, int32_t& outRelative,
                                const char* const name) {
        const int64_t difference = static_cast<int64_t>(reinterpret_cast<uintptr_t>(target)) -
                                   static_cast<int64_t>(site + 5);
        if (difference < std::numeric_limits<int32_t>::min() ||
            difference > std::numeric_limits<int32_t>::max()) {
            LOG_ERROR("PowerPoleCustomization: {} target is outside rel32 range of site 0x{:08X}; "
                      "not patching.", name, static_cast<uint32_t>(site));
            return false;
        }
        outRelative = static_cast<int32_t>(difference);
        return true;
    }

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
            int32_t relative = 0;
            if (!TryComputeRelativeCall(kWireWidthLookupPatchSite, &WireWidthLookupHook, relative,
                                        "wire-width patch")) {
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(site, kWireWidthLookupPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for wire-width patch (error {}).", GetLastError());
                return false;
            }
            std::memcpy(original_.data(), site, original_.size());
            patched_ = original_;
            patched_[0] = 0xe8;
            std::memcpy(patched_.data() + 1, &relative, sizeof(relative));
            std::fill(patched_.begin() + 5, patched_.end(), static_cast<uint8_t>(0x90));
            std::memcpy(site, patched_.data(), patched_.size());
            FlushInstructionCache(GetCurrentProcess(), site, kWireWidthLookupPatchSize);
            installed_ = true;
            DWORD ignored = 0;
            if (!VirtualProtect(site, kWireWidthLookupPatchSize, oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after installing "
                          "the wire-width patch (error {}).", GetLastError());
                return false; // installed_ remains true so all-or-nothing rollback restores bytes
            }
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const site = reinterpret_cast<uint8_t*>(kWireWidthLookupPatchSite);
            if (std::memcmp(site, patched_.data(), patched_.size()) != 0) {
                LOG_ERROR("PowerPoleCustomization: refusing to uninstall the wire-width patch because "
                          "its byte span is no longer owned by this DLL.");
                return;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(site, kWireWidthLookupPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed while uninstalling the "
                          "wire-width patch (error {}).", GetLastError());
                return;
            }
            std::memcpy(site, original_.data(), original_.size());
            FlushInstructionCache(GetCurrentProcess(), site, kWireWidthLookupPatchSize);
            installed_ = false;
            DWORD ignored = 0;
            if (!VirtualProtect(site, kWireWidthLookupPatchSize, oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after uninstalling "
                          "the wire-width patch (error {}).", GetLastError());
            }
        }

        [[nodiscard]] bool IsInstalled() const noexcept { return installed_; }

    private:
        std::array<uint8_t, kWireWidthLookupPatchSize> original_{};
        std::array<uint8_t, kWireWidthLookupPatchSize> patched_{};
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

    // Defined in the foundation-pad section below; used by the connection rebuild to keep a
    // follow-span pad aligned the moment its bearing changes.
    bool PoleFoundationYaw(void* occupant, float& outYaw);
    void RebuildFoundation(void* occupant, float halfExtent, float yaw);
    void RefreshFoundationAfterConnectionChange(void* occupant);

    // Rebuilds the complete regular-polyline list whenever either endpoint customizes attachment
    // geometry/count or sag. Width-only customization keeps vanilla geometry and merely registers
    // per-polyline width values. The active/render-owning side determines appearance when two pole
    // families disagree; if it has no appearance properties, the other endpoint supplies them.
    void ApplyConnectionCustomization(void* occupant, void* otherPole) {
        const uint32_t direction = DirectionBetween(occupant, otherPole);
        const uint32_t count = std::min(StrandCount(occupant, direction), StrandCount(otherPole, direction));
        // Re-key texture overrides first: this pole's connections vector may have just reallocated
        // regardless of which side render-owns the new pair, so every registered entry address of
        // this pole must be refreshed even when the early-return below fires.
        RegisterOccupantConnectionTextures(occupant);
        // The reciprocal/non-rendering side still owns a foundation. Refresh before the render-owner
        // test so both endpoints react when a span appears, disappears, or changes bearing.
        RefreshFoundationAfterConnectionChange(occupant);
        void* const entry = FindConnectionEntry(occupant, otherPole);
        if (entry == nullptr) {
            return; // reciprocal non-rendering side, or no live connection for this pair
        }

        const PoleAttachOverride* const appearance = FindAppearanceOverride(occupant, otherPole);
        // Off-nominal bearing (e.g. a FAR span) forces a rebuild so each pole's baked attach offsets
        // can be yaw-rotated onto the true span direction. A single shared delta is wrong when the
        // two endpoints were authored in different bases (a FAR-authored pole meeting a fallback or
        // vanilla one), so compute one deviation per endpoint. Both are 0 for vanilla-aligned spans,
        // so this reduces exactly to the previous single-delta behavior there.
        float spanBearing = 0.0f;
        const bool haveBearing = ComputeSpanBearing(occupant, otherPole, spanBearing);
        const float yawA = haveBearing ? WrapYaw(spanBearing - AttachBasisAngle(occupant, direction)) : 0.0f;
        const float yawB = haveBearing ? WrapYaw(spanBearing - AttachBasisAngle(otherPole, direction)) : 0.0f;
        // Vanilla built the water-crossing coarse list only when the span crosses water; reuse
        // that decision (list non-empty) instead of re-querying the terrain service, then keep it
        // aligned with the rebuilt strands -- count, attach points, yaw, and sag included. The
        // buoy source follows the same render-owner-first precedence as the appearance override.
        const bool hadWaterPolylines =
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + kConnection_WaterPolylinesBegin) !=
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entry) + kConnection_WaterPolylinesEnd);
        const PoleAttachOverride* buoySource = nullptr;
        if (hadWaterPolylines) {
            const auto self = gOverrides.find(occupant);
            if (self != gOverrides.end() &&
                (self->second.hasWaterBuoySize || self->second.hasWaterBuoyCount)) {
                buoySource = &self->second;
            } else {
                const auto other = gOverrides.find(otherPole);
                if (other != gOverrides.end() &&
                    (other->second.hasWaterBuoySize || other->second.hasWaterBuoyCount)) {
                    buoySource = &other->second;
                }
            }
        }
        const int32_t waterPointCount = (buoySource != nullptr && buoySource->hasWaterBuoyCount)
            ? static_cast<int32_t>(buoySource->waterBuoyPointCount)
            : kWaterPolylinePointCount;
        // Any water-crossing span is rebuilt unconditionally: vanilla's own layout draws its first
        // buoy ball exactly on pole A's attach node (the ball loop renders every point except the
        // last of a uniform 0..1 tessellation), and the rebuild replaces that with the
        // strictly-interior layout produced by BuildWaterPolylinePoints. On an otherwise-vanilla
        // span the strand rebuild this forces is geometry-identical (vanilla table, zero yaw,
        // default sag), so only the ball positions change.
        const bool rebuildGeometry = HasAttachPointCustomization(occupant, direction) ||
                                     HasAttachPointCustomization(otherPole, direction) ||
                                     (appearance != nullptr && appearance->hasSagCustomization) ||
                                     std::fabs(yawA) > kYawEpsilonRadians ||
                                     std::fabs(yawB) > kYawEpsilonRadians ||
                                     hadWaterPolylines;
        if (rebuildGeometry) {
            // Prepare every replacement before destroying the live geometry. This keeps allocation
            // failures and invalid derived coordinates on the old, complete engine-owned vectors.
            std::vector<std::vector<float>> regularPoints(count);
            std::vector<std::vector<float>> waterPoints(hadWaterPolylines ? count : 0);
            bool prepared = true;
            for (uint32_t i = 0; i < count; ++i) {
                const AttachPoint a = GetAttachPoint(occupant, direction, i, yawA);
                const AttachPoint b = GetAttachPoint(otherPole, direction, i, yawB);
                const float sagScale = appearance ? appearance->SagScale(i) : 1.0f;
                const float maximumSag = appearance ? appearance->MaximumSag(i) :
                                                      std::numeric_limits<float>::infinity();
                if (!BuildPolylinePoints(a, b, sagScale, maximumSag, regularPoints[i]) ||
                    (hadWaterPolylines && !BuildWaterPolylinePoints(
                        a, b, sagScale, maximumSag, waterPointCount, waterPoints[i]))) {
                    prepared = false;
                    break;
                }
            }
            if (!prepared) {
                LOG_ERROR("PowerPoleCustomization: derived non-finite points for connection {} -> {}; "
                          "keeping its previous geometry.", occupant, otherPole);
            } else {
                if (CommitPreparedConnectionGeometry(
                        entry, hadWaterPolylines, regularPoints, waterPoints)) {
                    LOG_DEBUG("PowerPoleCustomization: rebuilt {} strands in direction {} for "
                              "connection {} -> {}{}.", count, direction, occupant, otherPole,
                              hadWaterPolylines ? " (incl. water-crossing coarse list)" : "");
                }
            }
        }
        RegisterConnectionWidths(occupant, entry, appearance);
        if (hadWaterPolylines) {
            RegisterConnectionWaterBallSizes(occupant, entry,
                (buoySource != nullptr && buoySource->hasWaterBuoySize) ? buoySource->waterBuoySizeScale
                                                                        : 1.0f);
        }

    }

    // ------------------------------------------------------------------
    // Foundation pad size. CreateFloor always builds the vanilla 5.0 half-extent quad; we then
    // rescale its 4 corners in place around the pole's XZ position (leaving Y, color, and UV
    // untouched). CreateWalls runs unmodified afterward and reads this same buffer (FUN_0064c2c0
    // indexes occupant+0xe8 directly), so a 0 half-extent collapses floor AND walls to a degenerate
    // point with no separate texture-ID risk -- the safe way to get "no visible foundation".
    // ------------------------------------------------------------------
    // In-plane pad yaw for a follow-span pole: the shared undirected bearing of its live
    // connections, reduced to the minimal deviation from the nearest 90-degree multiple so the
    // walls stay on their vanilla sides. Returns false (pad stays axis-aligned) when the pole has
    // no connections or its connections disagree on a bearing (junctions/turns).
    bool PoleFoundationYaw(void* occupant, float& outYaw) {
        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionsBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionsEnd);
        bool haveBearing = false;
        float bearing = 0.0f;
        for (uint8_t* entry = begin; entry != end; entry += kConnection_Size) {
            void* const otherPole = *reinterpret_cast<void**>(entry + kConnection_OtherPole);
            float entryBearing = 0.0f;
            if (otherPole == nullptr || !ComputeSpanBearing(occupant, otherPole, entryBearing)) {
                continue;
            }
            if (!haveBearing) {
                bearing = entryBearing;
                haveBearing = true;
            } else if (std::fabs(WrapYaw(entryBearing - bearing)) > kYawEpsilonRadians) {
                return false; // junction: connections disagree, keep the vanilla axis-aligned pad
            }
        }
        if (!haveBearing) {
            return false;
        }
        // Reduce to (-45, 45] degrees around the nearest axis so wall sides are preserved.
        float yaw = std::fmod(bearing, kPi / 2.0f);
        if (yaw > kPi / 4.0f) {
            yaw -= kPi / 2.0f;
        }
        if (std::fabs(yaw) <= kYawEpsilonRadians) {
            return false; // axis-aligned span; nothing to rotate
        }
        outYaw = yaw;
        return true;
    }

    // Scales and/or yaw-rotates the freshly-built floor quad in place around the pole's XZ
    // position. Must run before CreateWalls, which derives the wall quads from this buffer.
    void TransformFoundationFloor(void* occupant, const float halfExtent, const float yaw) {
        const float scale = halfExtent / kVanillaFoundationHalfExtent;
        if (scale == 1.0f && yaw == 0.0f) {
            return;
        }
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        const auto* const position = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_PosX);
        const float posX = position[0];
        const float posZ = position[2]; // kOccupant_PosZ - kOccupant_PosX spans posY in between

        auto* const begin = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_FloorVertsBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(occupant) + kOccupant_FloorVertsEnd);
        for (uint8_t* vertex = begin; vertex + kFloorVertexStride <= end; vertex += kFloorVertexStride) {
            auto* const xyz = reinterpret_cast<float*>(vertex);
            const float dx = (xyz[0] - posX) * scale;
            const float dz = (xyz[2] - posZ) * scale;
            xyz[0] = posX + dx * c - dz * s;
            xyz[2] = posZ + dx * s + dz * c;
        }
    }

    // Full foundation rebuild outside the engine's own CreateFloor call sites: vanilla floor, our
    // transform, vanilla walls -- the exact sequence SetPosition runs (CreateFloor 0x0064c611 then
    // CreateWalls 0x0064c616). Used when a connection change alters the follow-span bearing after
    // the pad was already built.
    void RebuildFoundation(void* occupant, const float halfExtent, const float yaw) {
        using CreateWallsFn = void(__fastcall*)(void* occupant, void* edxUnused);
        reinterpret_cast<CreateWallsFn>(kCreateFloor)(occupant, nullptr);
        TransformFoundationFloor(occupant, halfExtent, yaw);
        reinterpret_cast<CreateWallsFn>(kCreateWalls)(occupant, nullptr);
    }

    // A connection change can alter the follow-span bearing (the first/last span appearing, or a
    // junction beginning/ending). Rebuild immediately instead of waiting for a zoom change to call
    // CreateFloor. This deliberately runs for render-owner and reciprocal connection entries.
    void RefreshFoundationAfterConnectionChange(void* occupant) {
        const auto selfOverride = gOverrides.find(occupant);
        if (selfOverride == gOverrides.end() ||
            (!selfOverride->second.foundationFollowSpan && !selfOverride->second.hasFoundationHalfExtent)) {
            return;
        }
        const float halfExtent = selfOverride->second.hasFoundationHalfExtent
                                     ? selfOverride->second.foundationHalfExtent
                                     : kVanillaFoundationHalfExtent;
        float yaw = 0.0f;
        if (selfOverride->second.foundationFollowSpan) {
            PoleFoundationYaw(occupant, yaw);
        }
        RebuildFoundation(occupant, halfExtent, yaw);
    }

    // Rebuild every runtime-only registration for an occupant whose outer connection vector was
    // loaded or compacted. The point-buffer and tConnection addresses are not stable across those
    // operations, so erasing by owner first is required even when the visible geometry is unchanged.
    uint32_t RehydrateOccupantConnections(void* occupant) {
        ForgetOccupantWidths(occupant);
        ForgetOccupantTextures(occupant);
        ForgetOccupantWaterBallSizes(occupant);

        auto* const begin = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionsBegin);
        auto* const end = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionsEnd);
        uint32_t liveConnections = 0;
        for (uint8_t* entry = begin; entry != end; entry += kConnection_Size) {
            void* const otherPole = *reinterpret_cast<void**>(entry + kConnection_OtherPole);
            if (otherPole == nullptr) {
                continue;
            }
            ++liveConnections;
            ApplyConnectionCustomization(occupant, otherPole);
        }
        if (liveConnections == 0) {
            RefreshFoundationAfterConnectionChange(occupant);
        }
        return liveConnections;
    }

    void RehydrateLoadedConnections() {
        auto pending = std::move(gPendingLoadedPoles);
        gPendingLoadedPoles.clear();
        uint32_t connectionEntries = 0;
        for (void* const occupant : pending) {
            try {
                connectionEntries += RehydrateOccupantConnections(occupant);
            } catch (...) {
                LogCurrentHookException("PostCityInit rehydration");
            }
        }
        if (!pending.empty()) {
            LOG_INFO("PowerPoleCustomization: rehydrated {} saved poles ({} connection entries) "
                     "after city load.", pending.size(), connectionEntries);
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
        const uint32_t requested = it != gOverrides.end() && it->second.hasFloorTexture
            ? it->second.foundationFloorTextureId : kVanillaFloorTextureId;
        return ReadyPoleTextureOrVanilla(requested, kVanillaFloorTextureId);
    }

    uint32_t __cdecl GetWallTextureHashOverride(void* occupant) noexcept {
        const auto it = gOverrides.find(occupant);
        const uint32_t requested = it != gOverrides.end() && it->second.hasWallTexture
            ? it->second.foundationWallTextureId : kVanillaWallTextureId;
        return ReadyPoleTextureOrVanilla(requested, kVanillaWallTextureId);
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

    // ------------------------------------------------------------------
    // Pole-style lookup. Returns the pole instance for a direction mask under the active style, or
    // the vanilla instance when no style is active / the mask isn't defined in that style's ini
    // section. EDI at the call site already holds the direction mask (0-15); pushed as the sole
    // __cdecl argument.
    // ------------------------------------------------------------------
    // Regular (non-FAR) resolution: the active style's entry for this mask, else vanilla.
    uint32_t ResolveRegularInstance(const uint32_t mask, const uint32_t* const vanillaTable) noexcept {
        if (gActiveStyleIndex == 0 || gActiveStyleIndex > gStyles.size()) {
            return vanillaTable[mask];
        }
        const PoleStyle& style = gStyles[gActiveStyleIndex - 1];
        return style.hasMask[mask] ? style.instanceByDirectionMask[mask] : vanillaTable[mask];
    }

    // Debug logging for FAR model selection. Logs only when the (heading, mask, resolved instance)
    // tuple changes, so a whole drag of identical mid-line poles logs once, and endpoints/junctions
    // that resolve differently log again. noexcept: the whole resolver path is noexcept, so the
    // formatting call is wrapped -- a logging failure must never propagate into the engine's placement
    // loop. Remove this and its call sites once the orient->model mapping is confirmed in-game.
    void LogFarModelSelection(const uint32_t headingIndex, const uint32_t mask, const uint32_t resolved,
                              const char* const source) noexcept {
        static uint32_t lastHeading = 0xFFFFFFFFu;
        static uint32_t lastMask = 0xFFFFFFFFu;
        static uint32_t lastResolved = 0xFFFFFFFFu;
        if (headingIndex == lastHeading && mask == lastMask && resolved == lastResolved) {
            return;
        }
        lastHeading = headingIndex;
        lastMask = mask;
        lastResolved = resolved;
        try {
            const uint32_t ratioIndex = std::min(headingIndex / kFarOrientCount,
                                                 static_cast<uint32_t>(kFarRatios.size() - 1));
            const uint32_t orient = headingIndex % kFarOrientCount;
            const FarRatio& r = kFarRatios[ratioIndex];
            const double slopeDeg = std::atan2(static_cast<double>(r.rise), static_cast<double>(r.run)) *
                                    180.0 / 3.14159265358979323846;
            LOG_DEBUG("PowerPoleCustomization: [FAR] model select FAR-{}.{} (slope {:.1f} deg, mask 0x{:X}{}) "
                     "-> pole instance 0x{:08X} [{}].",
                     kFarRatioLabels[ratioIndex], FarOrientName(orient), slopeDeg, mask,
                     mask == 0x8 ? " => engine +90 deg quarter-turn" : " => no engine turn",
                     resolved, source);
        } catch (...) {
        }
    }

    // FAR resolution order (docs/far-power-lines-design.md): the exact FAR key in the active style,
    // then in the [PowerPoles.FAR] default section, then the active style's regular entry for the
    // mask the span already classifies to, then the vanilla instance. Sparse tables therefore
    // degrade gracefully without unexpectedly changing pole family before the final fallback.
    uint32_t ResolveFarInstance(const uint32_t headingIndex, const uint32_t mask,
                                const uint32_t* const vanillaTable) noexcept {
        if (headingIndex < kFarHeadingCount) {
            if (gActiveStyleIndex >= 1 && gActiveStyleIndex <= gStyles.size()) {
                const PoleStyle& style = gStyles[gActiveStyleIndex - 1];
                if (style.hasFarHeading[headingIndex]) {
                    const uint32_t resolved = style.farInstanceByHeading[headingIndex];
                    LogFarModelSelection(headingIndex, mask, resolved, "active style");
                    return resolved;
                }
            }
            if (gHasFarDefault && gFarDefaultStyle.hasFarHeading[headingIndex]) {
                const uint32_t resolved = gFarDefaultStyle.farInstanceByHeading[headingIndex];
                LogFarModelSelection(headingIndex, mask, resolved, "[PowerPoles.FAR] default");
                return resolved;
            }
        }
        const uint32_t resolved = ResolveRegularInstance(mask, vanillaTable);
        LogFarModelSelection(headingIndex, mask, resolved, "REG/vanilla fallback (no FAR key)");
        return resolved;
    }

    uint32_t __cdecl ResolvePoleInstanceForDirectionMask(const uint32_t directionMask) noexcept {
        const auto* const vanillaTable = reinterpret_cast<const uint32_t*>(kPowerPoleForDirectionsFlag);
        const uint32_t mask = directionMask & 0xF;
        if (!RuntimeCustomizationEnabled()) {
            return vanillaTable[mask];
        }
        // A lone-diagonal mask placed during an active FAR drag routes to the drag's exact FAR
        // model. Multi-bit junctions and every regular drag stay on the regular path. Known gap:
        // the four-bit mask cannot distinguish two different bearings/ratios in the same broad
        // diagonal class, so a same-class merge can still look like a lone 0x2/0x8 mask and receive
        // the latest FAR model. Resolving that needs a merge-site hook plus live-bearing inspection.
        if (gFarDragActive && (mask == 0x2 || mask == 0x8)) {
            const uint32_t headingIndex = gFarDragRatioIndex * kFarOrientCount + gFarDragOrient;
            return ResolveFarInstance(headingIndex, mask, vanillaTable);
        }
        return ResolveRegularInstance(mask, vanillaTable);
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    // EDI must survive untouched (read again by the caller right after both call sites); EAX is the
    // only register the original 7-byte MOV wrote, matching what CALL leaves behind here too.
    __declspec(naked) void PoleStyleLookupHook() {
        __asm {
            push edi
            call ResolvePoleInstanceForDirectionMask
            add esp, 4
            ret
        }
    }
#else
#error Power-pole style-switching hook requires 32-bit MSVC.
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
    // Entered by a CALL replacing the first 7 bytes of UpdateOnZoneChange. The CALL adds one
    // temporary return address below the function's own return address/arguments. Disabled:
    // discard it, reproduce the two overwritten instructions, and resume at 0x006522f7. Enabled:
    // discard it and return directly to the caller, cleaning the original five stack arguments.
    __declspec(naked) void KeepWiresOnZoneChangeHook() {
        __asm {
            // UpdateOnZoneChange can create a replacement pole before it asks DrawNetworkLine for
            // a new path. Never let that non-drag placement inherit the previous user's FAR model.
            mov byte ptr [gFarDragActive], 0
            mov byte ptr [gFarSnapAnchorValid], 0
            cmp byte ptr [gRuntimeHooksEnabled], 0
            je run_original
            cmp byte ptr [gKeepWiresOnZoneChange], 0
            je run_original
            add esp, 4
            ret 14h
        run_original:
            add esp, 4
            sub esp, 68h
            mov eax, [esp + 7ch]
            mov edx, kUpdateOnZoneChangeResume
            jmp edx
        }
    }
#else
#error Keep-wires zone-change hook requires 32-bit MSVC.
#endif

    // Generic N-byte-prefix-match -> `CALL rel32` patch (5 bytes) + NOP padding for the remainder.
    // Used for the 7-byte pole-style lookup sites; kept separate from Imm32CallPatch (fixed 5-byte
    // `MOV EAX,imm32` sites) since the expected byte pattern and size differ here.
    class ByteSpanCallPatch final {
    public:
        bool Install(const uintptr_t site, std::initializer_list<uint8_t> expectedBytes, void* hookFn,
                     const char* name) {
            if (installed_) {
                return true;
            }
            site_ = site;
            size_ = static_cast<uint32_t>(expectedBytes.size());
            name_ = name;
            if (size_ < 5) {
                LOG_ERROR("PowerPoleCustomization: {} patch is only {} bytes; a rel32 call needs 5.",
                          name, size_);
                return false;
            }
            auto* const p = reinterpret_cast<uint8_t*>(site);
            if (!std::equal(expectedBytes.begin(), expectedBytes.end(), p)) {
                LOG_ERROR("PowerPoleCustomization: {} bytes at 0x{:08X} do not match 1.1.641; not patching.",
                          name, static_cast<uint32_t>(site));
                return false;
            }
            int32_t relative = 0;
            if (!TryComputeRelativeCall(site, hookFn, relative, name)) {
                return false;
            }

            // Prepare both byte images before making executable memory writable. Vector growth can
            // throw; no exception should strand the code page RWX halfway through installation.
            original_.assign(p, p + size_);
            patched_ = original_;
            patched_[0] = 0xe8;
            std::memcpy(patched_.data() + 1, &relative, sizeof(relative));
            std::fill(patched_.begin() + 5, patched_.end(), static_cast<uint8_t>(0x90));

            DWORD oldProtect = 0;
            if (!VirtualProtect(p, size_, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for {} (error {}).", name, GetLastError());
                return false;
            }
            std::memcpy(p, patched_.data(), patched_.size());
            FlushInstructionCache(GetCurrentProcess(), p, size_);
            installed_ = true;
            DWORD ignored = 0;
            if (!VirtualProtect(p, size_, oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after installing "
                          "{} (error {}).", name_, GetLastError());
                return false; // installed_ remains true so all-or-nothing rollback restores bytes
            }
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const p = reinterpret_cast<uint8_t*>(site_);
            if (patched_.size() != size_ || std::memcmp(p, patched_.data(), size_) != 0) {
                LOG_ERROR("PowerPoleCustomization: refusing to uninstall {} because its byte span is "
                          "no longer owned by this DLL.", name_);
                return;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(p, size_, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed while uninstalling {} "
                          "(error {}).", name_, GetLastError());
                return;
            }
            std::memcpy(p, original_.data(), original_.size());
            FlushInstructionCache(GetCurrentProcess(), p, size_);
            installed_ = false;
            DWORD ignored = 0;
            if (!VirtualProtect(p, size_, oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after uninstalling "
                          "{} (error {}).", name_, GetLastError());
            }
        }

        [[nodiscard]] bool IsInstalled() const noexcept { return installed_; }

    private:
        uintptr_t site_ = 0;
        uint32_t size_ = 0;
        std::string name_;
        std::vector<uint8_t> original_;
        std::vector<uint8_t> patched_;
        bool installed_ = false;
    };

    // Replaces one function pointer inside a vtable (a 4-byte data write, not a code patch).
    // Used for the power tool's DrawNetworkLine slot: the method is only ever dispatched
    // virtually, so swapping cSC4PowerLineTool's own slot hooks that tool exclusively -- other
    // network tools dispatch through their own untouched vtables.
    class VTableSlotPatch final {
    public:
        bool Install(const uintptr_t slotAddress, const uintptr_t expectedTarget, void* hookFn, const char* name) {
            if (installed_) {
                return true;
            }
            slot_ = slotAddress;
            name_ = name;
            replacement_ = reinterpret_cast<uintptr_t>(hookFn);
            auto* const slot = reinterpret_cast<uintptr_t*>(slotAddress);
            if (*slot != expectedTarget) {
                LOG_ERROR("PowerPoleCustomization: {} vtable slot 0x{:08X} holds 0x{:08X}, expected 0x{:08X}; not patching.",
                          name, static_cast<uint32_t>(slotAddress), static_cast<uint32_t>(*slot),
                          static_cast<uint32_t>(expectedTarget));
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for {} (error {}).", name, GetLastError());
                return false;
            }
            original_ = *slot;
            *slot = replacement_;
            installed_ = true;
            DWORD ignored = 0;
            if (!VirtualProtect(slot, sizeof(uintptr_t), oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after installing "
                          "{} (error {}).", name_, GetLastError());
                return false; // installed_ remains true so all-or-nothing rollback restores the slot
            }
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const slot = reinterpret_cast<uintptr_t*>(slot_);
            if (*slot != replacement_) {
                LOG_ERROR("PowerPoleCustomization: refusing to uninstall {} because its vtable slot is "
                          "no longer owned by this DLL.", name_);
                return;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed while uninstalling {} "
                          "(error {}).", name_, GetLastError());
                return;
            }
            *slot = original_;
            installed_ = false;
            DWORD ignored = 0;
            if (!VirtualProtect(slot, sizeof(uintptr_t), oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after uninstalling "
                          "{} (error {}).", name_, GetLastError());
            }
        }

        [[nodiscard]] bool IsInstalled() const noexcept { return installed_; }

    private:
        uintptr_t slot_ = 0;
        uintptr_t original_ = 0;
        uintptr_t replacement_ = 0;
        std::string name_;
        bool installed_ = false;
    };

    // Generic 5-byte `MOV r32, imm32` -> `CALL rel32` patch, one instance per hash-lookup site.
    // Kept separate from InlineCallPatch (used for the 23-byte wire-width site) since the byte
    // count and expected-opcode check differ. The opcode selects the destination register the
    // vanilla instruction wrote (0xB8 = EAX for the Draw floor/wall sites, 0xBE = ESI for the
    // DrawPowerlines wire/buoy sites); the hook is responsible for producing its result in that
    // same register.
    class Imm32CallPatch final {
    public:
        bool Install(const uintptr_t site, const uint32_t expectedImm, void* hookFn, const char* name,
                     const uint8_t expectedOpcode = kMovEaxOpcode) {
            if (installed_) {
                return true;
            }
            site_ = site;
            name_ = name;
            auto* const p = reinterpret_cast<uint8_t*>(site);
            const std::array<uint8_t, 5> expected = {
                expectedOpcode,
                static_cast<uint8_t>(expectedImm), static_cast<uint8_t>(expectedImm >> 8),
                static_cast<uint8_t>(expectedImm >> 16), static_cast<uint8_t>(expectedImm >> 24),
            };
            if (std::memcmp(p, expected.data(), expected.size()) != 0) {
                LOG_ERROR("PowerPoleCustomization: {} bytes at 0x{:08X} do not match 1.1.641; not patching.",
                          name, static_cast<uint32_t>(site));
                return false;
            }
            int32_t relative = 0;
            if (!TryComputeRelativeCall(site, hookFn, relative, name)) {
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for {} (error {}).", name, GetLastError());
                return false;
            }
            std::memcpy(original_.data(), p, original_.size());
            patched_ = original_;
            patched_[0] = 0xe8;
            std::memcpy(patched_.data() + 1, &relative, sizeof(relative));
            std::memcpy(p, patched_.data(), patched_.size());
            FlushInstructionCache(GetCurrentProcess(), p, 5);
            installed_ = true;
            DWORD ignored = 0;
            if (!VirtualProtect(p, 5, oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after installing "
                          "{} (error {}).", name_, GetLastError());
                return false; // installed_ remains true so all-or-nothing rollback restores bytes
            }
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const p = reinterpret_cast<uint8_t*>(site_);
            if (std::memcmp(p, patched_.data(), patched_.size()) != 0) {
                LOG_ERROR("PowerPoleCustomization: refusing to uninstall {} because its byte span is "
                          "no longer owned by this DLL.", name_);
                return;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed while uninstalling {} "
                          "(error {}).", name_, GetLastError());
                return;
            }
            std::memcpy(p, original_.data(), original_.size());
            FlushInstructionCache(GetCurrentProcess(), p, 5);
            installed_ = false;
            DWORD ignored = 0;
            if (!VirtualProtect(p, 5, oldProtect, &ignored)) {
                LOG_ERROR("PowerPoleCustomization: failed to restore page protection after uninstalling "
                          "{} (error {}).", name_, GetLastError());
            }
        }

        [[nodiscard]] bool IsInstalled() const noexcept { return installed_; }

    private:
        uintptr_t site_ = 0;
        std::array<uint8_t, 5> original_{};
        std::array<uint8_t, 5> patched_{};
        std::string name_;
        bool installed_ = false;
    };

    // ------------------------------------------------------------------
    // Hook bodies.
    // ------------------------------------------------------------------
    using InitConnectionPointsFn = void(__thiscall*)(void* occupant);
    using AddConnectionFn = void(__thiscall*)(void* occupant, void* otherPole, void* lineInfoVector);
    using UpdateConnectionFn = void(__thiscall*)(void* occupant, void* otherPole);
    using RemoveConnectionFn = void(__thiscall*)(void* occupant, void* otherPole, uint32_t notify);
    using RemoveConnectionEntryFn = void(__thiscall*)(void* occupant, void* entry, uint32_t notify);
    using CreateFloorFn = void(__fastcall*)(void* occupant, void* /*unused edx*/);

    // Vanilla selects one of its two fixed attachment tables from bits 8-11 of the model's RKT1
    // group. Arbitrary custom-model groups can encode a value outside vanilla's accepted 1-4
    // range, in which case InitConnectionPoints silently leaves the constructor-initialized field
    // null. AddConnection/UpdateConnection dereference that field before our custom polyline rebuild,
    // so seed a safe staging table for those models. Custom attachment geometry still replaces the
    // resulting vanilla strands in ApplyConnectionCustomization.
    void RepairMissingConnectionPointTable(void* occupant) {
        auto*& table = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uint8_t*>(occupant) + kOccupant_ConnectionPointTablePtr);
        if (table != nullptr) {
            return;
        }

        const uint32_t directionFlags = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(occupant) + kOccupant_DirectionFlags);
        const bool diagonalOnly = (directionFlags & kDiagonalDirectionFlags) != 0 &&
                                  (directionFlags & kCardinalDirectionFlags) == 0;
        table = reinterpret_cast<uint8_t*>(
            diagonalOnly ? kDiagonalConnectionPoints : kCardinalConnectionPoints);
        LOG_WARN("PowerPoleCustomization: vanilla left pole {} without a connection-point table "
                 "(direction flags 0x{:X}); using the {} table as a safe fallback.",
                 occupant, directionFlags, diagonalOnly ? "diagonal" : "cardinal");
    }

    void __fastcall CreateFloorHook(void* occupant, void* edxUnused) {
        const auto original = reinterpret_cast<CreateFloorFn>(kCreateFloor);
        original(occupant, edxUnused);
        try {
            if (!RuntimeCustomizationEnabled()) {
                return;
            }
            const auto it = gOverrides.find(occupant);
            if (it == gOverrides.end()) {
                return;
            }
            const float halfExtent = it->second.hasFoundationHalfExtent
                ? it->second.foundationHalfExtent : kVanillaFoundationHalfExtent;
            float yaw = 0.0f;
            if (it->second.foundationFollowSpan) {
                PoleFoundationYaw(occupant, yaw);
            }
            TransformFoundationFloor(occupant, halfExtent, yaw);
        } catch (...) {
            LogCurrentHookException("CreateFloor");
        }
    }

    using DestructorFn = void(__thiscall*)(void* occupant);

    // Runs when the engine destroys a pole (demolition, zone rebuild, city teardown). Dropping the
    // side-table entries here -- unconditionally, even while disabled -- guarantees no stale
    // customization can attach to a future pole that recycles the same 292-byte pool slot, and
    // keeps both maps from growing across a long session.
    void __fastcall DestructorHook(void* occupant, void* /*edx*/) {
        try {
            gPendingLoadedPoles.erase(occupant);
            gOverrides.erase(occupant);
            ForgetOccupantWidths(occupant);
            ForgetOccupantTextures(occupant);
            ForgetOccupantWaterBallSizes(occupant);
        } catch (...) {
            LogCurrentHookException("Destructor cleanup");
        }
        const auto original = reinterpret_cast<DestructorFn>(kDestructor);
        original(occupant);
    }

    void ApplyInitConnectionPointsCustomization(void* occupant) {
        ForgetOccupantWidths(occupant);
        ForgetOccupantTextures(occupant);
        ForgetOccupantWaterBallSizes(occupant);
        gOverrides.erase(occupant); // a failed refresh must never leave the previous exemplar's data

        if (!RuntimeCustomizationEnabled()) {
            return;
        }
        RepairMissingConnectionPointTable(occupant);

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
        }
    }

    void __fastcall InitConnectionPointsHook(void* occupant, void* /*edx*/) {
        const auto original = reinterpret_cast<InitConnectionPointsFn>(kInitConnectionPoints);
        original(occupant); // vanilla engine failures retain their original propagation semantics
        try {
            ApplyInitConnectionPointsCustomization(occupant);
        } catch (...) {
            LogCurrentHookException("InitConnectionPoints");
        }
    }

    // Read calls InitConnectionPoints only after deserializing both polyline vectors. Do not rebuild
    // them in the middle of object-stream loading; queue this occupant for the PostCityInit pass,
    // when the reciprocal pole and both exemplar overrides are guaranteed to have initialized.
    void __fastcall InitConnectionPointsReadHook(void* occupant, void* /*edx*/) {
        const auto original = reinterpret_cast<InitConnectionPointsFn>(kInitConnectionPoints);
        original(occupant);
        try {
            ApplyInitConnectionPointsCustomization(occupant);
            if (!RuntimeCustomizationEnabled()) {
                return;
            }
            const auto* const begin = *reinterpret_cast<uint8_t* const*>(
                reinterpret_cast<const uint8_t*>(occupant) + kOccupant_ConnectionsBegin);
            const auto* const end = *reinterpret_cast<uint8_t* const*>(
                reinterpret_cast<const uint8_t*>(occupant) + kOccupant_ConnectionsEnd);
            if (begin != end) {
                gPendingLoadedPoles.insert(occupant);
            }
        } catch (...) {
            LogCurrentHookException("InitConnectionPoints@Read");
        }
    }

    void __fastcall AddConnectionHook(void* occupant, void* /*edx*/, void* otherPole, void* lineInfoVector) {
        try {
            if (RuntimeCustomizationEnabled()) {
                // A growing outer tConnection vector uses its copy constructor for every existing
                // entry (0x0064e0e0 -> 0x0064cfa0), which deep-copies both inner polyline vectors.
                // Their point-buffer keys can all change, not just the newly added span's key.
                ForgetOccupantWidths(occupant);
                ForgetOccupantWaterBallSizes(occupant);
            }
        } catch (...) {
            LogCurrentHookException("AddConnection pre-cleanup");
        }
        const auto original = reinterpret_cast<AddConnectionFn>(kAddConnection);
        original(occupant, otherPole, lineInfoVector);

        if (!RuntimeCustomizationEnabled()) {
            return;
        }
        try {
            RehydrateOccupantConnections(occupant);
        } catch (...) {
            LogCurrentHookException("AddConnection");
        }
    }

    void __fastcall UpdateConnectionHook(void* occupant, void* /*edx*/, void* otherPole) {
        try {
            if (RuntimeCustomizationEnabled()) {
                void* const entry = FindConnectionEntry(occupant, otherPole);
                ForgetConnectionWidths(entry);
                ForgetConnectionWaterBallSizes(entry);
            }
        } catch (...) {
            LogCurrentHookException("UpdateConnection pre-cleanup");
        }
        const auto original = reinterpret_cast<UpdateConnectionFn>(kUpdateConnection);
        original(occupant, otherPole);
        if (RuntimeCustomizationEnabled()) {
            try {
                ApplyConnectionCustomization(occupant, otherPole);
            } catch (...) {
                LogCurrentHookException("UpdateConnection");
            }
        }
    }

    // RemoveConnection compacts the tConnection vector, while its inner polyline buffers are freed.
    // Drop all owner registrations before vanilla mutates either structure, then register surviving
    // connections at their new addresses and refresh a foundation whose last bearing may be gone.
    void __fastcall RemoveConnectionHook(void* occupant, void* /*edx*/, void* otherPole, uint32_t notify) {
        try {
            if (RuntimeCustomizationEnabled()) {
                ForgetOccupantWidths(occupant);
                ForgetOccupantTextures(occupant);
                ForgetOccupantWaterBallSizes(occupant);
            }
        } catch (...) {
            LogCurrentHookException("RemoveConnection pre-cleanup");
        }
        const auto original = reinterpret_cast<RemoveConnectionFn>(kRemoveConnection);
        original(occupant, otherPole, notify);
        if (RuntimeCustomizationEnabled()) {
            try {
                RehydrateOccupantConnections(occupant);
            } catch (...) {
                LogCurrentHookException("RemoveConnection");
            }
        }
    }

    // BreakAllConnections clears every entry in one fixed-stride loop instead of compacting the
    // outer vector. This call-site-specific hook drops registrations once at the first entry and
    // refreshes once at the last; rebuilding after every entry is quadratic and exposes teardown's
    // intermediate state to foundation/geometry work for no benefit.
    void __fastcall RemoveConnectionEntryHook(void* occupant, void* /*edx*/, void* entry, uint32_t notify) {
        const auto* const connectionsBegin = *reinterpret_cast<uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(occupant) + kOccupant_ConnectionsBegin);
        const auto* const connectionsEnd = *reinterpret_cast<uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(occupant) + kOccupant_ConnectionsEnd);
        const bool firstEntry = entry == connectionsBegin;
        const bool lastEntry = reinterpret_cast<const uint8_t*>(entry) + kConnection_Size == connectionsEnd;
        try {
            if (RuntimeCustomizationEnabled() && firstEntry) {
                ForgetOccupantWidths(occupant);
                ForgetOccupantTextures(occupant);
                ForgetOccupantWaterBallSizes(occupant);
            }
        } catch (...) {
            LogCurrentHookException("RemoveConnectionEntry pre-cleanup");
        }
        const auto original = reinterpret_cast<RemoveConnectionEntryFn>(kRemoveConnectionEntry);
        original(occupant, entry, notify);
        if (RuntimeCustomizationEnabled() && lastEntry) {
            try {
                RehydrateOccupantConnections(occupant);
            } catch (...) {
                LogCurrentHookException("RemoveConnectionEntry");
            }
        }
    }

    // Inter-pole distance for the active style, or the tool's own vanilla value when no style is
    // active / the active style doesn't override it. See kDeterminePolePositions block above.
    uint32_t ResolveMaxCellsBetweenPoles(const uint32_t vanillaValue) noexcept {
        if (gActiveStyleIndex == 0 || gActiveStyleIndex > gStyles.size()) {
            return vanillaValue;
        }
        const PoleStyle& style = gStyles[gActiveStyleIndex - 1];
        return style.hasMaxCells ? style.maxCellsBetweenPoles : vanillaValue;
    }

    using DeterminePolePositionsFn = void(__thiscall*)(void* tool);
    // Ghidra: every caller pushes a 32-bit 0/1 flag and the callee ends in RET 4. Keep the full
    // stack slot in both signatures even though the source-level value behaves like a bool.
    using PlacePolesFn = void(__thiscall*)(void* tool, uint32_t placementPass);

    void __fastcall DeterminePolePositionsHook(void* tool, void* /*edx*/) {
        auto* const field = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(tool) + kTool_MaxCellsBetweenPoles);
        const uint32_t saved = *field;
        struct RestoreField final {
            uint32_t* field;
            uint32_t value;
            ~RestoreField() noexcept { *field = value; }
        } restore{field, saved};
        gLastObservedVanillaInterPoleDistance = saved;
        if (RuntimeCustomizationEnabled()) {
            uint32_t resolved = ResolveMaxCellsBetweenPoles(saved);
            // During a FAR drag every synthetic step is one FAR period, and
            // the cadence field counts step indices, not literal cells (docs SS17.C). Poles can
            // only sit on FAR nodes, so convert the cell spacing into whole periods (nearest,
            // minimum 1): FAR-2 with vanilla 10 -> every 5 nodes = 10 cells along the major axis.
            if (gFarDragActive && gFarDragRun > 0) {
                resolved = std::max(1u, (resolved + gFarDragRun / 2) / gFarDragRun);
            }
            *field = resolved;
        }
        const auto original = reinterpret_cast<DeterminePolePositionsFn>(kDeterminePolePositions);
        original(tool);
    }

    void __fastcall PlacePolesHook(void* tool, void* /*edx*/, const uint32_t placementPass) {
        struct ResetFarTransaction final {
            ~ResetFarTransaction() noexcept {
                gFarDragActive = false;
                gFarSnapAnchorValid = false;
            }
        } reset;
        const auto original = reinterpret_cast<PlacePolesFn>(kPlacePoles);
        original(tool, placementPass); // reset runs on return and preserves engine exception propagation
    }

    // ------------------------------------------------------------------
    // FAR drag hook (see the kDrawNetworkLine constants block above and
    // docs/sc4-powerline-tool-re.md SS17 for the design). Validated in-game 2026-07-19.
    // ------------------------------------------------------------------
    using DrawNetworkLineFn = uint8_t(__thiscall*)(void* tool, uint32_t* start, uint32_t* end,
                                                    uint8_t straightOnly, int networkType);

    struct CellXZ {
        uint32_t x;
        uint32_t z;
    };

    struct DraggedStep {
        uint32_t firstCellIndex;
        uint32_t lastCellIndex;
        uint32_t unknownZeroed; // vanilla ComputeDraggedCells writes 0 here
    };

    // 4-connected supercover of one FAR period in (major, minor) cell space: every cell the
    // center-to-center node segment passes through, from (0,0) inclusive to (run,rise) exclusive,
    // in traversal order. At an exact corner crossing both adjacent cells are included, matching
    // vanilla's own 2-cells-per-column diagonal registration.
    std::vector<std::pair<uint32_t, uint32_t>> BuildFarPeriodPattern(const uint32_t run, const uint32_t rise) {
        std::vector<std::pair<uint32_t, uint32_t>> cells;
        uint32_t u = 0;
        uint32_t v = 0;
        cells.emplace_back(0u, 0u);
        while (u != run || v != rise) {
            // Next boundary crossings of the segment (0.5,0.5) -> (run+0.5, rise+0.5), compared
            // cross-multiplied to stay in integers: next u-boundary at t_u = (u + 0.5) / run,
            // next v-boundary at t_v = (v + 0.5) / rise (both scaled by 2*run*rise).
            bool advanceU;
            bool corner = false;
            if (v >= rise) {
                advanceU = true;
            } else if (u >= run) {
                advanceU = false;
            } else {
                const uint64_t tu = static_cast<uint64_t>(2 * u + 1) * rise;
                const uint64_t tv = static_cast<uint64_t>(2 * v + 1) * run;
                advanceU = tu < tv;
                corner = tu == tv;
            }
            if (corner) {
                // Corner crossing: register both adjacent cells, then continue diagonally.
                cells.emplace_back(u + 1, v);
                cells.emplace_back(u, v + 1);
                ++u;
                ++v;
            } else if (advanceU) {
                ++u;
            } else {
                ++v;
            }
            if (u == run && v == rise) {
                break; // the closing node belongs to the next period (or the final step)
            }
            cells.emplace_back(u, v);
        }
        return cells;
    }

    float SnapCandidateAngle(const uint32_t candidate) {
        if (candidate == kAxisSnapCandidate) {
            return 0.0f;
        }
        if (candidate == kDiagonalSnapCandidate) {
            return kDiagonalAngleRadians;
        }
        const FarRatio& ratio = kFarRatios[candidate - 1];
        return std::atan2(static_cast<float>(ratio.rise), static_cast<float>(ratio.run));
    }

    bool ActiveStyleSupportsRatio(uint32_t ratioIndex);

    uint32_t SelectSnapCandidate(const uint32_t startX, const uint32_t startZ,
                                 const uint32_t absoluteDx, const uint32_t absoluteDz) {
        const uint32_t major = std::max(absoluteDx, absoluteDz);
        const uint32_t minor = std::min(absoluteDx, absoluteDz);
        const float angle = std::atan2(static_cast<float>(minor), static_cast<float>(major));

        uint32_t bestCandidate = kAxisSnapCandidate;
        float bestDifference = std::fabs(angle - SnapCandidateAngle(bestCandidate));
        for (uint32_t candidate = 1; candidate <= kDiagonalSnapCandidate; ++candidate) {
            if (candidate != kDiagonalSnapCandidate && !ActiveStyleSupportsRatio(candidate - 1)) {
                continue; // never snap to a FAR ratio that has no model mapping for this style
            }
            const float difference = std::fabs(angle - SnapCandidateAngle(candidate));
            const bool candidateIsRegular = candidate == kAxisSnapCandidate || candidate == kDiagonalSnapCandidate;
            const bool bestIsRegular = bestCandidate == kAxisSnapCandidate || bestCandidate == kDiagonalSnapCandidate;
            // Prefer a regular heading at an exact tie, otherwise take the genuinely nearer one.
            if (difference < bestDifference || (difference == bestDifference && candidateIsRegular && !bestIsRegular)) {
                bestCandidate = candidate;
                bestDifference = difference;
            }
        }

        if (gFarSnapAnchorValid && gFarSnapAnchorX == startX && gFarSnapAnchorZ == startZ) {
            const bool previousIsRegular = gFarSnapCandidate == kAxisSnapCandidate ||
                                           gFarSnapCandidate == kDiagonalSnapCandidate;
            const bool previousIsSupported = previousIsRegular ||
                (gFarSnapCandidate > kAxisSnapCandidate && gFarSnapCandidate < kDiagonalSnapCandidate &&
                 ActiveStyleSupportsRatio(gFarSnapCandidate - 1));
            if (previousIsSupported) {
                const float previousDifference = std::fabs(angle - SnapCandidateAngle(gFarSnapCandidate));
                if (previousDifference <= bestDifference + kSnapHysteresisRadians) {
                    bestCandidate = gFarSnapCandidate;
                }
            }
        }

        gFarSnapAnchorValid = true;
        gFarSnapAnchorX = startX;
        gFarSnapAnchorZ = startZ;
        gFarSnapCandidate = bestCandidate;
        return bestCandidate;
    }

    uint8_t __fastcall FarDrawNetworkLineHookImpl(void* tool, void* /*edx*/, uint32_t* start, uint32_t* end,
                                                  uint8_t straightOnly, int networkType,
                                                  bool& inOriginalCall) {
        const auto original = reinterpret_cast<DrawNetworkLineFn>(kDrawNetworkLine);
        const auto callOriginal = [&](uint32_t* const originalStart, uint32_t* const originalEnd,
                                      const uint8_t originalStraightOnly) -> uint8_t {
            inOriginalCall = true;
            const uint8_t result = original(
                tool, originalStart, originalEnd, originalStraightOnly, networkType);
            inOriginalCall = false;
            return result;
        };
        gFarDragActive = false;
        if (!RuntimeCustomizationEnabled() || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            gFarSnapAnchorValid = false;
            return callOriginal(start, end, straightOnly);
        }

        const int32_t dx = static_cast<int32_t>(end[0]) - static_cast<int32_t>(start[0]);
        const int32_t dz = static_cast<int32_t>(end[1]) - static_cast<int32_t>(start[1]);
        const uint32_t adx = static_cast<uint32_t>(std::abs(dx));
        const uint32_t adz = static_cast<uint32_t>(std::abs(dz));
        if (adx == 0 && adz == 0) {
            gFarSnapAnchorValid = false;
            return callOriginal(start, end, straightOnly);
        }

        const uint32_t snapCandidate = SelectSnapCandidate(start[0], start[1], adx, adz);
        if (snapCandidate == kAxisSnapCandidate || snapCandidate == kDiagonalSnapCandidate) {
            return callOriginal(start, end, straightOnly); // vanilla handles regular headings
        }

        const FarRatio ratio = kFarRatios[snapCandidate - 1];

        const uint32_t cityCellsX = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uint8_t*>(tool) + kTool_CityCellsX);
        const uint32_t cityCellsZ = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uint8_t*>(tool) + kTool_CityCellsZ);
        const bool majorIsX = adx >= adz;
        const uint32_t majorLen = majorIsX ? adx : adz;
        const uint32_t minorLen = majorIsX ? adz : adx;
        const int32_t majorSign = (majorIsX ? dx : dz) > 0 ? 1 : -1;
        const int32_t minorSign = (majorIsX ? dz : dx) > 0 ? 1 : -1;

        // Neighbor connections require the terminal cell itself to be on the city boundary. For
        // ordinary drags, retain nearest-period snapping. For a boundary drag, keep as many exact
        // FAR periods as fit and preserve the cursor's boundary cell as a short transition tail.
        const CellXZ requestedEnd{end[0], end[1]};
        const bool exitsAtNegativeX = dx < 0 && requestedEnd.x == 0;
        const bool exitsAtPositiveX = dx > 0 && requestedEnd.x + 1 == cityCellsX;
        const bool exitsAtNegativeZ = dz < 0 && requestedEnd.z == 0;
        const bool exitsAtPositiveZ = dz > 0 && requestedEnd.z + 1 == cityCellsZ;
        const bool boundaryTerminal = exitsAtNegativeX || exitsAtPositiveX ||
                                      exitsAtNegativeZ || exitsAtPositiveZ;
        uint32_t periods = boundaryTerminal
            ? std::min(majorLen / ratio.run, minorLen / ratio.rise)
            : (majorLen + ratio.run / 2) / ratio.run;
        auto nodeCell = [&](const uint32_t period) -> CellXZ {
            const int32_t du = majorSign * static_cast<int32_t>(period * ratio.run);
            const int32_t dv = minorSign * static_cast<int32_t>(period * ratio.rise);
            return CellXZ{
                static_cast<uint32_t>(static_cast<int32_t>(start[0]) + (majorIsX ? du : dv)),
                static_cast<uint32_t>(static_cast<int32_t>(start[1]) + (majorIsX ? dv : du)),
            };
        };
        while (periods >= 1) {
            const CellXZ last = nodeCell(periods);
            if (last.x < cityCellsX && last.z < cityCellsZ) {
                break;
            }
            --periods;
        }
        if (periods < 1) {
            return callOriginal(start, end, straightOnly);
        }

        const CellXZ terminal = boundaryTerminal ? requestedEnd : nodeCell(periods);
        const CellXZ lastFarNode = nodeCell(periods);
        const bool hasTransitionTail = terminal.x != lastFarNode.x || terminal.z != lastFarNode.z;

        // Dense preview layout: one step per FAR period, with that step spanning every terrain tile
        // crossed by the period's 4-connected supercover. DeterminePolePositions treats the first
        // cell of each all-straight step as its primary cell, so pole placement remains sparse at FAR
        // nodes while vanilla's blue terrain overlay sees a complete tile path instead of dots.
        std::vector<CellXZ> cells;
        std::vector<DraggedStep> steps;
        const auto periodPattern = BuildFarPeriodPattern(ratio.run, ratio.rise);
        cells.reserve(periods * periodPattern.size() + ratio.run + ratio.rise + 2);
        steps.reserve(periods + 3);
        const auto appendPattern = [&](const CellXZ origin,
                                       const std::vector<std::pair<uint32_t, uint32_t>>& pattern,
                                       const bool patternMajorIsX, const int32_t patternMajorSign,
                                       const int32_t patternMinorSign) {
            const uint32_t firstCell = static_cast<uint32_t>(cells.size());
            for (const auto [u, v] : pattern) {
                const int32_t du = patternMajorSign * static_cast<int32_t>(u);
                const int32_t dv = patternMinorSign * static_cast<int32_t>(v);
                const int32_t x = static_cast<int32_t>(origin.x) + (patternMajorIsX ? du : dv);
                const int32_t z = static_cast<int32_t>(origin.z) + (patternMajorIsX ? dv : du);
                if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= cityCellsX ||
                    static_cast<uint32_t>(z) >= cityCellsZ) {
                    return false;
                }
                cells.push_back(CellXZ{static_cast<uint32_t>(x), static_cast<uint32_t>(z)});
            }
            if (cells.size() == firstCell) {
                return false;
            }
            steps.push_back(DraggedStep{firstCell, static_cast<uint32_t>(cells.size() - 1), 0});
            return true;
        };
        for (uint32_t p = 0; p < periods; ++p) {
            if (!appendPattern(nodeCell(p), periodPattern, majorIsX, majorSign, minorSign)) {
                return callOriginal(start, end, straightOnly);
            }
        }
        if (hasTransitionTail) {
            // A boundary drag keeps the last whole-period node as a pole, then the boundary cell
            // itself, so AttemptNeighborConnections' outward one-cell probe actually leaves the city.
            const int32_t rawTailMajor = majorIsX
                ? static_cast<int32_t>(terminal.x) - static_cast<int32_t>(lastFarNode.x)
                : static_cast<int32_t>(terminal.z) - static_cast<int32_t>(lastFarNode.z);
            const int32_t rawTailMinor = majorIsX
                ? static_cast<int32_t>(terminal.z) - static_cast<int32_t>(lastFarNode.z)
                : static_cast<int32_t>(terminal.x) - static_cast<int32_t>(lastFarNode.x);
            if (rawTailMajor * majorSign < 0 || rawTailMinor * minorSign < 0) {
                LOG_WARN("PowerPoleCustomization: [FAR PoC] boundary transition reverses direction; falling back.");
                return callOriginal(start, end, straightOnly);
            }
            const uint32_t tailRun = static_cast<uint32_t>(std::abs(rawTailMajor));
            const uint32_t tailRise = static_cast<uint32_t>(std::abs(rawTailMinor));
            const auto tailPattern = BuildFarPeriodPattern(tailRun, tailRise);
            const int32_t tailMajorSign = rawTailMajor >= 0 ? 1 : -1;
            const int32_t tailMinorSign = rawTailMinor >= 0 ? 1 : -1;
            if (!appendPattern(lastFarNode, tailPattern, majorIsX, tailMajorSign, tailMinorSign)) {
                return callOriginal(start, end, straightOnly);
            }
        }
        const uint32_t terminalIndex = static_cast<uint32_t>(cells.size());
        cells.push_back(terminal);
        steps.push_back(DraggedStep{terminalIndex, terminalIndex, 0});

        // Ask vanilla to allocate both vectors with the largest legal in-city drag. A corner-to-
        // corner diagonal creates roughly width+height cells, which bounds every legal supercover,
        // while the major-axis step count bounds our one-step-per-period representation. We use the
        // vectors' real capacity pointers below rather than mistaking their current end for capacity.
        const auto totalCells = static_cast<uint32_t>(cells.size());
        if (cityCellsX == 0 || cityCellsZ == 0) {
            return callOriginal(start, end, straightOnly);
        }
        uint32_t fakeStart[2]{0, 0};
        uint32_t fakeEnd[2]{cityCellsX - 1, cityCellsZ - 1};
        if (callOriginal(fakeStart, fakeEnd, 0) == 0) {
            LOG_WARN("PowerPoleCustomization: [FAR PoC] vanilla allocation drag failed; falling back.");
            return callOriginal(start, end, straightOnly);
        }

        auto* const base = reinterpret_cast<uint8_t*>(tool);
        auto* const stepsBegin = *reinterpret_cast<uint8_t**>(base + kTool_StepsBegin);
        auto* const cellsBegin = *reinterpret_cast<uint8_t**>(base + kTool_CellsBegin);
        auto* const stepsCapacityEnd = *reinterpret_cast<uint8_t**>(base + kTool_StepsCapacity);
        auto* const cellsCapacityEnd = *reinterpret_cast<uint8_t**>(base + kTool_CellsCapacity);
        const auto stepCapacity = stepsBegin != nullptr && stepsCapacityEnd >= stepsBegin
            ? static_cast<uint32_t>((stepsCapacityEnd - stepsBegin) / sizeof(DraggedStep)) : 0;
        const auto cellCapacity = cellsBegin != nullptr && cellsCapacityEnd >= cellsBegin
            ? static_cast<uint32_t>((cellsCapacityEnd - cellsBegin) / sizeof(CellXZ)) : 0;
        if (stepsBegin == nullptr || cellsBegin == nullptr || stepsCapacityEnd == nullptr ||
            cellsCapacityEnd == nullptr ||
            stepCapacity < steps.size() || cellCapacity < totalCells) {
            LOG_WARN("PowerPoleCustomization: [FAR PoC] vanilla drag allocated {} steps/{} cells, "
                     "need {}/{}; falling back.", stepCapacity, cellCapacity, steps.size(), totalCells);
            return callOriginal(start, end, straightOnly);
        }

        // Rewrite the POD contents in place and shrink the end pointers -- no game-allocator
        // interaction. All steps are declared "straight" so GetPrimaryCell returns first cells
        // and the straight/diag-junction special case never engages.
        std::memcpy(cellsBegin, cells.data(), totalCells * sizeof(CellXZ));
        *reinterpret_cast<uint8_t**>(base + kTool_CellsEnd) = cellsBegin + totalCells * sizeof(CellXZ);
        std::memcpy(stepsBegin, steps.data(), steps.size() * sizeof(DraggedStep));
        *reinterpret_cast<uint8_t**>(base + kTool_StepsEnd) = stepsBegin + steps.size() * sizeof(DraggedStep);
        *reinterpret_cast<uint32_t*>(base + kTool_TotalSteps) = static_cast<uint32_t>(steps.size());
        *reinterpret_cast<uint32_t*>(base + kTool_StraightSteps) = static_cast<uint32_t>(steps.size());
        *reinterpret_cast<uint8_t*>(base + kTool_DiagonalFlag) = 0;

        // AttemptNeighborConnections reduces these even direction codes with `direction >> 1`
        // and indexes its cardinal X/Z delta tables. Prefer the actual crossed edge at corners;
        // otherwise the FAR major-axis direction is the outward probe direction.
        uint32_t neighborDirection = 0;
        if ((majorIsX && (exitsAtNegativeX || exitsAtPositiveX)) ||
            (!majorIsX && !(exitsAtNegativeZ || exitsAtPositiveZ) &&
             (exitsAtNegativeX || exitsAtPositiveX))) {
            neighborDirection = exitsAtPositiveX ? 4u : 0u;
        } else if (exitsAtNegativeZ || exitsAtPositiveZ) {
            neighborDirection = exitsAtPositiveZ ? 6u : 2u;
        } else if (majorIsX) {
            neighborDirection = majorSign > 0 ? 4u : 0u;
        } else {
            neighborDirection = majorSign > 0 ? 6u : 2u;
        }
        *reinterpret_cast<uint32_t*>(base + kTool_FirstDirection) = neighborDirection;
        *reinterpret_cast<uint32_t*>(base + kTool_SecondDirection) = neighborDirection;

        // BreakIntoStraightAndDiagSegments writes the snapped endpoint back into `end`; callers
        // consume it, so the FAR snap must do the same.
        end[0] = terminal.x;
        end[1] = terminal.z;
        // Record the exact heading so CreatePowerPole's resolver can pick the FAR model. Orientation
        // is fixed by which axis is major and whether the major/minor steps share sign (the +x,+z /
        // -x,-z family vs the +x,-z / -x,+z family). majorSign/minorSign share sign exactly when the
        // span runs into the sign-agreeing diagonal, matching Get0To3Direction's 1-vs-3 split.
        const bool signsAgree = majorSign == minorSign;
        gFarDragOrient = majorIsX ? (signsAgree ? kFarOrientXP : kFarOrientXN)
                                  : (signsAgree ? kFarOrientZP : kFarOrientZN);
        gFarDragRatioIndex = snapCandidate - 1; // snapCandidate 1..N maps to kFarRatios 0..N-1

        // Debug trace (silent at the default info level): report when the drag snaps to a different
        // FAR heading than last time. Pairs with the per-pole model-select trace in ResolveFarInstance
        // so a bad orientation can be traced from the heading the drag chose to the resolved instance.
        static uint32_t lastLoggedDragHeading = 0xFFFFFFFFu;
        const uint32_t dragHeading = gFarDragRatioIndex * kFarOrientCount + gFarDragOrient;
        if (dragHeading != lastLoggedDragHeading) {
            lastLoggedDragHeading = dragHeading;
            const double slopeDeg = std::atan2(static_cast<double>(ratio.rise),
                                               static_cast<double>(ratio.run)) * 180.0 / 3.14159265358979323846;
            LOG_DEBUG("PowerPoleCustomization: [FAR] drag snapped to FAR-{}.{} (slope {:.1f} deg, {}-major, "
                     "run/rise={}/{}, majorSign={}, minorSign={}).",
                     kFarRatioLabels[gFarDragRatioIndex], FarOrientName(gFarDragOrient), slopeDeg,
                     majorIsX ? "x" : "z", ratio.run, ratio.rise, majorSign, minorSign);
        }

        gFarDragActive = true;
        gFarDragRun = ratio.run;
        return 1;
    }

    uint8_t __fastcall FarDrawNetworkLineHook(void* tool, void* /*edx*/, uint32_t* start, uint32_t* end,
                                              uint8_t straightOnly, int networkType) {
        const uint32_t savedEnd[2] = {end[0], end[1]};
        bool inOriginalCall = false;
        try {
            return FarDrawNetworkLineHookImpl(
                tool, nullptr, start, end, straightOnly, networkType, inOriginalCall);
        } catch (...) {
            if (inOriginalCall) {
                throw; // preserve the original engine call's exception behavior; never replay it
            }
            LogCurrentHookException("FarDrawNetworkLine");
            end[0] = savedEnd[0];
            end[1] = savedEnd[1];
            gFarDragActive = false;
            gFarSnapAnchorValid = false;
        }
        const auto original = reinterpret_cast<DrawNetworkLineFn>(kDrawNetworkLine);
        return original(tool, start, end, straightOnly, networkType);
    }

    // ------------------------------------------------------------------
    // Tab / Shift-Tab pole-style switch + optional ImGui status overlay.
    // See docs/power-line-style-ui-design.md. The key hook has no ImGui dependency; the overlay only
    // ever runs when the ImGui service was acquired. Both the IC vtable (0x00aab008, proven by the
    // OnKeyDown slot's install-time byte check) and the power-tool vtable (0x00aa9f30, proven by the
    // FAR DrawNetworkLine patch's byte check) are validated against the live binary before either is
    // trusted at runtime, so a recompiled SC4 with moved vtables degrades to "no overlay / no Tab"
    // rather than misbehaving. The runtime checks below are pure reads + compares (no writes).
    // ------------------------------------------------------------------

    // Cached only to gate overlay visibility. City lifecycle messages and the render callback can
    // interleave, so acquisition/use/release is serialized and disabled outside an active city.
    cISC4View3DWin* gView3D = nullptr;
    std::mutex gView3DMutex;
    std::atomic_bool gCityActive{false};

    // True when `control` is the shared network-tool input control AND the subtool it is currently
    // driving is the power line tool. Used by the key hook (control = its own `this`) and by the
    // overlay visibility check (control = GetCurrentViewInputControl()).
    bool IsNetworkControlDrivingPowerTool(void* control) {
        if (control == nullptr || *reinterpret_cast<uintptr_t*>(control) != kNetworkToolInputControlVtable) {
            return false;
        }
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(control) + kIC_NetworkType) == kNetworkTypePower;
    }

    // Advances the active pole style. dir > 0 forward, dir < 0 backward. Index 0 is vanilla; custom
    // styles occupy 1..gStyles.size(). Wraps. Driven by the Tab/Shift-Tab key hook.
    void CycleStyle(const int dir) {
        if (gStyles.empty()) {
            gActiveStyleIndex = 0;
            gFarDragActive = false;
            gFarSnapAnchorValid = false;
            return;
        }
        const uint32_t count = static_cast<uint32_t>(gStyles.size()) + 1; // + vanilla(0)
        const uint32_t step = dir >= 0 ? 1u : count - 1u;
        gActiveStyleIndex = (gActiveStyleIndex + step) % count;
        // The last drag's classification was resolved under the previous style's supported FAR
        // ratios. Do not let it select a stale angled model or remain highlighted in the overlay.
        gFarDragActive = false;
        gFarSnapAnchorValid = false;
        const std::string activeName = gActiveStyleIndex == 0 ? "vanilla" : gStyles[gActiveStyleIndex - 1].name;
        LOG_INFO("PowerPoleCustomization: active pole style -> \"{}\" ({} of {}) -- applies to subsequent "
                 "placement; touched junctions may be restyled.",
                 activeName, gActiveStyleIndex, gStyles.size());
    }

    using OnKeyDownFn = uint8_t(__thiscall*)(void* control, int vkCode, uint32_t modifiers);
    using InputControlIsOnTopFn = bool(__thiscall*)(void* control);

    // Bit 0 of the OnKeyDown modifiers argument is Shift -- confirmed against vanilla's own
    // Tab handler in cSC4ViewInputControlNetworkIntxTool::OnKeyDown (0x00660e80), which picks the
    // forward/backward intersection-rule direction from exactly `modifiers & 1`.
    constexpr uint32_t kKeyModifierShift = 0x1;

    // Vtable-slot hook (installed like the FAR DrawNetworkLine hook: __fastcall matches __thiscall
    // with a throwaway edx). Consumes Tab only while the power tool is the active network subtool;
    // everything else -- including Tab under road/rail/street -- falls through to vanilla untouched.
    uint8_t __fastcall OnKeyDownHook(void* control, void* /*edx*/, int vkCode, uint32_t modifiers) {
        try {
            const auto isOnTop = reinterpret_cast<InputControlIsOnTopFn>(kInputControlIsOnTop);
            if (RuntimeCustomizationEnabled() && !gStyles.empty() &&
                vkCode == static_cast<int>(kVkTab) &&
                IsNetworkControlDrivingPowerTool(control) && isOnTop(control)) {
                // A style switch between the last preview and mouse-up would commit the already-
                // built FAR path with a different model/cadence. Consume this keypress without
                // switching; the user can press Tab again after the drag transaction has ended.
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                    return 1;
                }
                const bool shift = (modifiers & kKeyModifierShift) != 0;
                CycleStyle(shift ? -1 : 1);
                return 1; // don't let vanilla focus handling also act on Tab
            }
        } catch (...) {
            LogCurrentHookException("OnKeyDown");
        }
        // Keep the engine call outside the customization catch: a vanilla exception must not cause
        // the same input event to be dispatched a second time.
        const auto original = reinterpret_cast<OnKeyDownFn>(kOnKeyDown);
        return original(control, vkCode, modifiers);
    }

    // View3D (and the 3D-view window it lives under) does not exist at PostAppInit -- it appears
    // once a city view is up. Acquire it lazily on the render thread, retrying until it resolves,
    // instead of failing overlay setup permanently at app init.
    cISC4View3DWin* EnsureView3DLocked() {
        if (!gCityActive.load(std::memory_order_acquire)) {
            return nullptr;
        }
        if (gView3D != nullptr) {
            return gView3D;
        }
        if (const cISC4AppPtr app; app) {
            if (cIGZWin* const mainWindow = app->GetMainWindow()) {
                if (cIGZWin* const sc4AppWin = mainWindow->GetChildWindowFromID(kGZWin_WinSC4App)) {
                    sc4AppWin->GetChildAs(kGZWin_SC4View3DWin, kGZIID_cISC4View3DWin,
                                          reinterpret_cast<void**>(&gView3D));
                }
            }
        }
        return gView3D;
    }

    bool OverlayShouldShow() {
        const std::lock_guard lock(gView3DMutex);
        cISC4View3DWin* const view = EnsureView3DLocked();
        return view != nullptr && IsNetworkControlDrivingPowerTool(view->GetCurrentViewInputControl());
    }

    // Does a style define any orientation of the given FAR ratio?
    bool StyleDefinesRatio(const PoleStyle& style, const uint32_t ratioIndex) {
        for (uint32_t orient = 0; orient < kFarOrientCount; ++orient) {
            if (style.hasFarHeading[ratioIndex * kFarOrientCount + orient]) {
                return true;
            }
        }
        return false;
    }

    // The active style's supported FAR ratios: the style's own headings plus the [PowerPoles.FAR]
    // default fallback (which applies under any style, vanilla included).
    bool ActiveStyleSupportsRatio(const uint32_t ratioIndex) {
        if (ratioIndex >= kFarRatios.size()) {
            return false;
        }
        if (gActiveStyleIndex >= 1 && gActiveStyleIndex <= gStyles.size() &&
            StyleDefinesRatio(gStyles[gActiveStyleIndex - 1], ratioIndex)) {
            return true;
        }
        return gHasFarDefault && StyleDefinesRatio(gFarDefaultStyle, ratioIndex);
    }

    // ImGui render callback. Only invoked by the ImGui service (so imgui.dll is guaranteed present
    // by the time any ImGui:: symbol below is touched). Styled after sc4-plop-and-paint's
    // PaintStatusPanel: borderless, auto-sized, click-through, top-left.
    void RenderPoleStyleOverlay(void* /*userData*/) {
        if (!OverlayShouldShow()) {
            return;
        }

        constexpr float kMargin = 10.0f;
        ImGui::SetNextWindowPos(ImVec2(kMargin, kMargin), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.7f);
        constexpr ImGuiWindowFlags kFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoMove;
        if (!ImGui::Begin("##PoleStyle", nullptr, kFlags)) {
            ImGui::End();
            return;
        }

        if (gActiveStyleIndex == 0) {
            ImGui::TextUnformatted("Style: Vanilla");
        } else {
            ImGui::Text("Style: %s (%u / %u)", gStyles[gActiveStyleIndex - 1].name.c_str(),
                        gActiveStyleIndex, static_cast<uint32_t>(gStyles.size()));
        }

        const uint32_t interPole = (gActiveStyleIndex >= 1 && gActiveStyleIndex <= gStyles.size() &&
                                    gStyles[gActiveStyleIndex - 1].hasMaxCells)
            ? gStyles[gActiveStyleIndex - 1].maxCellsBetweenPoles
            : gLastObservedVanillaInterPoleDistance;
        ImGui::Text("Poles every %u cells", interPole);

        // Best-effort live heading: reflects the most recent snap while a drag is/was in progress.
        if (gFarSnapAnchorValid) {
            if (gFarSnapCandidate == kAxisSnapCandidate) {
                ImGui::TextUnformatted("Heading: orthogonal");
            } else if (gFarSnapCandidate == kDiagonalSnapCandidate) {
                ImGui::TextUnformatted("Heading: 45 deg diagonal");
            } else if (gFarDragActive) {
                const FarRatio& r = kFarRatios[gFarDragRatioIndex];
                const double deg = std::atan2(static_cast<double>(r.rise), static_cast<double>(r.run)) *
                                   180.0 / 3.14159265358979323846;
                const std::string_view orient = FarOrientName(gFarDragOrient);
                ImGui::Text("Heading: FA-%s  %.1f deg (%.*s)",
                            std::string(kFarRatioLabels[gFarDragRatioIndex]).c_str(), deg,
                            static_cast<int>(orient.size()), orient.data());
            }
        }

        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::TextUnformatted("Shift: regular headings only");
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextUnformatted("FA:");
        bool anyRatio = false;
        for (uint32_t ri = 0; ri < kFarRatios.size(); ++ri) {
            if (!ActiveStyleSupportsRatio(ri)) {
                continue;
            }
            anyRatio = true;
            ImGui::SameLine();
            const bool activeRatio = gFarDragActive && gFarSnapAnchorValid && gFarDragRatioIndex == ri;
            if (activeRatio) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
            }
            ImGui::Text("FA-%s", std::string(kFarRatioLabels[ri]).c_str());
            if (activeRatio) {
                ImGui::PopStyleColor();
            }
        }
        if (!anyRatio) {
            ImGui::SameLine();
            ImGui::TextUnformatted("none");
        }
        ImGui::TextUnformatted("Tab / Shift+Tab  style");
        ImGui::PopStyleColor();

        ImGui::End();
    }

    // ------------------------------------------------------------------
    // Settings (SC4PowerPoleCustomization.ini, same layout convention as SC4TerrainDiagonalFix.ini).
    // ------------------------------------------------------------------
    std::string_view TrimIniScalar(std::string_view text) {
        if (const size_t comment = text.find_first_of(";#"); comment != std::string_view::npos) {
            text = text.substr(0, comment);
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
            text.remove_prefix(1);
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
            text.remove_suffix(1);
        }
        return text;
    }

    bool TryParseBool(const std::string& value, bool& result) {
        std::string text(TrimIniScalar(value));
        std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (text == "true" || text == "1" || text == "yes") {
            result = true;
            return true;
        }
        if (text == "false" || text == "0" || text == "no") {
            result = false;
            return true;
        }
        return false;
    }

    bool TryParseUInt32(const std::string& value, uint32_t& result) {
        const std::string_view text = TrimIniScalar(value);
        result = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
        return !text.empty() && ec == std::errc() && ptr == text.data() + text.size();
    }

    // Instance IDs remain hexadecimal values even though direction masks now use named REG.* keys.
    bool TryParseHexUInt32(const std::string& value, uint32_t& result) {
        std::string_view text = TrimIniScalar(value);
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text.remove_prefix(2);
        }
        result = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result, 16);
        return !text.empty() && ec == std::errc() && ptr == text.data() + text.size();
    }

    std::filesystem::path GetDllDirectoryPath() {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetDllDirectoryPath), &module)) {
            return {};
        }
        std::vector<wchar_t> path(512);
        constexpr size_t kMaximumPathCharacters = 32768;
        while (true) {
            const DWORD copied = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (copied == 0) {
                return {};
            }
            if (copied < path.size() - 1) {
                return std::filesystem::path(path.data(), path.data() + copied).parent_path();
            }
            if (path.size() >= kMaximumPathCharacters) {
                break;
            }
            path.resize(std::min(path.size() * 2, kMaximumPathCharacters));
        }
        LOG_ERROR("PowerPoleCustomization: DLL path exceeds the Windows maximum; settings disabled.");
        return {};
    }

    // [PowerPoles.<Name>] sections use named REG.* connection combinations. The names below are in
    // engine mask order, so parsing compiles the author-facing key directly into the existing
    // 16-entry lookup table. mINI makes keys case-insensitive. A section may be sparse -- masks it
    // doesn't define fall back to the vanilla instance for that mask at resolve time, not here.
    constexpr std::array<std::string_view, 16> kRegularDirectionKeys = {{
        "reg.none", "reg.x", "reg.dp", "reg.x+dp",
        "reg.z", "reg.x+z", "reg.dp+z", "reg.x+dp+z",
        "reg.dn", "reg.x+dn", "reg.dp+dn", "reg.x+dp+dn",
        "reg.z+dn", "reg.x+z+dn", "reg.dp+z+dn", "reg.x+dp+z+dn",
    }};

    // FAR heading keys, index = ratioIndex * kFarOrientCount + orient (same layout as
    // PoleStyle::farInstanceByHeading and the gFarDragRatioIndex/gFarDragOrient pair). Ratio order
    // matches kFarRatios (1.5, 2, 3, 6); orientation order matches the FarOrient enum (XP,XN,ZP,ZN).
    constexpr std::array<std::string_view, kFarHeadingCount> kFarHeadingKeys = {{
        "far-1.5.xp", "far-1.5.xn", "far-1.5.zp", "far-1.5.zn",
        "far-2.xp",   "far-2.xn",   "far-2.zp",   "far-2.zn",
        "far-3.xp",   "far-3.xn",   "far-3.zp",   "far-3.zn",
        "far-6.xp",   "far-6.xn",   "far-6.zp",   "far-6.zn",
    }};

    // Parses every FAR-<ratio>.<orient> key present in a section into the style's FAR table. Returns
    // the number of valid keys read. Shared by named styles and the [PowerPoles.FAR] default section.
    uint32_t ParseFarHeadingKeys(const mINI::INIMap<std::string>& section, PoleStyle& style,
                                 const std::string& sectionLabel) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < kFarHeadingCount; ++i) {
            const std::string key(kFarHeadingKeys[i]);
            if (!section.has(key)) {
                continue;
            }
            uint32_t instanceId = 0;
            if (!TryParseHexUInt32(section.get(key), instanceId) || instanceId == 0) {
                LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] {} must be a nonzero hexadecimal "
                         "instance ID; ignoring \"{}\".", sectionLabel, key, section.get(key));
                continue;
            }
            style.farInstanceByHeading[i] = instanceId;
            style.hasFarHeading[i] = true;
            ++count;
        }
        return count;
    }

    // Warns about far-* keys that aren't one of the 16 canonical headings (typo/wrong ratio/orient).
    void WarnUnknownFarKeys(const mINI::INIMap<std::string>& section, const std::string& sectionLabel) {
        for (const auto& entry : section) {
            const std::string& key = entry.first;
            if (key.starts_with("far-") &&
                std::ranges::find(kFarHeadingKeys, key) == kFarHeadingKeys.end()) {
                LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] unknown FAR heading key {}; expected "
                         "far-<1.5|2|3|6>.<xp|xn|zp|zn>. Ignoring it.", sectionLabel, key);
            }
        }
    }

    void LoadPoleStyles(const mINI::INIStructure& ini) {
        gStyles.clear();
        gFarDefaultStyle = PoleStyle{};
        gHasFarDefault = false;
        constexpr std::string_view kPrefix = "powerpoles.";
        for (const auto& [sectionName, section] : ini) {
            std::string lowerName = sectionName;
            std::ranges::transform(lowerName, lowerName.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowerName.size() <= kPrefix.size() || lowerName.compare(0, kPrefix.size(), kPrefix) != 0) {
                continue;
            }

            const std::string styleName = sectionName.substr(kPrefix.size());

            // [PowerPoles.FAR] is not a cyclable style: it's the style-independent FAR fallback.
            // Parse only its FAR headings and never push it into gStyles.
            if (lowerName == "powerpoles.far") {
                const uint32_t farCount = ParseFarHeadingKeys(section, gFarDefaultStyle, styleName);
                WarnUnknownFarKeys(section, styleName);
                for (const auto& entry : section) {
                    std::string key = entry.first;
                    std::ranges::transform(key, key.begin(), [](const unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    if (!key.starts_with("far-")) {
                        LOG_WARN("PowerPoleCustomization: [PowerPoles.FAR] unknown key {}; only FAR-* "
                                 "heading keys are valid here.", entry.first);
                    }
                }
                if (farCount > 0) {
                    gHasFarDefault = true;
                    LOG_INFO("PowerPoleCustomization: loaded [PowerPoles.FAR] default ({} of {} FAR "
                             "headings defined).", farCount, kFarHeadingCount);
                } else {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.FAR] defines no valid FAR headings; "
                             "ignoring the section.");
                }
                continue;
            }

            PoleStyle style;
            style.configName = styleName;
            style.name = styleName;
            if (section.has("DisplayName")) {
                const std::string configuredName = section.get("DisplayName");
                const std::string_view displayName = TrimIniScalar(configuredName);
                if (displayName.empty()) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] DisplayName is empty; using "
                             "the normalized section name.", styleName);
                } else {
                    // Preserve the full byte sequence; blindly truncating a local UTF-8 name can
                    // split a code point and feed invalid text to ImGui/logging.
                    style.name.assign(displayName);
                }
            }
            uint32_t definedCount = 0;
            for (uint32_t mask = 0; mask < 16; ++mask) {
                const std::string key(kRegularDirectionKeys[mask]);
                if (section.has(key)) {
                    uint32_t instanceId = 0;
                    if (!TryParseHexUInt32(section.get(key), instanceId) || instanceId == 0) {
                        LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] {} must be a nonzero "
                                 "hexadecimal instance ID; ignoring \"{}\".", style.name, key,
                                 section.get(key));
                        continue;
                    }
                    style.instanceByDirectionMask[mask] = instanceId;
                    style.hasMask[mask] = true;
                    ++definedCount;
                }
            }

            const uint32_t farCount = ParseFarHeadingKeys(section, style, style.name);

            for (const auto& entry : section) {
                std::string key = entry.first;
                std::ranges::transform(key, key.begin(), [](const unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (key.starts_with("0x")) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] legacy mask key {} is unsupported; "
                             "use a named REG.* key.", style.name, entry.first);
                } else if (key.starts_with("reg.") &&
                           std::ranges::find(kRegularDirectionKeys, key) == kRegularDirectionKeys.end()) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] unknown or non-canonical regular "
                             "direction key {}; ignoring it.", style.name, entry.first);
                } else if (!key.starts_with("far-") && key != "interpoledistance" &&
                           key != "displayname" &&
                           std::ranges::find(kRegularDirectionKeys, key) == kRegularDirectionKeys.end()) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] unknown key {}; ignoring it.",
                             style.name, entry.first);
                }
            }
            WarnUnknownFarKeys(section, style.name);
            if (section.has("InterPoleDistance")) {
                const std::string value = section.get("InterPoleDistance");
                uint32_t parsed = 0;
                if (!TryParseUInt32(value, parsed)) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] InterPoleDistance has invalid "
                             "decimal value \"{}\"; leaving spacing inherited.", style.name, value);
                } else {
                    const uint32_t clamped = std::clamp(parsed, 1u, kMaxInterPoleDistance);
                    if (clamped != parsed) {
                        LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] InterPoleDistance {} is outside "
                                 "1..{}; clamping to {}.", style.name, parsed, kMaxInterPoleDistance, clamped);
                    }
                    style.maxCellsBetweenPoles = clamped;
                    style.hasMaxCells = true;
                }
            }

            if (definedCount == 0 && farCount == 0 && !style.hasMaxCells) {
                LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] defines no valid REG.*/FAR.* keys or "
                         "InterPoleDistance; skipping.", style.name);
                continue;
            }
            LOG_INFO("PowerPoleCustomization: loaded pole style \"{}\" ({} of 16 direction masks, {} of {} "
                     "FAR headings defined; rest fall back; InterPoleDistance={}).", style.name, definedCount,
                     farCount, kFarHeadingCount,
                     style.hasMaxCells ? std::to_string(style.maxCellsBetweenPoles) : std::string("vanilla"));
            gStyles.push_back(std::move(style));
        }
    }

    void LoadSettings() {
        gSettings = {};
        gStyles.clear();
        gFarDefaultStyle = PoleStyle{};
        gHasFarDefault = false;
        gActiveStyleIndex = 0;
        const auto dllDirectory = GetDllDirectoryPath();
        if (dllDirectory.empty()) {
            LOG_ERROR("PowerPoleCustomization: could not resolve the DLL directory; using defaults "
                      "and refusing to read a relative INI path.");
            return;
        }
        const auto settingsPath = dllDirectory / "SC4PowerPoleCustomization.ini";
        // Pass the filesystem path through unchanged so non-ASCII DLL directories work on Windows.
        mINI::INIFile file(settingsPath);
        mINI::INIStructure ini;
        if (!file.read(ini)) {
            LOG_INFO("PowerPoleCustomization: using default settings; no readable INI beside the DLL.");
            return;
        }

        if (ini.has("SC4PowerPoleCustomization")) {
            const auto section = ini.get("SC4PowerPoleCustomization");
            if (section.has("Enabled")) {
                const std::string value = section.get("Enabled");
                bool parsed = gSettings.enabled;
                if (TryParseBool(value, parsed)) {
                    gSettings.enabled = parsed;
                } else {
                    LOG_WARN("PowerPoleCustomization: Enabled has invalid boolean value \"{}\"; "
                             "keeping default {}.", value, gSettings.enabled);
                }
            }
            if (section.has("MaxPointsPerDirection")) {
                const std::string value = section.get("MaxPointsPerDirection");
                uint32_t parsed = 0;
                if (!TryParseUInt32(value, parsed)) {
                    LOG_WARN("PowerPoleCustomization: MaxPointsPerDirection has invalid decimal value "
                             "\"{}\"; keeping default {}.", value, gSettings.maxPointsPerDirection);
                } else {
                    const uint32_t clamped = std::clamp(parsed, 1u, 15u);
                    if (clamped != parsed) {
                        LOG_WARN("PowerPoleCustomization: MaxPointsPerDirection {} is outside 1..15; "
                                 "clamping to {}.", parsed, clamped);
                    }
                    gSettings.maxPointsPerDirection = clamped;
                }
            }
            for (const auto& entry : section) {
                std::string key = entry.first;
                std::ranges::transform(key, key.begin(), [](const unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (key != "enabled" && key != "maxpointsperdirection") {
                    LOG_WARN("PowerPoleCustomization: [SC4PowerPoleCustomization] unknown key {}; "
                             "ignoring it.", entry.first);
                }
            }
        } else {
            LOG_INFO("PowerPoleCustomization: [{}] is absent; using core defaults and still loading "
                     "PowerPoles.* style sections.", "SC4PowerPoleCustomization");
        }

        LOG_INFO("PowerPoleCustomization: settings enabled={}, maxPointsPerDirection={}",
                 gSettings.enabled, gSettings.maxPointsPerDirection);

        LoadPoleStyles(ini);
    }

    // CreatePowerPole does not reliably recover from a style ID whose prop exemplar is absent: it
    // can allocate the occupant and then receive a null exemplar. Validate every configured model
    // after the application's resource index is ready, before enabling the lookup hooks.
    void ValidatePoleStyleResources() {
        const cIGZPersistResourceManagerPtr resourceManager;
        const auto validateStyle = [&resourceManager](PoleStyle& style, const std::string& label) {
            uint32_t validModels = 0;
            for (uint32_t mask = 0; mask < style.hasMask.size(); ++mask) {
                if (!style.hasMask[mask]) {
                    continue;
                }
                const uint32_t instanceId = style.instanceByDirectionMask[mask];
                const cGZPersistResourceKey key(
                    kExemplarTypeId, kVanillaPowerPoleGroupId, instanceId);
                if (!resourceManager || !resourceManager->TestForKey(key)) {
                    LOG_WARN("PowerPoleCustomization: [{}] {} references missing prop exemplar "
                             "0x{:08X} in group 0x{:08X}; disabling that model key.", label,
                             kRegularDirectionKeys[mask], instanceId, kVanillaPowerPoleGroupId);
                    style.hasMask[mask] = false;
                    style.instanceByDirectionMask[mask] = 0;
                } else {
                    ++validModels;
                }
            }
            for (uint32_t heading = 0; heading < style.hasFarHeading.size(); ++heading) {
                if (!style.hasFarHeading[heading]) {
                    continue;
                }
                const uint32_t instanceId = style.farInstanceByHeading[heading];
                const cGZPersistResourceKey key(
                    kExemplarTypeId, kVanillaPowerPoleGroupId, instanceId);
                if (!resourceManager || !resourceManager->TestForKey(key)) {
                    LOG_WARN("PowerPoleCustomization: [{}] {} references missing prop exemplar "
                             "0x{:08X} in group 0x{:08X}; disabling that model key.", label,
                             kFarHeadingKeys[heading], instanceId, kVanillaPowerPoleGroupId);
                    style.hasFarHeading[heading] = false;
                    style.farInstanceByHeading[heading] = 0;
                } else {
                    ++validModels;
                }
            }
            return validModels;
        };

        validateStyle(gFarDefaultStyle, "PowerPoles.FAR");
        gHasFarDefault = std::ranges::any_of(gFarDefaultStyle.hasFarHeading,
                                             [](const bool present) { return present; });
        for (PoleStyle& style : gStyles) {
            validateStyle(style, "PowerPoles." + style.configName);
        }
        std::erase_if(gStyles, [](const PoleStyle& style) {
            const bool hasRegular = std::ranges::any_of(style.hasMask,
                                                        [](const bool present) { return present; });
            const bool hasFar = std::ranges::any_of(style.hasFarHeading,
                                                    [](const bool present) { return present; });
            return !hasRegular && !hasFar && !style.hasMaxCells;
        });
        if (gActiveStyleIndex > gStyles.size()) {
            gActiveStyleIndex = 0;
        }
    }

    // City teardown: every pole is destroyed (the destructor hook empties the side tables
    // per-pole), the engine's texture registry is torn down by its own static shutdown, and the
    // View3D window this DLL cached for overlay visibility is destroyed with the city. Belt and
    // suspenders: drop everything city-scoped here so nothing stale survives into the next city,
    // whatever destruction order the engine picked.
    void FlushCityScopedState() {
        gCityActive.store(false, std::memory_order_release);
        gPendingLoadedPoles.clear();
        gOverrides.clear();
        gPolylineWidthOverrides.clear();
        gConnectionTextureOverrides.clear();
        gWaterBallSizeOverrides.clear();
        gRegisteredPoleTextures.clear();
        gFarDragActive = false;
        gFarSnapAnchorValid = false;
        gLastObservedVanillaInterPoleDistance = 10;
        {
            const std::lock_guard lock(gView3DMutex);
            if (gView3D != nullptr) {
                gView3D->Release();
                gView3D = nullptr;
            }
        }
        LOG_DEBUG("PowerPoleCustomization: city-scoped state flushed on city shutdown.");
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
        gRuntimeHooksEnabled = false;
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

        ValidatePoleStyleResources();

        size_t installed = 0;
        installed += InstallCallSitePatch(modelInstanceIdPatch_, "GetModelInstanceID@LoadModel",
                                           kGetModelInstanceID_CallSite_LoadModel, kGetModelInstanceID,
                                           &GetModelInstanceIDHook);
        installed += InstallCallSitePatch(initConnectionPointsPatches_[0], "InitConnectionPoints@Init",
                                           kInitConnectionPoints_CallSite_Init, kInitConnectionPoints,
                                           &InitConnectionPointsHook);
        installed += InstallCallSitePatch(initConnectionPointsPatches_[1], "InitConnectionPoints@SetDefaultExemplar",
                                           kInitConnectionPoints_CallSite_SetDefaultExemplar, kInitConnectionPoints,
                                           &InitConnectionPointsHook);
        installed += InstallCallSitePatch(initConnectionPointsPatches_[2], "InitConnectionPoints@Read",
                                           kInitConnectionPoints_CallSite_Read, kInitConnectionPoints,
                                           &InitConnectionPointsReadHook);
        installed += InstallCallSitePatch(destructorPatch_, "Destructor@DeletingDtor",
                                           kDestructor_CallSite_DeletingDtor, kDestructor,
                                           &DestructorHook);

        for (size_t i = 0; i < kAddConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(addConnectionPatches_[i], "AddConnection@PlacePoles",
                                               kAddConnectionCallSites[i], kAddConnection, &AddConnectionHook);
        }
        for (size_t i = 0; i < kUpdateConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(updateConnectionPatches_[i], "UpdateConnection@SetDefaultExemplar",
                                               kUpdateConnectionCallSites[i], kUpdateConnection, &UpdateConnectionHook);
        }
        for (size_t i = 0; i < kRemoveConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(removeConnectionPatches_[i], "RemoveConnection",
                                               kRemoveConnectionCallSites[i], kRemoveConnection,
                                               &RemoveConnectionHook);
        }
        installed += InstallCallSitePatch(removeConnectionEntryPatch_, "RemoveConnectionEntry@BreakAll",
                                           kRemoveConnectionEntry_CallSite_BreakAll, kRemoveConnectionEntry,
                                           &RemoveConnectionEntryHook);
        for (size_t i = 0; i < kDeterminePolePositionsCallSites.size(); ++i) {
            installed += InstallCallSitePatch(determinePolePositionsPatches_[i], "DeterminePolePositions",
                                               kDeterminePolePositionsCallSites[i], kDeterminePolePositions,
                                               &DeterminePolePositionsHook);
        }
        for (size_t i = 0; i < kPlacePolesTransactionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(placePolesTransactionPatches_[i],
                                               "PlacePoles@transaction-end",
                                               kPlacePolesTransactionCallSites[i], kPlacePoles,
                                               &PlacePolesHook);
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
        installed += wireStrandTexturePatch_.Install(kWireStrandTextureSite, kVanillaWireStrandTextureId,
                                                       &WireStrandTextureHook, "WireStrandTexture",
                                                       kMovEsiOpcode) ? 1 : 0;
        installed += wireBuoyTexturePatch_.Install(kWireBuoyTextureSite, kVanillaWireBuoyTextureId,
                                                     &WireBuoyTextureHook, "WireBuoyTexture",
                                                     kMovEsiOpcode) ? 1 : 0;
        installed += waterBallSizePatch_.Install(kWaterBallSizeSite,
            {0xa1, 0x98, 0x67, 0xb4, 0x00, 0xd9, 0x04, 0x90, 0x8d, 0x04, 0x90},
            &WaterBallSizeHook, "WaterBallSize") ? 1 : 0;

        installed += poleStyleLookupPatch1_.Install(kPoleStyleLookupSite1,
            {0x8b, 0x04, 0xbd, 0xb0, 0x67, 0xb4, 0x00}, &PoleStyleLookupHook, "PoleStyleLookup@1") ? 1 : 0;
        installed += poleStyleLookupPatch2_.Install(kPoleStyleLookupSite2,
            {0x8b, 0x04, 0xbd, 0xb0, 0x67, 0xb4, 0x00}, &PoleStyleLookupHook, "PoleStyleLookup@2") ? 1 : 0;

        // FAR drag (docs SS17). A vtable-slot swap, not a code patch.
        const bool farInstalled = farDrawNetworkLinePatch_.Install(kPowerToolDrawNetworkLineSlot, kDrawNetworkLine,
                                                                    &FarDrawNetworkLineHook, "FarDrawNetworkLine");
        installed += farInstalled ? 1 : 0;
        installed += keepWiresPatch_.Install(kUpdateOnZoneChange,
            {0x83, 0xec, 0x68, 0x8b, 0x44, 0x24, 0x7c}, &KeepWiresOnZoneChangeHook,
            "KeepWiresOnZoneChange") ? 1 : 0;

        // Tab / Shift-Tab pole-style switch. Byte-validates the OnKeyDown slot, which transitively
        // confirms the network-tool input-control vtable the overlay's power-tool check also reads.
        const bool keyHookInstalled = onKeyDownPatch_.Install(kOnKeyDownSlot, kOnKeyDown,
                                                              &OnKeyDownHook, "OnKeyDown@NetworkToolInputControl");
        installed += keyHookInstalled ? 1 : 0;
        styleUiValidated_ = keyHookInstalled && farInstalled;

        const size_t expectedPatches =
            1 + // model-instance selector
            initConnectionPointsPatches_.size() +
            1 + // pole destructor
            kAddConnectionCallSites.size() +
            kUpdateConnectionCallSites.size() +
            kRemoveConnectionCallSites.size() +
            1 + // BreakAll's inner connection-entry removal
            kDeterminePolePositionsCallSites.size() +
            kPlacePolesTransactionCallSites.size() +
            1 + // per-wire width lookup
            createFloorPatches_.size() +
            4 + // floor/wall texture hashes
            2 + // strand/buoy texture selectors
            1 + // water-buoy size
            2 + // regular/FAR pole-style selectors
            1 + // FAR DrawNetworkLine vtable slot
            1 + // keepwires zone handler
            1;  // Tab/Shift-Tab input slot
        LOG_INFO("PowerPoleCustomization: installed {} of {} patches.", installed, expectedPatches);
        if (installed != expectedPatches) {
            // The hooks share side tables and several byte sites form indivisible hash/lookup pairs.
            // A partially installed set is less safe than disabling this DLL for an unknown binary.
            LOG_ERROR("PowerPoleCustomization: incomplete patch set; rolling back all installed hooks.");
            PinModuleIfHooksRemain(UninstallPatches());
            return true;
        }

        cIGZMessageServer2Ptr pMS2;
        if (!pMS2 || !pMS2->AddNotification(this, kSC4MessagePostCityInit)) {
            LOG_ERROR("PowerPoleCustomization: PostCityInit notification is unavailable; rolling "
                      "back because saved connection metadata could not be restored safely.");
            PinModuleIfHooksRemain(UninstallPatches());
            return true;
        }
        if (!pMS2->AddNotification(this, kSC4MessagePreCityShutdown)) {
            pMS2->RemoveNotification(this, kSC4MessagePostCityInit);
            LOG_ERROR("PowerPoleCustomization: PreCityShutdown notification is unavailable; rolling "
                      "back because the retained view and city-scoped caches cannot cross cities.");
            PinModuleIfHooksRemain(UninstallPatches());
            return true;
        }
        messageServer_ = pMS2;
        gRuntimeHooksEnabled = true;

        const cISC4AppPtr app;
        if (app) {
            if (auto* const cheats = app->GetCheatCodeManager()) {
                if (cheats->RegisterCheatCode(kCheatKeepWires, cRZBaseString("keepwires"))) {
                    if (cheats->AddNotification2(this, 0)) {
                        cheatManager_ = cheats;
                        LOG_INFO("PowerPoleCustomization: cheat code registered (keepwires) -- toggles "
                                 "suppression of power-line rerouting on zone changes.");
                    } else {
                        cheats->UnregisterCheatCode(kCheatKeepWires);
                        LOG_WARN("PowerPoleCustomization: keepwires notification subscription failed; "
                                 "the cheat was unregistered.");
                    }
                } else {
                    LOG_WARN("PowerPoleCustomization: failed to register cheat code keepwires.");
                }
            }
        }

        SetupStatusOverlay();
        return true;
    }

    bool PostAppShutdown() override {
        // Fail closed before touching patch bytes: a site whose protection/ownership prevents
        // uninstall must continue to delegate to vanilla for the remainder of process shutdown.
        gRuntimeHooksEnabled = false;
        gKeepWiresOnZoneChange = false;
        gCityActive.store(false, std::memory_order_release);
        if (cheatManager_) {
            cheatManager_->UnregisterCheatCode(kCheatKeepWires);
            cheatManager_->RemoveNotification2(this, 0);
            cheatManager_.Reset();
        }
        if (messageServer_) {
            messageServer_->RemoveNotification(this, kSC4MessagePostCityInit);
            messageServer_->RemoveNotification(this, kSC4MessagePreCityShutdown);
            messageServer_ = nullptr;
        }
        TeardownStatusOverlay();
        PinModuleIfHooksRemain(UninstallPatches());
        gPolylineWidthOverrides.clear();
        gConnectionTextureOverrides.clear();
        gWaterBallSizeOverrides.clear();
        gPendingLoadedPoles.clear();
        gRegisteredPoleTextures.clear();
        gOverrides.clear();
        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }
        return true;
    }

    bool DoMessage(cIGZMessage2* pMsg) override {
        try {
            if (pMsg) {
                auto* const stdMsg = static_cast<cIGZMessage2Standard*>(pMsg);
                if (stdMsg->GetType() == kSC4MessagePostCityInit) {
                    gCityActive.store(true, std::memory_order_release);
                    RehydrateLoadedConnections();
                } else if (stdMsg->GetType() == kSC4MessagePreCityShutdown) {
                    FlushCityScopedState();
                } else if (stdMsg->GetType() == kCheatCodeMessageType &&
                           static_cast<uint32_t>(stdMsg->GetData1()) == kCheatKeepWires) {
                    gKeepWiresOnZoneChange = !gKeepWiresOnZoneChange;
                    LOG_INFO("PowerPoleCustomization: keepwires {} -- zone changes {} "
                             "the power-line remove/reposition/rebuild handler.",
                             gKeepWiresOnZoneChange ? "ENABLED" : "disabled",
                             gKeepWiresOnZoneChange ? "will bypass" : "will run");
                }
            }
        } catch (...) {
            LogCurrentHookException("DoMessage");
        }
        return true;
    }

private:
    static size_t InstallCallSitePatch(TerrainDecal::RelativeCallPatch& patch, const char* name,
                                        const uintptr_t callSite, const uintptr_t expectedTarget, void* hookFn) {
        const auto* const site = reinterpret_cast<const uint8_t*>(callSite);
        if (site[0] != 0xe8) {
            LOG_ERROR("PowerPoleCustomization: {} site 0x{:08X} is not a relative CALL; not patching.",
                      name, static_cast<uint32_t>(callSite));
            return 0;
        }
        int32_t originalRelative = 0;
        std::memcpy(&originalRelative, site + 1, sizeof(originalRelative));
        const uintptr_t originalTarget = static_cast<uintptr_t>(
            static_cast<intptr_t>(callSite + 5) + originalRelative);
        if (originalTarget != expectedTarget) {
            LOG_ERROR("PowerPoleCustomization: {} call site 0x{:08X} targets 0x{:08X}, expected "
                      "0x{:08X}; not patching.", name, static_cast<uint32_t>(callSite),
                      static_cast<uint32_t>(originalTarget), static_cast<uint32_t>(expectedTarget));
            return 0;
        }
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

    size_t UninstallPatches() {
        onKeyDownPatch_.Uninstall();
        farDrawNetworkLinePatch_.Uninstall();
        keepWiresPatch_.Uninstall();
        poleStyleLookupPatch2_.Uninstall();
        poleStyleLookupPatch1_.Uninstall();
        waterBallSizePatch_.Uninstall();
        wireBuoyTexturePatch_.Uninstall();
        wireStrandTexturePatch_.Uninstall();
        wallTextureHashPatch2_.Uninstall();
        wallTextureHashPatch1_.Uninstall();
        floorTextureHashPatch2_.Uninstall();
        floorTextureHashPatch1_.Uninstall();
        for (auto& patch : createFloorPatches_) patch.Uninstall();
        wireWidthLookupPatch_.Uninstall();
        for (auto& patch : placePolesTransactionPatches_) patch.Uninstall();
        for (auto& patch : determinePolePositionsPatches_) patch.Uninstall();
        removeConnectionEntryPatch_.Uninstall();
        for (auto& patch : removeConnectionPatches_) patch.Uninstall();
        for (auto& patch : updateConnectionPatches_) patch.Uninstall();
        for (auto& patch : addConnectionPatches_) patch.Uninstall();
        destructorPatch_.Uninstall();
        for (auto& patch : initConnectionPointsPatches_) patch.Uninstall();
        modelInstanceIdPatch_.Uninstall();
        styleUiValidated_ = false;

        // Uninstall methods deliberately retain IsInstalled() when page protection fails or a
        // later mod has taken ownership of the same site. Never hide that partial teardown: an
        // apparently clean rollback with a live callback into this DLL is worse than a loud error.
        size_t remaining = 0;
        const auto countRemaining = [&remaining](const auto& patch) {
            remaining += patch.IsInstalled() ? 1u : 0u;
        };
        countRemaining(onKeyDownPatch_);
        countRemaining(farDrawNetworkLinePatch_);
        countRemaining(keepWiresPatch_);
        countRemaining(poleStyleLookupPatch2_);
        countRemaining(poleStyleLookupPatch1_);
        countRemaining(waterBallSizePatch_);
        countRemaining(wireBuoyTexturePatch_);
        countRemaining(wireStrandTexturePatch_);
        countRemaining(wallTextureHashPatch2_);
        countRemaining(wallTextureHashPatch1_);
        countRemaining(floorTextureHashPatch2_);
        countRemaining(floorTextureHashPatch1_);
        for (const auto& patch : createFloorPatches_) countRemaining(patch);
        countRemaining(wireWidthLookupPatch_);
        for (const auto& patch : placePolesTransactionPatches_) countRemaining(patch);
        for (const auto& patch : determinePolePositionsPatches_) countRemaining(patch);
        countRemaining(removeConnectionEntryPatch_);
        for (const auto& patch : removeConnectionPatches_) countRemaining(patch);
        for (const auto& patch : updateConnectionPatches_) countRemaining(patch);
        for (const auto& patch : addConnectionPatches_) countRemaining(patch);
        countRemaining(destructorPatch_);
        for (const auto& patch : initConnectionPointsPatches_) countRemaining(patch);
        countRemaining(modelInstanceIdPatch_);
        if (remaining != 0) {
            LOG_ERROR("PowerPoleCustomization: {} hook patches remain installed after teardown; see "
                      "the preceding ownership/protection errors.", remaining);
        }
        return remaining;
    }

    static void PinModuleIfHooksRemain(const size_t remaining) noexcept {
        if (remaining == 0) {
            return;
        }
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&PinModuleIfHooksRemain), &module)) {
            try {
                LOG_ERROR("PowerPoleCustomization: pinned this DLL for process lifetime because {} "
                          "hook patches still reference it.", remaining);
            } catch (...) {
            }
        } else {
            try {
                LOG_ERROR("PowerPoleCustomization: failed to pin a DLL still referenced by {} hook "
                          "patches (Windows error {}).", remaining, GetLastError());
            } catch (...) {
            }
        }
    }

    // Acquires the optional ImGui status overlay. Safe no-op when the power-tool identity vtables
    // weren't byte-confirmed at install (recompiled/other binary), or when the ImGui service
    // (SC4RenderServices / imgui.dll) is absent. The pole features and the Tab key hook keep working
    // regardless -- nothing here is on the critical path.
    void SetupStatusOverlay() {
        if (!styleUiValidated_) {
            LOG_INFO("PowerPoleCustomization: power-tool vtables not confirmed on this binary; status "
                     "overlay disabled (Tab switching inactive too).");
            return;
        }

        // View3D is acquired lazily at render time (EnsureView3DLocked) -- it does not exist yet here.
        // Registration only needs the ImGui service, which is available at PostAppInit.
        if (mpFrameWork == nullptr ||
            !mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            imguiService_ = nullptr;
            LOG_INFO("PowerPoleCustomization: ImGui service absent; status overlay disabled "
                     "(Tab style switching still works).");
            return;
        }

        const uint32_t apiVersion = imguiService_->GetApiVersion();
        if (apiVersion != kImGuiServiceApiVersion) {
            LOG_WARN("PowerPoleCustomization: ImGui service API {} is incompatible with required API {}; "
                     "status overlay disabled.", apiVersion, kImGuiServiceApiVersion);
            imguiService_->Release();
            imguiService_ = nullptr;
            return;
        }

        ImGuiPanelDesc desc{};
        desc.id = kStatusPanelId;
        desc.order = 120;
        desc.visible = true;
        desc.on_render = &RenderPoleStyleOverlay;
        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("PowerPoleCustomization: failed to register status overlay panel.");
            imguiService_->Release();
            imguiService_ = nullptr;
            return;
        }
        overlayRegistered_ = true;
        LOG_INFO("PowerPoleCustomization: status overlay registered (ImGui api {}).",
                 apiVersion);
    }

    void TeardownStatusOverlay() {
        if (imguiService_ != nullptr) {
            if (overlayRegistered_) {
                imguiService_->UnregisterPanel(kStatusPanelId);
                overlayRegistered_ = false;
            }
            imguiService_->Release();
            imguiService_ = nullptr;
        }
        {
            const std::lock_guard lock(gView3DMutex);
            if (gView3D != nullptr) {
                gView3D->Release();
                gView3D = nullptr;
            }
        }
    }

    std::array<TerrainDecal::RelativeCallPatch, 3> initConnectionPointsPatches_{};
    TerrainDecal::RelativeCallPatch modelInstanceIdPatch_{};
    TerrainDecal::RelativeCallPatch destructorPatch_{}; // side-table cleanup on pole destruction
    std::array<TerrainDecal::RelativeCallPatch, 4> addConnectionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 2> updateConnectionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 6> removeConnectionPatches_{};
    TerrainDecal::RelativeCallPatch removeConnectionEntryPatch_{};
    std::array<TerrainDecal::RelativeCallPatch, 3> determinePolePositionsPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 2> placePolesTransactionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 3> createFloorPatches_{};
    Imm32CallPatch floorTextureHashPatch1_{};
    Imm32CallPatch floorTextureHashPatch2_{};
    Imm32CallPatch wallTextureHashPatch1_{};
    Imm32CallPatch wallTextureHashPatch2_{};
    Imm32CallPatch wireStrandTexturePatch_{}; // DrawPowerlines strand texture (MOV ESI site)
    Imm32CallPatch wireBuoyTexturePatch_{};   // DrawPowerlines water-crossing texture (MOV ESI site)
    ByteSpanCallPatch waterBallSizePatch_{};  // DrawPowerlines buoy-ball size read (11-byte site)
    ByteSpanCallPatch poleStyleLookupPatch1_{};
    ByteSpanCallPatch poleStyleLookupPatch2_{};
    ByteSpanCallPatch keepWiresPatch_{};
    InlineCallPatch wireWidthLookupPatch_{};
    VTableSlotPatch farDrawNetworkLinePatch_{}; // automatic FAR snapping; hold Shift for regular-only
    VTableSlotPatch onKeyDownPatch_{};         // Tab/Shift-Tab pole-style switch
    bool styleUiValidated_ = false;            // OnKeyDown + FAR vtable slots byte-confirmed at install
    cIGZImGuiService* imguiService_ = nullptr; // optional; null when SC4RenderServices/imgui.dll absent
    bool overlayRegistered_ = false;
    cRZAutoRefCount<cIGZCheatCodeManager> cheatManager_; // holds the keepwires cheat registration
    cRZAutoRefCount<cIGZMessageServer2> messageServer_;  // holds the city-shutdown notification
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
//   2. CLOSED (validated in-game 2026-07-19): public cISCExemplarPropertyHolder access through
//      cSC4PowerPoleOccupant::QueryInterface replaces the old nullptr stub.
//   3. CLOSED: Get0To3Direction confirmed at 0x0061f4c0. ApplyConnectionCustomization computes real
//      direction and strand counts.
//   4. CLOSED (superseded): cSC4PowerLineOccupant's vtable was fully mapped and used to register
//      extra strands as city objects (see docs SS9/SS10). Confirmed in-game (SS13) that this alone
//      doesn't render anything -- removed from the render path. The vtable mapping itself remains
//      documented in docs/sc4-powerline-tool-re.md if city-object/save-load registration is wanted
//      again later; the code for it was deleted from this file (kept unused code out).
//   5. CLOSED (validated in-game 2026-07-19): full 1-N geometry and sag rebuild in the active
//      tConnection regular-polyline vector, including counts below vanilla's fixed four.
//   6. CLOSED (validated in-game 2026-07-19): per-wire width lookup at 0x0064b855. The patch is
//      byte-validated before installation and falls back to vanilla width for unregistered wires.
//   7. CLOSED (2026-07-19): pole lifetime. The complete destructor (0x0064dfc0) is hooked at its
//      sole call site inside the scalar deleting destructor (0x0064e383), so side-table entries
//      die with their pole; a PreCityShutdown flush clears all remaining city-scoped state.
//   8. CLOSED (2026-07-19): custom foundation texture IDs are validated against the resource
//      manager and registered with the engine's texture registry before use (StaticInit only
//      registers the four vanilla IDs; an unregistered ID null-derefs in Draw).
// ------------------------------------------------------------------
