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

#include "cIGZCOM.h"
#include "cIGZCheatCodeManager.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2.h"
#include "cIGZMessage2Standard.h"
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

    // Experimental, deliberately runtime-scoped switch: while enabled, the lot developer's
    // ClearLotBlockingObjects loop leaves power-pole and hidden power-line occupants alone.
    constexpr uint32_t kCheatKeepWires = 0xB07E1002;
    bool gKeepWiresOnZoneChange = false;

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

    // ------------------------------------------------------------------
    // TEMPORARY PoC-ONLY: FAR (fractional-angle) power-line dragging. See
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
    // one step per FAR period whose FIRST cell is the period's integer grid node, plus a final
    // single-cell step for the last node, with this+0x74 == this+0x70 (all steps "straight", so
    // GetPrimaryCell returns each step's first cell). DeterminePolePositionsHook scopes
    // this+0x3a4 = 1 for the same drag, so vanilla DeterminePolePositions forces a pole at every
    // step's primary cell -- exactly the FAR nodes -- and nothing else (docs SS17.C).
    //
    // Allocation: the game's step/cell vectors use the game allocator, so the hook first runs the
    // ORIGINAL DrawNetworkLine with a fake straight drag of exactly the needed cell count (vanilla
    // performs all allocation), then rewrites the POD contents in place and shrinks the end
    // pointers. Any capacity/expectation mismatch falls back to a plain vanilla call.
    // ------------------------------------------------------------------
    constexpr uintptr_t kDrawNetworkLine = 0x0063af40;
    constexpr uintptr_t kPowerToolDrawNetworkLineSlot = 0x00aa9f78; // dword inside vtable 0x00aa9f30
    constexpr uint32_t kTool_StepsBegin = 0x54;   // vector<tDraggedStep> begin ptr (stride 0xc)
    constexpr uint32_t kTool_StepsEnd = 0x58;
    constexpr uint32_t kTool_CellsBegin = 0x60;   // vector<SC4Point<uint>> begin ptr (stride 8)
    constexpr uint32_t kTool_CellsEnd = 0x64;
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
        if (ShouldInvertModelQuarterTurn(occupant)) {
            quarterTurn ^= 1u;
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

        // Attach basis is only meaningful alongside custom attach points, so it deliberately does
        // NOT set `any`: a pole that declares a basis but no attach points has nothing to rotate and
        // stays fully vanilla. Stored normalized to [0, pi) to match ComputeSpanBearing's range.
        float basisDegrees = 0.0f;
        if (TryReadFiniteFloatProperty(holder, kPropAttachBasisDegrees, basisDegrees)) {
            constexpr float pi = 3.14159265358979323846f;
            float basisRadians = std::fmod(basisDegrees * (pi / 180.0f), pi);
            if (basisRadians < 0.0f) {
                basisRadians += pi;
            }
            out.attachBasisRadians = basisRadians;
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
        // Off-nominal bearing (e.g. a FAR span) forces a rebuild so each pole's baked attach offsets
        // can be yaw-rotated onto the true span direction. A single shared delta is wrong when the
        // two endpoints were authored in different bases (a FAR-authored pole meeting a fallback or
        // vanilla one), so compute one deviation per endpoint. Both are 0 for vanilla-aligned spans,
        // so this reduces exactly to the previous single-delta behavior there.
        float spanBearing = 0.0f;
        const bool haveBearing = ComputeSpanBearing(occupant, otherPole, spanBearing);
        const float yawA = haveBearing ? WrapYaw(spanBearing - AttachBasisAngle(occupant, direction)) : 0.0f;
        const float yawB = haveBearing ? WrapYaw(spanBearing - AttachBasisAngle(otherPole, direction)) : 0.0f;
        const bool rebuildGeometry = HasAttachPointCustomization(occupant, direction) ||
                                     HasAttachPointCustomization(otherPole, direction) ||
                                     (appearance != nullptr && appearance->hasSagCustomization) ||
                                     std::fabs(yawA) > kYawEpsilonRadians ||
                                     std::fabs(yawB) > kYawEpsilonRadians;
        if (rebuildGeometry) {
            ClearConnectionPolylines(entry);
            for (uint32_t i = 0; i < count; ++i) {
                const AttachPoint a = GetAttachPoint(occupant, direction, i, yawA);
                const AttachPoint b = GetAttachPoint(otherPole, direction, i, yawB);
                const float sagScale = appearance ? appearance->SagScale(i) : 1.0f;
                const float maximumSag = appearance ? appearance->MaximumSag(i) :
                                                      std::numeric_limits<float>::infinity();
                AppendPolylineToConnection(entry, a, b, sagScale, maximumSag);
            }
            LOG_DEBUG("PowerPoleCustomization: rebuilt {} strands in direction {} for connection {} -> {}.",
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
        // A clean lone-diagonal pole placed during an active FAR drag routes to the FAR model for
        // the drag's exact (ratio, orientation). Junctions/merges (multi-bit masks) and every
        // regular drag stay on the regular path -- FAR buckets apply only to clean two-connection
        // mid-line poles (docs/far-power-lines-design.md). Any FAR span classifies as diagonal
        // (Get0To3Direction returns 1 or 3), so its lone-direction mask is exactly 0x2 or 0x8.
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
            site_ = site;
            size_ = static_cast<uint32_t>(expectedBytes.size());
            auto* const p = reinterpret_cast<uint8_t*>(site);
            if (!std::equal(expectedBytes.begin(), expectedBytes.end(), p)) {
                LOG_ERROR("PowerPoleCustomization: {} bytes at 0x{:08X} do not match 1.1.641; not patching.",
                          name, static_cast<uint32_t>(site));
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(p, size_, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("PowerPoleCustomization: VirtualProtect failed for {} (error {}).", name, GetLastError());
                return false;
            }
            original_.assign(p, p + size_);
            const auto relative = static_cast<int32_t>(
                reinterpret_cast<intptr_t>(hookFn) - (static_cast<intptr_t>(site) + 5));
            p[0] = 0xe8;
            std::memcpy(p + 1, &relative, sizeof(relative));
            std::fill(p + 5, p + size_, static_cast<uint8_t>(0x90));
            FlushInstructionCache(GetCurrentProcess(), p, size_);
            DWORD ignored = 0;
            VirtualProtect(p, size_, oldProtect, &ignored);
            installed_ = true;
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const p = reinterpret_cast<uint8_t*>(site_);
            DWORD oldProtect = 0;
            if (VirtualProtect(p, size_, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                std::memcpy(p, original_.data(), original_.size());
                FlushInstructionCache(GetCurrentProcess(), p, size_);
                DWORD ignored = 0;
                VirtualProtect(p, size_, oldProtect, &ignored);
            }
            installed_ = false;
        }

    private:
        uintptr_t site_ = 0;
        uint32_t size_ = 0;
        std::vector<uint8_t> original_;
        bool installed_ = false;
    };

    // Replaces one function pointer inside a vtable (a 4-byte data write, not a code patch).
    // Used for the power tool's DrawNetworkLine slot: the method is only ever dispatched
    // virtually, so swapping cSC4PowerLineTool's own slot hooks that tool exclusively -- other
    // network tools dispatch through their own untouched vtables.
    class VTableSlotPatch final {
    public:
        bool Install(const uintptr_t slotAddress, const uintptr_t expectedTarget, void* hookFn, const char* name) {
            slot_ = slotAddress;
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
            *slot = reinterpret_cast<uintptr_t>(hookFn);
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(uintptr_t), oldProtect, &ignored);
            installed_ = true;
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const slot = reinterpret_cast<uintptr_t*>(slot_);
            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                *slot = original_;
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(uintptr_t), oldProtect, &ignored);
            }
            installed_ = false;
        }

    private:
        uintptr_t slot_ = 0;
        uintptr_t original_ = 0;
        bool installed_ = false;
    };

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
        RepairMissingConnectionPointTable(occupant);
        ForgetOccupantWidths(occupant);

        if (!gSettings.enabled) {
            gOverrides.erase(occupant);
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

    void __fastcall DeterminePolePositionsHook(void* tool, void* /*edx*/) {
        auto* const field = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(tool) + kTool_MaxCellsBetweenPoles);
        const uint32_t saved = *field;
        if (gSettings.enabled) {
            uint32_t resolved = ResolveMaxCellsBetweenPoles(saved);
            // TEMPORARY PoC-ONLY: during a FAR drag every synthetic step is one FAR period, and
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
        *field = saved; // restore vanilla; the placement loop is this field's only reader
    }

    // ------------------------------------------------------------------
    // TEMPORARY PoC-ONLY: FAR drag hook (see the kDrawNetworkLine constants block above and
    // docs/sc4-powerline-tool-re.md SS17 for the design).
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
    // Supercover cell set for one FAR period. No longer used by the node-only drag synthesis (which
    // stores only pole nodes), but retained for reference / a future per-tile wire-occupancy option.
    [[maybe_unused]] std::vector<std::pair<uint32_t, uint32_t>> BuildFarPeriodPattern(const uint32_t run, const uint32_t rise) {
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

    uint32_t SelectSnapCandidate(const uint32_t startX, const uint32_t startZ,
                                 const uint32_t absoluteDx, const uint32_t absoluteDz) {
        const uint32_t major = std::max(absoluteDx, absoluteDz);
        const uint32_t minor = std::min(absoluteDx, absoluteDz);
        const float angle = std::atan2(static_cast<float>(minor), static_cast<float>(major));

        uint32_t bestCandidate = kAxisSnapCandidate;
        float bestDifference = std::fabs(angle - SnapCandidateAngle(bestCandidate));
        for (uint32_t candidate = 1; candidate <= kDiagonalSnapCandidate; ++candidate) {
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
            const float previousDifference = std::fabs(angle - SnapCandidateAngle(gFarSnapCandidate));
            if (previousDifference <= bestDifference + kSnapHysteresisRadians) {
                bestCandidate = gFarSnapCandidate;
            }
        }

        gFarSnapAnchorValid = true;
        gFarSnapAnchorX = startX;
        gFarSnapAnchorZ = startZ;
        gFarSnapCandidate = bestCandidate;
        return bestCandidate;
    }

    uint8_t __fastcall FarDrawNetworkLineHook(void* tool, void* /*edx*/, uint32_t* start, uint32_t* end,
                                              uint8_t straightOnly, int networkType) {
        const auto original = reinterpret_cast<DrawNetworkLineFn>(kDrawNetworkLine);
        gFarDragActive = false;
        if (!gSettings.enabled || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            gFarSnapAnchorValid = false;
            return original(tool, start, end, straightOnly, networkType);
        }

        const int32_t dx = static_cast<int32_t>(end[0]) - static_cast<int32_t>(start[0]);
        const int32_t dz = static_cast<int32_t>(end[1]) - static_cast<int32_t>(start[1]);
        const uint32_t adx = static_cast<uint32_t>(std::abs(dx));
        const uint32_t adz = static_cast<uint32_t>(std::abs(dz));
        if (adx == 0 && adz == 0) {
            gFarSnapAnchorValid = false;
            return original(tool, start, end, straightOnly, networkType);
        }

        const uint32_t snapCandidate = SelectSnapCandidate(start[0], start[1], adx, adz);
        if (snapCandidate == kAxisSnapCandidate || snapCandidate == kDiagonalSnapCandidate) {
            return original(tool, start, end, straightOnly, networkType); // vanilla handles regular headings
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
            return original(tool, start, end, straightOnly, networkType);
        }

        const CellXZ terminal = boundaryTerminal ? requestedEnd : nodeCell(periods);
        const CellXZ lastFarNode = nodeCell(periods);
        const bool hasTransitionTail = terminal.x != lastFarNode.x || terminal.z != lastFarNode.z;

        // Node-only layout: one single-cell step per FAR period node (plus the boundary tail node
        // and terminal). DeterminePolePositions is scoped to max-cells-between-poles = 1, so a pole
        // is forced at every step's primary (= only) cell. Intermediate span cells are deliberately
        // omitted: the wire renders bezier pole-to-pole and power conducts through the connection
        // graph (proven by vanilla's 10-cell empty pole gaps), so they were never needed for pole
        // placement or rendering. Omitting them also keeps the hidden per-cell line occupants off the
        // tiles between poles -- zoning/lot construction then only interrupts the wire when a lot
        // lands on a pole node itself -- and caps the buffer at ~periods+2 cells so arbitrarily long
        // FAR runs fit on any city tile (256x256 included) instead of overflowing a straight drag.
        std::vector<CellXZ> cells;
        std::vector<DraggedStep> steps;
        cells.reserve(periods + 3);
        steps.reserve(periods + 3);
        const auto pushNode = [&](const CellXZ node) {
            steps.push_back(DraggedStep{static_cast<uint32_t>(cells.size()),
                                        static_cast<uint32_t>(cells.size()), 0});
            cells.push_back(node);
        };
        for (uint32_t p = 0; p < periods; ++p) {
            pushNode(nodeCell(p));
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
                return original(tool, start, end, straightOnly, networkType);
            }
            pushNode(lastFarNode);
        }
        pushNode(terminal);

        // Fake straight drag sized so vanilla allocates at least as many cells (and therefore steps:
        // a straight drag makes one step per cell) as we need. Every allocated cell is overwritten
        // below, so the fake anchor is irrelevant to the result -- anchor it at the map edge instead
        // of the real `start` so a run of `totalCells` fits whenever the route length is within a map
        // dimension. A center anchor has too little clearance on either side and forced a fallback.
        const auto totalCells = static_cast<uint32_t>(cells.size());
        uint32_t fakeStart[2];
        uint32_t fakeEnd[2];
        if (totalCells <= cityCellsX) {
            fakeStart[0] = 0;         fakeStart[1] = start[1];
            fakeEnd[0] = totalCells - 1; fakeEnd[1] = start[1];
        } else if (totalCells <= cityCellsZ) {
            fakeStart[0] = start[0];  fakeStart[1] = 0;
            fakeEnd[0] = start[0];    fakeEnd[1] = totalCells - 1;
        } else {
            LOG_WARN("PowerPoleCustomization: [FAR PoC] route needs {} cells, exceeds city bounds "
                     "({}x{}); falling back to vanilla drag.", totalCells, cityCellsX, cityCellsZ);
            return original(tool, start, end, straightOnly, networkType);
        }
        if (original(tool, fakeStart, fakeEnd, straightOnly, networkType) == 0) {
            LOG_WARN("PowerPoleCustomization: [FAR PoC] vanilla allocation drag failed; falling back.");
            return original(tool, start, end, straightOnly, networkType);
        }

        auto* const base = reinterpret_cast<uint8_t*>(tool);
        auto* const stepsBegin = *reinterpret_cast<uint8_t**>(base + kTool_StepsBegin);
        auto* const cellsBegin = *reinterpret_cast<uint8_t**>(base + kTool_CellsBegin);
        const auto stepCapacity = static_cast<uint32_t>(
            (*reinterpret_cast<uint8_t**>(base + kTool_StepsEnd) - stepsBegin) / sizeof(DraggedStep));
        const auto cellCapacity = static_cast<uint32_t>(
            (*reinterpret_cast<uint8_t**>(base + kTool_CellsEnd) - cellsBegin) / sizeof(CellXZ));
        if (stepsBegin == nullptr || cellsBegin == nullptr ||
            stepCapacity < steps.size() || cellCapacity < totalCells) {
            LOG_WARN("PowerPoleCustomization: [FAR PoC] vanilla drag produced {} steps/{} cells, "
                     "need {}/{}; falling back.", stepCapacity, cellCapacity, steps.size(), totalCells);
            return original(tool, start, end, straightOnly, networkType);
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

    // ------------------------------------------------------------------
    // Tab / Shift-Tab pole-style switch + optional ImGui status overlay.
    // See docs/power-line-style-ui-design.md. The key hook has no ImGui dependency; the overlay only
    // ever runs when the ImGui service was acquired. Both the IC vtable (0x00aab008, proven by the
    // OnKeyDown slot's install-time byte check) and the power-tool vtable (0x00aa9f30, proven by the
    // FAR DrawNetworkLine patch's byte check) are validated against the live binary before either is
    // trusted at runtime, so a recompiled SC4 with moved vtables degrades to "no overlay / no Tab"
    // rather than misbehaving. The runtime checks below are pure reads + compares (no writes).
    // ------------------------------------------------------------------

    // Cached only to gate overlay visibility. Read on the render thread; set once at init.
    cISC4View3DWin* gView3D = nullptr;

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
            return;
        }
        const uint32_t count = static_cast<uint32_t>(gStyles.size()) + 1; // + vanilla(0)
        const uint32_t step = dir >= 0 ? 1u : count - 1u;
        gActiveStyleIndex = (gActiveStyleIndex + step) % count;
        const std::string activeName = gActiveStyleIndex == 0 ? "vanilla" : gStyles[gActiveStyleIndex - 1].name;
        LOG_INFO("PowerPoleCustomization: active pole style -> \"{}\" ({} of {}) -- applies to newly-placed poles.",
                 activeName, gActiveStyleIndex, gStyles.size());
    }

    using OnKeyDownFn = uint8_t(__thiscall*)(void* control, int vkCode, uint32_t modifiers);

    // Vtable-slot hook (installed like the FAR DrawNetworkLine hook: __fastcall matches __thiscall
    // with a throwaway edx). Consumes Tab only while the power tool is the active network subtool;
    // everything else -- including Tab under road/rail/street -- falls through to vanilla untouched.
    uint8_t __fastcall OnKeyDownHook(void* control, void* /*edx*/, int vkCode, uint32_t modifiers) {
        if (gSettings.enabled && vkCode == static_cast<int>(kVkTab) &&
            IsNetworkControlDrivingPowerTool(control)) {
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            CycleStyle(shift ? -1 : 1);
            return 1; // consume: don't let vanilla focus handling also act on Tab
        }
        const auto original = reinterpret_cast<OnKeyDownFn>(kOnKeyDown);
        return original(control, vkCode, modifiers);
    }

    // View3D (and the 3D-view window it lives under) does not exist at PostAppInit -- it appears
    // once a city view is up. Acquire it lazily on the render thread, retrying until it resolves,
    // instead of failing overlay setup permanently at app init.
    cISC4View3DWin* EnsureView3D() {
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
        cISC4View3DWin* const view = EnsureView3D();
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
        if (gActiveStyleIndex >= 1 && StyleDefinesRatio(gStyles[gActiveStyleIndex - 1], ratioIndex)) {
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

        const uint32_t interPole = (gActiveStyleIndex >= 1 && gStyles[gActiveStyleIndex - 1].hasMaxCells)
            ? gStyles[gActiveStyleIndex - 1].maxCellsBetweenPoles
            : 10u; // vanilla default (docs SS2)
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

    // Instance IDs remain hexadecimal values even though direction masks now use named REG.* keys.
    bool TryParseHexUInt32(const std::string& value, uint32_t& result) {
        std::string_view text = value;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text.remove_prefix(2);
        }
        result = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result, 16);
        return ec == std::errc() && ptr == text.data() + text.size();
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
            if (!TryParseHexUInt32(section.get(key), instanceId)) {
                LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] {} has invalid hexadecimal instance "
                         "ID \"{}\"; ignoring it.", sectionLabel, key, section.get(key));
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
            style.name = styleName;
            uint32_t definedCount = 0;
            for (uint32_t mask = 0; mask < 16; ++mask) {
                const std::string key(kRegularDirectionKeys[mask]);
                if (section.has(key)) {
                    uint32_t instanceId = 0;
                    if (!TryParseHexUInt32(section.get(key), instanceId)) {
                        LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] {} has invalid hexadecimal "
                                 "instance ID \"{}\"; ignoring it.", style.name, key, section.get(key));
                        continue;
                    }
                    style.instanceByDirectionMask[mask] = instanceId;
                    style.hasMask[mask] = true;
                    ++definedCount;
                }
            }

            const uint32_t farCount = ParseFarHeadingKeys(section, style, style.name);

            for (const auto& entry : section) {
                const std::string& key = entry.first;
                if (key.starts_with("0x")) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] legacy mask key {} is unsupported; "
                             "use a named REG.* key.", style.name, key);
                } else if (key.starts_with("reg.") &&
                           std::ranges::find(kRegularDirectionKeys, key) == kRegularDirectionKeys.end()) {
                    LOG_WARN("PowerPoleCustomization: [PowerPoles.{}] unknown or non-canonical regular "
                             "direction key {}; ignoring it.", style.name, key);
                }
            }
            WarnUnknownFarKeys(section, style.name);
            if (section.has("InterPoleDistance")) {
                const uint32_t d = std::clamp(ParseUInt32(section.get("InterPoleDistance"), 0u), 1u, kMaxInterPoleDistance);
                style.maxCellsBetweenPoles = d;
                style.hasMaxCells = true;
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

        LoadPoleStyles(ini);
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
                                           &InitConnectionPointsHook);

        for (size_t i = 0; i < kAddConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(addConnectionPatches_[i], "AddConnection@PlacePoles",
                                               kAddConnectionCallSites[i], kAddConnection, &AddConnectionHook);
        }
        for (size_t i = 0; i < kUpdateConnectionCallSites.size(); ++i) {
            installed += InstallCallSitePatch(updateConnectionPatches_[i], "UpdateConnection@SetDefaultExemplar",
                                               kUpdateConnectionCallSites[i], kUpdateConnection, &UpdateConnectionHook);
        }
        for (size_t i = 0; i < kDeterminePolePositionsCallSites.size(); ++i) {
            installed += InstallCallSitePatch(determinePolePositionsPatches_[i], "DeterminePolePositions",
                                               kDeterminePolePositionsCallSites[i], kDeterminePolePositions,
                                               &DeterminePolePositionsHook);
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

        installed += poleStyleLookupPatch1_.Install(kPoleStyleLookupSite1,
            {0x8b, 0x04, 0xbd, 0xb0, 0x67, 0xb4, 0x00}, &PoleStyleLookupHook, "PoleStyleLookup@1") ? 1 : 0;
        installed += poleStyleLookupPatch2_.Install(kPoleStyleLookupSite2,
            {0x8b, 0x04, 0xbd, 0xb0, 0x67, 0xb4, 0x00}, &PoleStyleLookupHook, "PoleStyleLookup@2") ? 1 : 0;

        // TEMPORARY PoC-ONLY: FAR drag (docs SS17). A vtable-slot swap, not a code patch.
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

        LOG_INFO("PowerPoleCustomization: installed {} of {} patches.", installed,
                 initConnectionPointsPatches_.size() + kAddConnectionCallSites.size() +
                 kUpdateConnectionCallSites.size() + kDeterminePolePositionsCallSites.size() +
                 2 + createFloorPatches_.size() + 4 + 2 + 1 + 1 + 1);

        const cISC4AppPtr app;
        if (app) {
            if (auto* const cheats = app->GetCheatCodeManager()) {
                if (cheats->RegisterCheatCode(kCheatKeepWires, cRZBaseString("keepwires"))) {
                    cheats->AddNotification2(this, 0);
                    cheatManager_ = cheats;
                    LOG_INFO("PowerPoleCustomization: cheat code registered (keepwires) -- toggles "
                             "suppression of power-line rerouting on zone changes.");
                } else {
                    LOG_WARN("PowerPoleCustomization: failed to register cheat code keepwires.");
                }
            }
        }

        SetupStatusOverlay();
        return true;
    }

    bool PostAppShutdown() override {
        if (cheatManager_) {
            cheatManager_->UnregisterCheatCode(kCheatKeepWires);
            cheatManager_->RemoveNotification2(this, 0);
            cheatManager_.Reset();
        }
        TeardownStatusOverlay();
        onKeyDownPatch_.Uninstall();
        farDrawNetworkLinePatch_.Uninstall();
        keepWiresPatch_.Uninstall();
        modelInstanceIdPatch_.Uninstall();
        for (auto& patch : initConnectionPointsPatches_) patch.Uninstall();
        for (auto& patch : addConnectionPatches_) patch.Uninstall();
        for (auto& patch : updateConnectionPatches_) patch.Uninstall();
        for (auto& patch : determinePolePositionsPatches_) patch.Uninstall();
        wireWidthLookupPatch_.Uninstall();
        for (auto& patch : createFloorPatches_) patch.Uninstall();
        floorTextureHashPatch1_.Uninstall();
        floorTextureHashPatch2_.Uninstall();
        wallTextureHashPatch1_.Uninstall();
        wallTextureHashPatch2_.Uninstall();
        poleStyleLookupPatch1_.Uninstall();
        poleStyleLookupPatch2_.Uninstall();
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
                static_cast<uint32_t>(stdMsg->GetData1()) == kCheatKeepWires) {
                gKeepWiresOnZoneChange = !gKeepWiresOnZoneChange;
                LOG_INFO("PowerPoleCustomization: keepwires {} -- zone changes {} "
                         "the power-line remove/reposition/rebuild handler.",
                         gKeepWiresOnZoneChange ? "ENABLED" : "disabled",
                         gKeepWiresOnZoneChange ? "will bypass" : "will run");
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

        // View3D is acquired lazily at render time (EnsureView3D) -- it does not exist yet here.
        // Registration only needs the ImGui service, which is available at PostAppInit.
        if (mpFrameWork == nullptr ||
            !mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            imguiService_ = nullptr;
            LOG_INFO("PowerPoleCustomization: ImGui service absent; status overlay disabled "
                     "(Tab style switching still works).");
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
                 imguiService_->GetApiVersion());
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
        if (gView3D != nullptr) {
            gView3D->Release();
            gView3D = nullptr;
        }
    }

    std::array<TerrainDecal::RelativeCallPatch, 3> initConnectionPointsPatches_{};
    TerrainDecal::RelativeCallPatch modelInstanceIdPatch_{};
    std::array<TerrainDecal::RelativeCallPatch, 4> addConnectionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 2> updateConnectionPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 3> determinePolePositionsPatches_{};
    std::array<TerrainDecal::RelativeCallPatch, 3> createFloorPatches_{};
    Imm32CallPatch floorTextureHashPatch1_{};
    Imm32CallPatch floorTextureHashPatch2_{};
    Imm32CallPatch wallTextureHashPatch1_{};
    Imm32CallPatch wallTextureHashPatch2_{};
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
