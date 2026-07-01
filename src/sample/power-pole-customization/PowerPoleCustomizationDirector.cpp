// Custom power-pole cable attach points: lets a pole prop exemplar declare, per connection
// direction, how many cables it carries and exactly where each one attaches on the model,
// instead of being limited to the engine's two baked 4-point tables (_cardinalConnectionPoints /
// _diagonalConnectionPoints). See docs/sc4-powerline-tool-re.md for the full reverse-engineering
// trail behind every address and struct offset below.
//
// Data-driven surface: a pole prop exemplar opts in by defining 1-4 of the
// kPropAttachPointsDir0..3 properties (see PROPERTY FORMAT below). Exemplars that define none of
// them are untouched -- InitConnectionPointsHook falls through to the original cardinal/diagonal
// table exactly as vanilla does.
//
// PROPERTY FORMAT (Float32Array), one property per connection direction:
//   [pointCount, x0,y0,z0, x1,y1,z1, ..., x(pointCount-1),y(pointCount-1),z(pointCount-1)]
// pointCount is clamped to [1, kMaxPointsPerDirection] (ini-configurable).
//
// Status: hook sites and data structures below are real, address-confirmed (see "Hook sites"
// section and docs/sc4-powerline-tool-re.md SS7-14). The cable-count/strand-build algorithm is
// written out in full and ACTIVE.
//
// First attempt (2026-07-01) registered extra strands as cSC4PowerLineOccupant city objects --
// confirmed in-game (docs SS13) to run with no crash but NOT make DrawPowerlines render anything,
// since rendering reads polyline data cached directly in each pole's own tConnection entry,
// independent of any cSC4PowerLineOccupant's existence. Current approach (docs SS14) appends
// extra strands directly into that tConnection data instead: GetControlPoints (bezier control
// points + tessellation count) -> TessellateBezierSegment (writes the curve's points) ->
// polylines.insert(end(), 1, newPolyline) (the same vector-append vanilla's own AddConnection
// uses, confirmed via its element-copy helper), wired into TopUpExtraStrands(), which the hooks
// below call. Not yet verified in-game -- see "Known gaps" at the bottom of this file.
//
// TEMPORARY PoC-ONLY ADDITION: a "polelinetest" cheat code (search kCheatPoleLineTest) that
// bypasses the still-unverified exemplar-property-reading path entirely, so the strand-count/
// append path can be tested in isolation with zero new art/exemplars -- place or reload a power
// pole after toggling the cheat.

#include "cIGZCOM.h"
#include "cIGZCheatCodeManager.h"
#include "cIGZMessage2.h"
#include "cIGZMessage2Standard.h"
#include "cIGZPersistResourceManager.h"
#include "cISC4App.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
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
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    constexpr uint32_t kDirectorID = 0xB07E11E5; // arbitrary, must not collide with another director in this Plugins folder
    constexpr uint16_t kSupportedGameVersion = 641;
    constexpr uint32_t kCheatCodeMessageType = 0x230E27AC; // fixed SC4 message type, same constant every sample uses

    // ------------------------------------------------------------------
    // TEMPORARY PoC-ONLY cheat code: "polelinetest". See docs/sc4-powerline-tool-re.md's PoC plan
    // (step 4). The exemplar-property-reading path is still an unverified stub (see
    // InitConnectionPointsHook), so no real prop can trigger a strand-count override yet. This
    // cheat sidesteps that entirely: while active, every pole that goes through
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
    // Custom exemplar property IDs.
    // PLACEHOLDER RANGE -- arbitrary, not backed by any convention. Checked: this repo's "DJEM"
    // git history (commits mentioning "DJEM for props"/"DJEM for props ini") does NOT define a
    // property-ID convention -- DJEM turned out to be an unrelated terrain-diagonal-fix feature
    // (aligning prop placement height with corrected terrain triangulation), nothing to do with
    // exemplar property IDs. There is no existing precedent in this repo to reconcile against;
    // these four IDs need a real decision (e.g. checking the SC4 modding community's established
    // reserved ranges) before shipping, not just a cross-reference to something that doesn't exist.
    // ------------------------------------------------------------------
    constexpr uint32_t kPropAttachPointsDir0 = 0xB22A0000;
    constexpr uint32_t kPropAttachPointsDir1 = 0xB22A0001;
    constexpr uint32_t kPropAttachPointsDir2 = 0xB22A0002;
    constexpr uint32_t kPropAttachPointsDir3 = 0xB22A0003;
    constexpr std::array<uint32_t, 4> kAttachPointProperties = {
        kPropAttachPointsDir0, kPropAttachPointsDir1, kPropAttachPointsDir2, kPropAttachPointsDir3,
    };

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

        [[nodiscard]] uint32_t PointCount(const uint32_t direction) const {
            return direction < 4 ? static_cast<uint32_t>(perDirection[direction].size()) : 0;
        }

        [[nodiscard]] const AttachPoint* Point(const uint32_t direction, const uint32_t index) const {
            if (direction >= 4 || index >= perDirection[direction].size()) {
                return nullptr;
            }
            return &perDirection[direction][index];
        }
    };

    // this(cSC4PowerPoleOccupant*) -> override, only present for poles whose exemplar defined at
    // least one kPropAttachPointsDirN property. Poles without an override use vanilla behavior
    // unmodified, so this mod is opt-in per prop, not a global replacement.
    std::unordered_map<void*, PoleAttachOverride> gOverrides;

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
        if (valueCount < 4 || (valueCount - 1) % 3 != 0) {
            LOG_WARN("PowerPoleCustomization: property 0x{:08X} has {} values, expected 1 + 3*N; ignoring.", propertyId, valueCount);
            return false;
        }

        const float* const values = variant->RefFloat32();
        const uint32_t declaredCount = static_cast<uint32_t>(values[0]);
        const uint32_t availableCount = (valueCount - 1) / 3;
        const uint32_t pointCount = std::clamp(std::min(declaredCount, availableCount), 1u, gSettings.maxPointsPerDirection);

        outPoints.reserve(pointCount);
        for (uint32_t i = 0; i < pointCount; ++i) {
            const float* const xyz = values + 1 + i * 3;
            outPoints.push_back(AttachPoint{xyz[0], xyz[1], xyz[2]});
        }
        return true;
    }

    // Builds (or refreshes) this pole's override from its current default exemplar. Returns false
    // (and leaves gOverrides untouched for this pole) when the exemplar defines none of the four
    // properties -- that pole stays on vanilla behavior.
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

    // Builds one tessellated Bezier polyline between two attach points and appends it into the
    // already-resolved active tConnection -- the actual structure DrawPowerlines reads (confirmed,
    // docs SS13/SS14), not a cSC4PowerLineOccupant.
    // Uses only functions with clean, self-contained decompiles (no register-spill ambiguity):
    // GetControlPoints -> TessellateBezierSegment -> polylines.insert(end(), 1, newPolyline). The
    // polyline is deep-copied by the insert call, so a stack-local {begin,end,cap} view over our
    // own heap buffer is a safe value to pass -- no need to match the game's exact allocator.
    void AppendPolylineToConnection(void* entry, const AttachPoint& a, const AttachPoint& b) {
        float control1[3];
        float control2[3];
        int32_t pointCount = 0;
        const auto getControlPoints = reinterpret_cast<GetControlPointsFn>(kGetControlPoints);
        getControlPoints(a.x, a.y, a.z, b.x, b.y, b.z, control1, control2, &pointCount);
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

        // TEMPORARY PoC-ONLY diagnostic: confirm the append actually happens in memory before
        // chasing further render-side theories. Logs polyline-list element count before/after.
        auto* const beginPtr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin);
        auto* const endPtrBefore = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesEnd);
        const ptrdiff_t countBefore = (endPtrBefore - beginPtr) / 12;

        insert(polylines, insertPos, view, 0, 1, 1);

        auto* const beginPtrAfter = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesBegin);
        auto* const endPtrAfter = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(entry) + kConnection_PolylinesEnd);
        const ptrdiff_t countAfter = (endPtrAfter - beginPtrAfter) / 12;
        LOG_INFO("PowerPoleCustomization: [TEST MODE] polyline count {} -> {} on entry {} (begin {} -> {}).",
                 countBefore, countAfter, entry, static_cast<void*>(beginPtr), static_cast<void*>(beginPtrAfter));

        // TEMPORARY PoC-ONLY diagnostic: verify CONTENT, not just count. Reads back the just-
        // appended polyline (last element, 12 bytes: begin/end/cap) via the exact pointers
        // DrawPowerlines itself would use, and logs its point count plus first/last coordinates.
        if (countAfter > 0) {
            const uint8_t* const lastPolyline = beginPtrAfter + (countAfter - 1) * 12;
            const float* const polyBegin = *reinterpret_cast<const float* const*>(lastPolyline);
            const float* const polyEnd = *reinterpret_cast<const float* const*>(lastPolyline + 4);
            const ptrdiff_t pointCount = polyBegin ? (polyEnd - polyBegin) / 3 : -1;
            if (polyBegin != nullptr && pointCount > 0) {
                LOG_INFO("PowerPoleCustomization: [TEST MODE] last polyline has {} points; first ({}, {}, {}), last ({}, {}, {}).",
                         pointCount, polyBegin[0], polyBegin[1], polyBegin[2],
                         polyBegin[(pointCount - 1) * 3], polyBegin[(pointCount - 1) * 3 + 1], polyBegin[(pointCount - 1) * 3 + 2]);
            } else {
                LOG_WARN("PowerPoleCustomization: [TEST MODE] last polyline looks empty/invalid (begin {}, pointCount {}).",
                         static_cast<const void*>(polyBegin), pointCount);
            }
        }
    }

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

    // Vanilla AddConnection/UpdateConnection already built kVanillaPointsPerDirection (4) strands
    // correctly by calling through to the real game code above. This tops up any *extra* strands
    // an override calls for beyond that by appending into `occupant`'s OWN tConnection entry only
    // (see AppendPolylineToConnection) -- the mechanism DrawPowerlines actually reads.
    //
    // PlacePoles calls AddConnection twice with arguments swapped, but only one reciprocal entry has
    // entry+0x09 set and owns the rendered polyline list. FindConnectionEntry filters for that side,
    // so the other hook firing is an intentional no-op.
    void TopUpExtraStrands(void* occupant, void* otherPole) {
        const uint32_t direction = DirectionBetween(occupant, otherPole);
        const uint32_t count = std::min(StrandCount(occupant, direction), StrandCount(otherPole, direction));
        if (count <= kVanillaPointsPerDirection) {
            return; // vanilla's 4 already covers it
        }

        void* const entry = FindConnectionEntry(occupant, otherPole);
        if (entry == nullptr) {
            return; // reciprocal non-rendering side, or no live connection for this pair
        }

        LOG_INFO("PowerPoleCustomization: appending {} extra strands in direction {} (vanilla "
                 "already built {}).", count - kVanillaPointsPerDirection, direction, kVanillaPointsPerDirection);

        for (uint32_t i = kVanillaPointsPerDirection; i < count; ++i) {
            const AttachPoint a = GetAttachPoint(occupant, direction, i);
            const AttachPoint b = GetAttachPoint(otherPole, direction, i);
            AppendPolylineToConnection(entry, a, b);
        }
    }

    // ------------------------------------------------------------------
    // Hook bodies.
    // ------------------------------------------------------------------
    using InitConnectionPointsFn = void(__thiscall*)(void* occupant);
    using AddConnectionFn = void(__thiscall*)(void* occupant, void* otherPole, void* lineInfoVector);
    using UpdateConnectionFn = void(__thiscall*)(void* occupant, void* otherPole);

    void __fastcall InitConnectionPointsHook(void* occupant, void* /*edx*/) {
        const auto original = reinterpret_cast<InitConnectionPointsFn>(kInitConnectionPoints);
        original(occupant); // always run vanilla first: it still owns model load + default-table selection

        if (!gSettings.enabled) {
            gOverrides.erase(occupant);
            return;
        }

        // TEMPORARY PoC-ONLY: see the block comment above kCheatPoleLineTest. Bypasses the
        // exemplar-reading path entirely so the strand-count/registration path can be tested in
        // isolation. Delete this branch once the exemplar-cast question is resolved either way.
        if (gSyntheticTestModeEnabled) {
            gOverrides[occupant] = BuildSyntheticTestOverride(occupant);
            LOG_INFO("PowerPoleCustomization: [TEST MODE] injected {}-point synthetic override for pole {}.",
                     kSyntheticTestPointCount, occupant);
            return;
        }

        // Retrieval mechanism now confirmed (clean decompile of InitConnectionPoints itself, once
        // its prototype/this-type were fixed): call occupant->pVtbl6 (this+0x24)'s own vtable slot
        // +0x10, using &occupant->pVtbl6 as the this-argument (standard multiple-inheritance
        // adjustor pattern). The returned pointer's OWN vtable slot +0x2c behaves like a property
        // getter (matches vanilla's next call in the same function). What's NOT confirmed: whether
        // that returned pointer is ABI-compatible with the public cISCPropertyHolder/cISCResExemplar
        // GZCOM interfaces used by TryBuildOverride() below -- this looks like SC4's internal,
        // unexposed property-access path, and casting it to the public interface without confirming
        // vtable-slot-for-slot compatibility risks a genuine crash, not just a wrong read. Left
        // unresolved rather than risk that cast; the call sequence to reach the pointer is real.
        cISCResExemplar* const exemplar = nullptr; // TODO: confirmed retrieval path above, cast unverified

        PoleAttachOverride candidate;
        if (TryBuildOverride(exemplar, candidate)) {
            gOverrides[occupant] = std::move(candidate);
        } else {
            gOverrides.erase(occupant);
        }
    }

    void __fastcall AddConnectionHook(void* occupant, void* /*edx*/, void* otherPole, void* lineInfoVector) {
        // Direction is whatever cSC4NetworkRoutines::Get0To3Direction(cellA, cellB) returns in the
        // original -- recomputing it here would duplicate logic already proven correct in the
        // engine. Pragmatic approach: let vanilla AddConnection run first (it still does cell-coord
        // math, intersection bookkeeping, and the this[0x50]-gated add-vs-remove branch correctly),
        // then top up with any *extra* strands our override calls for beyond vanilla's fixed 4.
        const auto original = reinterpret_cast<AddConnectionFn>(kAddConnection);
        original(occupant, otherPole, lineInfoVector);

        if (!gSettings.enabled) {
            return;
        }

        TopUpExtraStrands(occupant, otherPole);
    }

    void __fastcall UpdateConnectionHook(void* occupant, void* /*edx*/, void* otherPole) {
        const auto original = reinterpret_cast<UpdateConnectionFn>(kUpdateConnection);
        original(occupant, otherPole);
        TopUpExtraStrands(occupant, otherPole);
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

        LOG_INFO("PowerPoleCustomization: installed {} of {} call-site patches.", installed,
                 initConnectionPointsPatches_.size() + kAddConnectionCallSites.size() +
                 kUpdateConnectionCallSites.size());

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
// Known gaps after the PoC render-path correction:
//   1. CLOSED: all three InitConnectionPoints call sites are hooked, including 0x0064ceaa inside
//      cSC4PowerPoleOccupant::Init, the new-pole creation path.
//   2. Production exemplar-pointer retrieval in InitConnectionPointsHook is still a stub,
//      but the real call sequence to reach it is now confirmed (occupant->pVtbl6 vtable slot +0x10,
//      see the comment in InitConnectionPointsHook). The polelinetest cheat deliberately bypasses
//      this production-only gap.
//   3. CLOSED: Get0To3Direction confirmed at 0x0061f4c0. TopUpExtraStrands() computes real
//      direction and strand counts.
//   4. CLOSED (superseded): cSC4PowerLineOccupant's vtable was fully mapped and used to register
//      extra strands as city objects (see docs SS9/SS10). Confirmed in-game (SS13) that this alone
//      doesn't render anything -- removed from the render path. The vtable mapping itself remains
//      documented in docs/sc4-powerline-tool-re.md if city-object/save-load registration is wanted
//      again later; the code for it was deleted from this file (kept unused code out).
//   5. CLOSED (implementation corrected, awaiting another in-game run): DrawPowerlines renders from polyline
//      data cached directly inside each pole's OWN tConnection entry (this+0xac/+0xb0 on
//      cSC4PowerPoleOccupant, +0x14/+0x18 for the polylines vector within each entry -- see docs
//      SS14). TopUpExtraStrands()/AppendPolylineToConnection() now build and append real polylines
//      there via GetControlPoints -> TessellateBezierSegment -> polylines.insert(end(), 1, ...),
//      using the active entry selected by otherPole at +0x00 and render-owner byte +0x09. Complete-
//      object position is +0xa0/+0xa4/+0xa8; the prior +0x98 base missed an adjusted-this +8.
// None of these are guesses standing in as facts -- each is a named function/TODO citing exactly
// what's missing, per docs/sc4-powerline-tool-re.md's "next steps".
// ------------------------------------------------------------------
