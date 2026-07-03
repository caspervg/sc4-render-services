# SC4 PowerLine Tool — Reverse Engineering Notes

Investigation date: 2026-06-30 to 2026-07-01. All addresses are **Mac** (`MacOS_x86-32-cpu0x3`,
symbolized, trusted per project convention) unless noted. Windows (`SimCity 4.exe` 1.1.641) addresses
confirmed in §7. The 19 confirmed Windows functions + 4 data globals have been **renamed directly in the
Ghidra database** (see §8), and a `cSC4PowerPoleOccupant` struct now exists in both programs (§9),
applied as the `this` type on 12 of the renamed methods.

## Class architecture

- `cSC4PowerLineTool` (0x0022c578) — drag tool, subclass of `cSC4NetworkTool`. Builds a pole/connection
  graph while dragging, commits it on mouse-up.
- `cSC4PowerPoleOccupant` — a live pole instance. Owns model, draw, connection list.
- `cSC4PowerLineOccupant` — represents a wire span (`tLineInfo`) between two poles.

Single global tool instance — there is no concept of "pole type" or per-network-tier config; everything
funnels through one static array + one fixed config exemplar (see below). This is the root constraint
behind most of the "can't do X without a binary patch" findings.

## 1. Data-driven pole shape selection (`[PowerPoles]` ini)

`cSC4PowerLineTool::ReadPowerPoleInfos` (0x0022b7f4) fetches a resource via `SC4GetResourceAsFile`
(TGI type=0, group=`0x8A5971C5`, instance=`0x8A5993B9` — confirmed in `SimCity_1.dat`, labeled
"INI (Networks)", QFS-compressed). Parses as ini (`cRZIniFile::EnumEntries`, section `[PowerPoles]`),
callback `AddPowerPoleInfo` (0x0022bd98) fills static array `cSC4PowerLineTool::sPowerPoleForDirectionsFlag[16]`
(confirmed 64-byte / 16×uint32 data region at 0x00ab9840).

Actual vanilla table (extracted from `SimCity_1.dat` via iLive Reader, QFS-compressed payload):

```
0x00=0x05000205   0x04=0x05000205   0x08=0x05000405   0x0c=0x05000305
0x01=0x05000205   0x05=0x05000105   0x09=0x05000305   0x0d=0x05000305
0x02=0x05000405   0x06=0x05000305   0x0a=0x05000305   0x0e=0x05000305
0x03=0x05000305   0x07=0x05000305   0x0b=0x05000305   0x0f=0x05000305
```

Key = 4-bit connection-direction bitmask (bit0=±x axis, bit1=diag NW-SE, bit2=±z axis, bit3=diag NE-SW).
Value = pole prop Instance ID. Collapses to 4 real models: straight (`0205`), lone-diagonal (`0405`),
cross/double-straight (`0105`), everything-else junction/turn (`0305`, catchall).

Full TGI assembled in `cSC4NetworkTool::GetOccupantExemplar` (0x0012441c):
- Type = fixed `0x6534284A` (exemplar type)
- Group = `param_2`, defaults to `*(int*)(*(int*)(this+0x398)+0xc)` when 0 passed (tool's current/base
  network group)
- Instance = `param_1` = the table value looked up above

`CreatePowerPole` (0x0022c06a): existing-pole-at-cell → OR direction flags together, re-lookup (merge
case); new pole → direct lookup. Orientation flags stored back on the occupant at offset `0xb4`.

**Mod surface**: edit `[PowerPoles]` to retexture/remodel which of the 4 baked shapes appears per
direction-combo. No binary changes needed for this part.

## 2. Global tunables exemplar

`cSC4PowerPoleOccupant::StaticInit` / `InitPowerLineControlVariables`
(0x00256ac8 / 0x002554da) read one fixed exemplar — Type `0x6534284A`, Group `0xE7E2C2DB`,
Instance `0xC911E35B`. That's the **"Utilities"** exemplar in `SimCity_1.dat`. Confirmed properties:

| Property | ID | Value |
|---|---|---|
| Power line: Scaling factor (LOD width per zoom) | `0xE9AC43E7` | `[0, 1, 0.5, 0.35, 0.2]` |
| Power Pole Demolition Cost | `0xA2ECED8` | 5 |
| Power line:Horizontal Control (max offset) | `0xE9A18D3F` | 16 |
| Power line control horiz. % | `0x49A1E8C4` | 10 |
| Power line vertical distance (max sag) | `0x89A18FC4` | 10 |
| Power line:Vertical Control % | `0xE9A1E8D4` | 0.15 |
| Curve distance between points | `0x49A195C0` | 8 |
| Min number of curve points | `0x89A1E8FB` | 5 |
| Max cells between power poles | `0x098B25C8` | 10 |
| Minimum power line height | `0x098B27D0` | 7 |
| Power line placement cost | `0x0A5F596B` | 2 |
| Power Pole: Cost | `0x4A5F5953` | 15 |

Width-array index = current camera zoom level (`ChangeZoomLevel` writes
`*(this+0xc0) = drawContext->zoom`). Index 0 = 0.0 → lines don't render at that zoom tier. So the
5-tier "width" array is pure render LOD, not voltage/network-class — confirmed not configurable per
network type, only per global zoom.

## 3. Sag curve geometry — `GetControlPoints` (0x0025608c)

Cubic Bezier per span:
- Horizontal control offset = `min(span * horizPercent/100, horizMax)`
- Vertical sag = `min(span² * vertPercent/100, vertMax)` (quadratic in span)
- Point count = `max(minCurvePoints, span/curveSegmentLength)`

All knobs come from the Utilities exemplar above.

## 4. Placement logic (drag-time)

`DeterminePolePositions` (0x0022c98a) walks the dragged path cell-by-cell:
- `CellIsOKForPlacement` (0x0022c1cc): rejects zone-interior mismatches, occupant overlap (GZCLSID
  checks), lot-manager occupied tiles. Returns 0/1/2/3 → UI failure-reason strings
  (`0xf0000012`/`0xf0000013` for cases 2/3).
- Forces new pole at slope/direction breaks, on collision with existing lines
  (`CellIsOccupiedByPowerLine`/`FindPoleOccupant`), or once span exceeds "Max cells between power
  poles" (10).
- Snaps/merges nodes at existing poles or wire crossings (`IntersectionIsInList`/`PoleIsInList`).
- Builds graph: `tPoleInfo` nodes + `tConnectionInfo` edges via `AddConnection`.

Commit pass `PlacePoles` (0x0022d4c2): dedups identical-position nodes into intersections,
instantiates real `cSC4PowerPoleOccupant` per node (`CreatePowerPole`) and `cSC4PowerLineOccupant`
per edge, wires `AddConnection` on the live occupants, computes 3D position via `ComputePolePosition`
(terrain-height query through vtable, slope-aware). **Pole position is grid-quantized** —
`ComputePolePosition` is `cellX * cellSize`, integer cell coords in, no fractional offset.

## 5. Rendering — `cSC4PowerPoleOccupant::DrawPowerlines` (0x00257810)

Per wire segment between consecutive bezier sample points: builds a camera-facing quad ribbon
(half-width = `sPowerLineWidthFactors[zoom]`), UV-tiled by segment length, textured
`kPowerLineTexture`, drawn via `SC4DrawContext::DrawPrims`. Separate pass for "buoy" markers
(`kPowerLineBuoyTexture`, fixed unit-size quads) on dangling/unconnected wire ends.

## 6. Cable attach points — bigger finding than initially assumed

`GetLineConnectionPoints(this, direction, out[4])` (0x00254e4a): reads
`(direction & 3) * 0x30` as base offset into table at `this+0xe0`, walks **4 consecutive
`cS3DVector3` entries** (`0xc`-byte stride) from there. So **each of the 4 directions already
stores up to 4 attach points** — table is `4 directions × 4 points × xyz`, min 192 bytes. No
rotation matrix applied — flat translation add of baked per-direction-slot offset:
```
point = table[direction & 3][i] + (this.x, this.y, this.z)
```

`this+0xe0` is set by `InitConnectionPoints` (0x002556ac) to one of exactly two **hardcoded**
global tables: `_cardinalConnectionPoints` (0x00c20fc0) or `_diagonalConnectionPoints`
(0x00c20f00), picked via a 2-bit field read off the exemplar's orientation property (bits 8-11 of
a property read at `piVar1+0x2c`).

`AddConnection` (0x00258eee) / `UpdateConnection` (0x00258346) both run
`while (local_110 != 4)` building **4 separate Bezier polylines** per connection — one per
point-pair — pushed into a `vector<vector<cS3DVector3>>` (a list of strands, not one polyline).
`DrawPowerlines` already iterates that outer vector generically; its loop doesn't hardcode a
strand count, it draws however many polylines got pushed.

**Confirmed visually**: the lattice transmission tower (user screenshot, reddit r/simcity4) shows
4 real cables per side — all 4 slots are actively used and meaningfully distinct on that prop, not
degenerate placeholders. So the engine isn't "barely using" this capacity — it's hardcoded to
**exactly 4, always**, no more, no less.

### What's actually hardcoded (3 sites, always travel together)

1. `GetLineConnectionPoints` — always reads exactly 4 points per direction.
2. `AddConnection` — always builds exactly 4 strands.
3. `UpdateConnection` — same, mirrored.

`this+0xe0`'s indirection-pointer design means swapping the table source costs nothing in struct
layout — it's already just a pointer write.

## DLL design notes (not yet implemented — design discussion only)

### A. Custom attach-point count + XYZ per pole prop

1. Patch `InitConnectionPoints` to build a per-exemplar heap table instead of picking between the 2
   static ones — read via `cSCPropertyHelp::GetPropertyValue`, same pattern as the sag-curve
   properties. Natural format: one new property per direction (4 total, dir0..dir3), each a flat
   float array `[count, x0,y0,z0, x1,y1,z1, ...]`. No new resource fetch needed — `InitConnectionPoints`
   already has the exemplar pointer in hand (the one `LoadModel` pulled via `GetOccupantExemplar`).
2. Store a per-pole strand count (1–4, or higher if the loop-bound constant is also raised)
   alongside the table pointer.
3. Patch the three `!= 4` loop bounds (`GetLineConnectionPoints`, `AddConnection`,
   `UpdateConnection`) to use that count instead of the literal.
4. `DrawPowerlines` needs **no changes** — already strand-count-agnostic.
5. Need 4 new property IDs. **Checked (2026-07-01): this repo's "DJEM" work is not a property-ID
   convention** — it's an unrelated terrain-diagonal-fix feature (aligning prop placement height
   with corrected terrain triangulation). No existing precedent in this repo to reuse; these IDs
   need a real decision (e.g. checking the SC4 modding community's established reserved ranges)
   before shipping. Also need a fallback path (count=1, snap to existing baked cardinal/diagonal
   offset) for exemplars that don't define the new properties, so vanilla/other-mod poles don't break.

### B. Multiple pole "types" / families

Recommended: reuse the existing `[PowerPoles]` ini mechanism rather than registering new GZCOM
tools.
- Extend ini to multiple named sections: `[PowerPoles]`, `[PowerPoles.Steel]`,
  `[PowerPoles.Wood]`, etc.
- Patch `ReadPowerPoleInfos`/`AddPowerPoleInfo` (currently one `EnumEntries` call on one section
  into a flat 16-entry array) to enumerate each section into `table[styleIndex][16]`.
- Add a "current style" selector; `CreatePowerPole`'s lookup becomes
  `table[styleIndex][directionMask]`.
- **Open question, not yet checked**: `cSC4NetworkTool` (base class of `cSC4PowerLineTool`) may
  already expose an unused "style index" member / cycle mechanism — vanilla uses exactly this
  pattern for Avenue lamp-post style cycling and street style toggling. If present, piggyback it
  instead of inventing new storage — next thing to check in Ghidra.
- Heavier alternative (not recommended unless distinct *placement logic* per type is needed, not
  just looks): register genuinely separate tools, own GZCOM CLSID, own toolbar icon, own ini
  resource — how Road vs Street vs Avenue are actually separate tools in vanilla. Much bigger
  surface (UI registration, icon assets, dispatch entries).

### C. Fractional angle (tangent, lower priority per user — deprioritized in favor of B/A)

- `SetTransform` (0x00254ee0) stores a free `cS3DTransform` matrix — not angle-snapped at the
  storage level.
- BUT `LoadModel` → `cSC4ModelMakerUtility::GetModelInstanceID` selects among a small number of
  pre-authored rotated S3D mesh variants via bit-packed instance ID — mesh itself locked to ~4
  discrete angles, not continuously rotatable without new art.
- `GetLineConnectionPoints` does a flat translation add, no rotation matrix — so free-rotating a
  pole via `SetTransform` would visually detach cables from their baked attach slots unless
  `GetLineConnectionPoints` also gets a rotation-matrix multiply added.
- Cheap version: rotate only the connection-point offset (pure code, no new art) while leaving the
  mesh on its nearest discrete variant — keeps cables visually attached, accepts a mesh-angle seam.
  True fractional mesh rendering needs new art, not just code.
- Pole *position* is also grid-quantized (`ComputePolePosition`, see §4) — separate problem from
  rotation, untouched by any of the above.

## Open next steps

1. ~~Cross-check all addresses above against Windows `SimCity 4.exe` 1.1.641~~ — done, see §7 below.
   Re-verify before writing a real patch; matches are strong/confirmed but not exhaustively
   byte-diffed.
2. Check whether `cSC4NetworkTool` already has an unused style-index field/cycle mechanism (for
   §B above).
3. Pin exact byte layout for the proposed new connection-point properties (property IDs from
   modder-reserved range, decide array-vs-fixed-struct encoding).
4. ~~Decide GetModelInstanceID's instance-ID bit layout~~ — resolved 2026-07-03; see §15D.
5. Find `GetLineConnectionPoints`'s Windows equivalent — likely inlined by MSVC at both call sites
   (`AddConnection`/`UpdateConnection`) rather than a standalone function; not yet located as a
   separate symbol. If a hook needs a single chokepoint, patch both inline sites instead.

## 7. Windows (`SimCity 4.exe` 1.1.641) address mapping — confirmed

Found via unique-constant cross-referencing (`search_instructions` on TGI components / property
IDs / GZCLSID values) and call-graph/structural matching, not name lookup — Windows symbol names
are OOAnalyzer guesses, mostly `FUN_xxxxxxxx`. A few function names did survive directly
(`Init`, `CheckForReservedTiles`) — noted below. Windows program: `SimCity 4.exe`, base `0x00400000`.

| Mac (`x86-32-cpu0x3`) | Windows (`SimCity 4.exe`) | Confidence / evidence |
|---|---|---|
| `cSC4PowerLineTool::Init` (0x0022c74e) | `Init` @ `0x00650630` | confirmed — name survived, sole caller of ReadPowerPoleInfos match |
| `cSC4PowerLineTool::ReadPowerPoleInfos` (0x0022b7f4) | `FUN_0064f880` | confirmed — literal `"PowerPoles"` string + EnumEntries callback |
| `cSC4PowerLineTool::AddPowerPoleInfo` (0x0022bd98) | `FUN_0064f140` (EnumEntries callback, no formal function header) | confirmed — writes into `[ESI*4 + 0xB467B0]`, matches table below |
| `cSC4PowerLineTool::sPowerPoleForDirectionsFlag` (Mac 0x00ab9840) | `0x00B467B0` (static array, 16×4 bytes) | confirmed |
| `cSC4PowerLineTool::CreatePowerPole` (0x0022c06a) | `FUN_00650140` | confirmed — structurally identical, indexes the table above at `&UNK_00b467a9+7+uVar3*4` = `0xB467B0` |
| `cSC4PowerLineTool::DeterminePolePositions` (0x0022c98a) | `FUN_00650840` | confirmed — 993 instr / 177 blocks, calls CellIsOKForPlacement+FindPoleOccupant+ComputePolePosition matches, and is the function that calls `FUN_00651560` (PlacePoles) exactly where Mac calls `PlacePoles(this,'\0')` |
| `cSC4PowerLineTool::PlacePoles` (0x0022d4c2) | `FUN_00651560` | confirmed — self-recursive call at the end (matches Mac's `PlacePoles(this,'\0')` recursion), calls CreatePowerPole/AddConnection |
| `cSC4PowerLineTool::CellIsOKForPlacement` (0x0022c1cc) | `FUN_0064fd00` | confirmed — only function referencing both GZCLSID `0x278128A0` and family `0x4A10AFC2` constants together |
| `cSC4PowerLineTool::CheckForReservedTiles` (0x0022c34c) | `CheckForReservedTiles` @ `0x006392f0` | confirmed — name survived directly |
| `cSC4PowerLineTool::ComputePolePosition` (0x0022b69c) | `FUN_0064efe0` | confirmed — called from CreatePowerPole/DeterminePolePositions matches |
| `cSC4PowerLineTool::FindPoleOccupant` (0x0022bf2a) | `FUN_0064ffe0` | confirmed — called from CreatePowerPole at matching position |
| `cSC4NetworkTool::GetOccupantExemplar` (0x0012441c) | `FUN_006244b0` | confirmed — exact TGI build `{0x6534284A, group, instance}`, IID `0xA52160F5` |
| `cSC4PowerPoleOccupant::StaticInit` (0x00256ac8) | `FUN_0064cc20` | confirmed — `0xC911E35B` + `0xE9AC43E7` both present |
| `cSC4PowerPoleOccupant::InitPowerLineControlVariables` (0x002554da) | `FUN_0064a170` | confirmed — `0xC911E35B` + `0xE9A18D3F` both present |
| `cSC4PowerPoleOccupant::GetControlPoints` (0x0025608c) | `FUN_0064a6c0` | confirmed — reads `sfMaxHorizontalControlPointDistance` global, 2 callers match AddConnection/UpdateConnection |
| `cSC4PowerPoleOccupant::InitConnectionPoints` (0x002556ac) | `FUN_00649b30` | confirmed — identical bit-extract `(prop>>8)&0xF`, 2-way branch to two table addresses |
| `cSC4PowerPoleOccupant::SetDefaultExemplar` (0x0025879a) | `FUN_0064d910` | confirmed — calls LoadModel + InitConnectionPoints-equiv + UpdateConnection-equiv, matches call shape |
| `cSC4PowerPoleOccupant::UpdateConnection` (0x00258346) | `FUN_0064d510` | confirmed — sole caller is the SetDefaultExemplar match above |
| `cSC4PowerPoleOccupant::AddConnection` (0x00258eee) | `FUN_0064e3e0` | confirmed — called directly inline from PlacePoles match, guarded by `this[0x50]==0` ⇄ `unaff_ESI[0x14]==0` |
| `cSC4PowerPoleOccupant::DrawPowerlines` (0x00257810) | `FUN_0064b750` | confirmed — reads `sPowerLineWidthFactors` global, sole non-StaticInit reader |
| `cSC4PowerPoleOccupant::operator.new` | `FUN_006499d0` | strong — called from CreatePowerPole match at matching position |
| `cSC4PowerPoleOccupant::cSC4PowerPoleOccupant` (ctor) | `FUN_0064db30` | strong — called right after operator.new match |
| `cSC4PowerPoleOccupant::GetLineConnectionPoints` (0x00254e4a) | **not found as standalone function** | likely inlined by MSVC at both call sites (AddConnection/UpdateConnection match) — see open step 5 |

### Windows data addresses

| Symbol | Mac address | Windows address |
|---|---|---|
| `sPowerPoleForDirectionsFlag` (16×uint32) | 0x00ab9840 | **0x00B467B0** |
| `sPowerLineWidthFactors` (vector\<float\> object, begin ptr) | 0x00c21080 | **0x00B46798** (end ptr `0x00B4679C`) |
| `_cardinalConnectionPoints` | 0x00c20fc0 | **0x00B0C0D8** |
| `_diagonalConnectionPoints` | 0x00c20f00 | **0x00B0C198** |
| `sfMaxHorizontalControlPointDistance` | (inline static) | **0x00B46754** |
| `sfHorizontalControlPointPercent` | (inline static) | **0x00B46750** |
| `sfMaxVerticalControlPointDistance` | (inline static) | **0x00B4674C** |
| `sfVerticalControlPointPercent` | (inline static) | **0x00B46748** |
| `sfCurveDistanceBetweenPoints` | (inline static) | **0x00B46744** |
| `sMinNumBezierPoints` | (inline static) | **0x00B46740** |

### Important: struct offsets differ between Mac (GCC) and Windows (MSVC) builds

Confirmed concretely, not assumed — same logical field sits at a different `this+` offset per
build:

- Connection-point table pointer: **Mac `this+0xe0`** vs **Windows `this+0xe4`** (`InitConnectionPoints` match).
- Network-tool "base group" source pointer (consumed by `GetOccupantExemplar`'s default-param
  path): **Mac `this+0x398`** vs **Windows `this+0x358`**.

Any actual DLL patch must re-derive every touched offset from the Windows decompile, not copy Mac
offsets — the two compilers do not lay out these classes identically (different padding /
base-class sizes). This is the single most important caveat for implementing anything from §6
above.

## 8. Ghidra database changes applied (2026-07-01)

All 19 confirmed-tier Windows functions renamed to their `cSC4...::Method` form (table in §7 — every
row's "Windows" column now reflects the live Ghidra symbol, not a `FUN_xxxxxxxx` placeholder).
`AddPowerPoleInfo` required `create_function` first since Ghidra hadn't auto-detected it as a function
boundary (it's only reached via an `EnumEntries` callback pointer, not a direct call).

Data globals renamed (Windows program enforces a `g_<hungarian>` naming gate that doesn't match SC4's
real `s`-prefix static convention — prefixed accordingly per user decision, real name preserved in full
after the prefix, original name also in the plate comment):

| Address | New name | Plate comment notes |
|---|---|---|
| 0x00B467B0 | `g_dwPowerPoleForDirectionsFlag` | = Mac `sPowerPoleForDirectionsFlag` |
| 0x00B0C0D8 | `g_abCardinalConnectionPoints` | = Mac `_cardinalConnectionPoints` |
| 0x00B0C198 | `g_abDiagonalConnectionPoints` | = Mac `_diagonalConnectionPoints` |
| 0x00B46798 | `g_abPowerLineWidthFactors` | = Mac `sPowerLineWidthFactors` |

## 9. `cSC4PowerPoleOccupant` struct recovery

Built from a full decompile of both constructors (Mac `cSC4PowerPoleOccupant::cSC4PowerPoleOccupant`
@ 0x00256e10 + `cSC4PowerPoleOccupant::Init` @ 0x00256d38; Windows ctor @ 0x0064db30) plus direct field
usage confirmed across `CreatePowerPole`, `DrawPowerlines`, `ChangeZoomLevel`, `SetPosition`,
`InitConnectionPoints` from earlier sections. Created in both Ghidra databases (`create_struct`,
`replace_placeholder=true` — both had a stale 1-byte demangler stub under this name already) and applied
via `set_function_this_type` to: ctor, `GetControlPoints`*, `InitConnectionPoints`*, `SetDefaultExemplar`,
`UpdateConnection`, `AddConnection`, `DrawPowerlines` (Mac also got `Init`). *Not yet applied on either
binary — `GetControlPoints`/`InitConnectionPoints` aren't analyzed as `__thiscall` on either side, so the
implicit-`this` binding rejected; needs `set_function_prototype` to force the calling convention first,
left as follow-up rather than risk it late in session.

### Mac struct (277 bytes, 41 fields) — all confirmed

| Offset | Field | Evidence |
|---|---|---|
| 0x00/0x04/0x08/0x0c/0x10/0x24 | `vtbl*` (6 vtable ptrs) | ctor base-class construction order |
| 0x2c | `dwClassMagic` = `0x2890d4de` | ctor literal write — likely CLSID, identical on Windows |
| 0x3c | `vtblModelInstanceBasic` | `cS3DModelInstanceBasic::cS3DModelInstanceBasic(this+0x3c)` in ctor |
| 0x50 | `bIsRemovingOrPreview` | gates the add-vs-remove-connection branch throughout `PlacePoles` |
| 0x51 | `bUnknownFlag51` | zeroed alongside 0x50, purpose not traced |
| 0x60 | `fScaleOrAlpha` | defaults to 1.0 |
| 0x64 | `transform` (56-byte blob) | `cS3DTransform::cS3DTransform(this+0x64)`; real `cS3DTransform` type is an unrecovered 1-byte demangler stub, so left as raw bytes rather than mis-typed |
| 0x9c/0xa0/0xa4 | `posX`/`posY`/`posZ` | `SetPosition`/`GetPosition`/`ComputePolePosition` write/read these directly |
| 0xa8/0xac/0xb0 | `connectionsBegin`/`End`/`Cap` | `vector<tConnection>` ctor call; iterated throughout `AddConnection`/`DrawConnection`/`PlacePoles` |
| 0xb4 | `dwDirectionFlags` | `CreatePowerPole` writes the direction-bitmask here, ORs with existing on merge |
| 0xb8 | `dwDirectionFlags2` | zeroed alongside 0xb4, not independently traced |
| 0xbc | `nCameraField` | `ChangeZoomLevel` writes a draw-context field here; default -1 |
| 0xc0 | `nCurrentZoomLevel` | `ChangeZoomLevel` writes zoom here; `DrawPowerlines` indexes `sPowerLineWidthFactors[this+0xc0]` |
| 0xc4/0xc8 | `nCellX`/`nCellZ` | integer cell coords, read in `AddConnection`/`UpdateConnection` |
| 0xcc | `bDirty` | defaults true in ctor, cleared/set around connection rebuilds |
| 0xe0 | `pConnectionPointTable` | `InitConnectionPoints` writes `&_cardinalConnectionPoints` or `&_diagonalConnectionPoints` here; `GetLineConnectionPoints` reads `(direction&3)*0x30` offset from it |
| 0xe4/0xf0/0xfc | `floorVerts*`/`wallVerts1*`/`wallVerts2*` (3x `vector<sVF_V3F_C4UB_T2F>`) | `CreateFloor`/`CreateWalls`/`AddWallQuad` mesh-vertex buffers, cleared in `Init` |
| 0x108–0x114 | misc flags + `pOwnerNetworkTool` | `pOwnerNetworkTool` (0x110) is called back from `SetPosition`; others zeroed in ctor, not independently traced |

### Windows struct (292 bytes, 32 fields) — mixed confidence, marked honestly

Directly confirmed via independent function usage this session (high confidence):

| Offset | Field | Evidence |
|---|---|---|
| 0x00/0x04/0x08/0x0c/0x10/0x24 | `vtbl*` | ctor, same relative order as Mac |
| 0x2c | `dwClassMagic` = `0x2890d4de` | ctor literal — **identical value and offset to Mac**, strongest cross-binary anchor found |
| 0x3c | `vtblModelInstanceBasic` | ctor calls `FUN_00801cf0(this+0x3c, 0)` at the same offset as Mac |
| 0xac/0xb0 | `connectionsBegin`/`connectionsEnd` | `DrawPowerlines` iterates exactly `this+0xac .. this+0xb0`, stride `0x2c` (44 bytes) per connection — **not the same offset as Mac's 0xa8/0xac**, found by direct re-decompile, not assumed |
| 0xb8 | `dwDirectionFlags` | `CreatePowerPole` writes the merged direction value here (Mac equivalent is 0xb4 — a real +4 semantic shift, confirmed, not guessed) |
| 0xc4 | `nCurrentZoomLevel` | `DrawPowerlines` indexes `g_abPowerLineWidthFactors[this+0xc4]` directly — initial guess of 0xc0 (naive Mac-offset-by-analogy) was **wrong**, corrected by checking actual usage |
| 0xe4 | `pConnectionPointTable` | `InitConnectionPoints` writes the cardinal/diagonal table pointer here (matches §7 finding) |

Everything else in the Windows struct is named `unk_*` deliberately — derived only from ctor
zero/default-value position matching against the Mac layout, **not independently confirmed** by a second
usage site the way the fields above were. Treat `unk_*` field names as starting hypotheses, not facts,
until each gets its own confirming xref the way `0xc4`/`0xb8`/`0xac` did.

## 10. Second RE pass (2026-07-01) — closing the DLL scaffold's gaps

Prompted by writing `PowerPoleCustomizationDirector.cpp` (§9's DLL, now at
`src/sample/power-pole-customization/`) — three of its four flagged gaps got closed by going back
into Ghidra, not guessed at.

**`cSC4NetworkRoutines::Get0To3Direction`** → Windows `0x0061f4c0`. Confirmed by decompiling both
sides: identical branch structure (compares two cell-coordinate pairs, returns 0/1/2/3), just
restructured by the optimizer. Renamed in the Windows DB.

**`cSC4PowerLineOccupant`** (the wire/strand object, not the pole) — found via the same
class-magic-constant technique as `cSC4PowerPoleOccupant`: Mac ctor (`0x002550ac`) writes
`0x0990B2EA` at `this+0x28`; searched Windows for that exact 32-bit constant, landed on
`FUN_0064a270` writing it at the identical `+0x28`. Confirmed:

| Method | Windows address |
|---|---|
| `operator.new` | `0x0064a2e0` (allocates from `cSC4StaticFloraData::SC4FixedPoolGrow`, same fixed-pool family flagged in `joutel-cold-load-crash` memory — a hand-rolled `new`/`malloc` here would be pool-incompatible with the matching `operator.delete`) |
| ctor | `0x0064a270` |
| `SetPosition` | `0x0064a460` |
| `GetPosition` | `0x0064a4a0` |
| `QueryInterface` | `0x006498c0` (thunk) |
| `Shutdown` | `0x00649270` (chains to `cSC4Occupant::Shutdown`) |

**The important correction, caught mid-session**: initially assumed Mac/Windows field layout was
identical here (ctor field-writes lined up exactly, offset-for-offset, unlike
`cSC4PowerPoleOccupant`). That was wrong, and the wrongness was self-inflicted — once a struct
gets applied to a function's `this` via `set_function_this_type`, Ghidra's *decompiled output*
for other functions in that class namespace re-labels raw offsets using that struct's field
names automatically. Decompiling Mac's `SetPosition` after creating the struct showed
`this->flPosX`/etc — which looked like independent confirmation but was actually just the
struct reflecting itself back. Raw **disassembly** (bypassing struct typing entirely) told the
real story:

- Mac `SetPosition`: writes position to `[EBX+0x38]/[+0x3c]/[+0x40]`, then caches truncated
  integer cell coordinates at `[+0x44]/[+0x48]`.
- Windows `SetPosition` (`0x0064a460`): writes position to `this+0x34/+0x38/+0x3c`, cell cache at
  `this+0x40/+0x44` — **shifted 4 bytes earlier than Mac**, despite every field before it
  (vtable pointers, the `0x0990B2EA` magic constant) landing at identical offsets on both sides.

Lesson for any future struct recovery here: a matching ctor zero-init pattern is not
confirmation by itself. It only becomes trustworthy once cross-checked against an independent
setter's *raw disassembly*, not its struct-typed decompile. Both `cSC4PowerLineOccupant` structs
(Mac and Windows) were corrected in Ghidra (`recreate_struct`) to reflect this.

**Update — fully resolved in a follow-up pass.** All 5 of `cSC4PowerLineOccupant`'s vtable
pointers were read and decompiled slot-by-slot:

- `this+0x00` (primary, 8 slots): `QueryInterface`/`AddRef`/`Release`, `GetConnectedPoles(+0xc)`
  (reads pole pointers from `+0x4c`/`+0x50` plus a byte from `+0x54`, with proper `AddRef` on
  each), `SetConnectedPoleA(+0x10)`, `GetCell(+0x14)`, `IsPreview(+0x18)`, `SetPreview(+0x1c)`.
- `this+0x04` (Occupant-shared, 16 slots): `QueryInterface`/`AddRef`/`Release`, a prepare-shutdown
  check, `Shutdown`, a flag getter, a subobject-cast helper, a generic shared getter,
  `GetPosition` (1-out variant), `SetPosition`, `GetPosition` (2-out variant), a cell-quantize
  helper, flag-ref add/remove/query, and a generic post-message method.
- `this+0x08` (serialization, 6 slots): `QueryInterface`/`AddRef`/`Release`, `Write`, `Read`,
  `GetGZCLSID` (returns the constant `0xC9C05C5D`).
- `this+0x0c`: a generic sorted-property-membership lookup (`__lower_bound` over a `uint32`
  array) — not connection-related.
- `this+0x20`: `QueryInterface`/`AddRef`/`Release`, `SetTraceFinal`, `GetModel` — both already
  named in the binary; a generic network-occupant interface, not connection-related.

The missing setter turned out to sit right next to the one already found: Ghidra treats
`SetConnectedPoleA` (`0x006492d0`) and a second, disassembly-confirmed entry point at
`0x006492f0` as **one physical function** (body `0x006492d0`–`0x00649328`) with two
vtable-callable entries sharing a tail — entry `0x006492d0` writes the pole pointer (with
AddRef/Release swap) into `+0x4c` and a byte into `+0x54`; entry `0x006492f0` does the identical
thing into `+0x50`/`+0x54`. Together: `SetConnectedPoleA(pole*, byte)` /
`SetConnectedPoleB(pole*, byte)`, matching Mac's combined 3-arg `SetConnectedPoles(poleA, poleB,
byte)` split into two calls. Found by decompiling `GetConnectedPoles` first (which reads both
`+0x4c` and `+0x50`), then hunting for the second setter once the first one's single-pointer
signature didn't match Mac's call site.

`CreateLineOccupant()` in the DLL now does the full sequence for real: allocate, construct,
`SetPosition`, `SetConnectedPoleA`, `SetConnectedPoleB`. `TopUpExtraStrands()` calls it instead of
only logging, since the object is now refcounted and reachable rather than an orphaned pool
allocation. **Still open, and now the load-bearing unknown**: whether pole registration alone is
sufficient for `DrawPowerlines` to render the strand, since that function (§6) reads polyline data
cached directly in each *pole's* own `tConnection` entry, not from the line occupant's position at
render time. This needs in-game verification, not further static analysis — it's a runtime
question (does a registered-but-not-tConnection-appended strand draw?), not a "find the address"
one.

## 11. DLL code review pass (2026-07-01) — two real bugs caught, one TODO advanced

A requested "verify the DLL, close remaining gaps" review, done by re-decompiling the functions the
DLL's constants were based on with corrected prototypes (several had broken calling-convention
detection until fixed with `set_function_prototype`, producing `unaff_EBX`/etc placeholder
registers that made earlier reads unreliable) and reading raw disassembly rather than trusting
struct-typed decompiler output (which reflects whatever struct is already applied — circular once
a struct exists, exactly the trap flagged in §10 for `cSC4PowerLineOccupant`, now caught again here
for `cSC4PowerPoleOccupant`).

**Bug 1 — wrong position offset.** The DLL read `occupant + 0x9c` as posX everywhere. Raw
disassembly of `cSC4PowerPoleOccupant::SetPosition` (found and renamed this pass, `0x0064c540`,
reached via the pole ctor's `this+8` vtable slot `+0x24` — same relative position as
`cSC4PowerLineOccupant`'s `SetPosition`, a pattern that held up) shows: `posX=0x98`, `posY=0x9c`,
`posZ=0xa0`. The DLL was reading Y where X belonged, Z where Y belonged, and unmapped memory where
Z belonged, for every custom attach point. Fixed to `0x98`.

**Bug 2 — genuine dual-use field, not a mistake on either side.** The same `SetPosition`
disassembly shows it also computes and writes truncated cell coordinates: `cellX=0xc0`,
`cellZ=0xc4`. But `0xc4` was *already* independently confirmed (§7, via `DrawPowerlines`) as
`nCurrentZoomLevel` — and re-verified this pass with clean raw disassembly
(`MOV EAX,[ECX+0xc4]` used directly as an index into `g_abPowerLineWidthFactors`). Both are real;
`this+0xc4` is a genuinely shared/reused field in the original game code — whichever of
`SetPosition` or `ChangeZoomLevel` ran most recently determines its current meaning. The DLL's
`DirectionBetween()` was reading it expecting cell-Z, which is only sometimes true. Fixed by not
reading any cached field at all: recompute `truncate(pos / 16.0f)` fresh from the confirmed
position fields, mirroring exactly what `SetPosition` itself does. (A third candidate,
`this+0xc8`/`this+0xcc`, appeared in one read of `AddConnection`, but that function still has
unresolved `unaff_` registers even after fixing its prototype — not trusted as a source either.)

**TODO advanced, not fully closed.** `InitConnectionPointsHook`'s stubbed exemplar retrieval: fixing
`InitConnectionPoints`'s prototype (it needed the same treatment as `AddConnection`) produced a
clean decompile showing the real retrieval sequence — call `occupant->pVtbl6` (`this+0x24`)'s own
vtable slot `+0x10` with `&occupant->pVtbl6` as the this-argument, then the *returned* pointer's own
slot `+0x2c` behaves like a property getter. What's still unconfirmed: whether that returned
pointer is ABI-compatible with the public `cISCPropertyHolder`/`cISCResExemplar` GZCOM interfaces —
this looks like SC4's internal, unexposed property-access path, and this session didn't verify its
vtable slot-for-slot against the public interface. Casting without that confirmation risks a crash,
not just a wrong read, so it's left as a `nullptr` stub with the real call sequence documented
rather than guessed at.

`cSC4PowerPoleOccupant`'s Ghidra struct (both the renamed fields and the corrected position
offsets) and `cSC4PowerPoleOccupant::SetPosition`/`DrawPowerlines`'s plate comments were all updated
in the Windows database to match.

## 13a. Bug found via the PoC run: SetConnectedPoleA/B was never two functions

The PoC crashed the game shortly after the log lines (exception in `cSC4PowerLineTool::PlacePoles`,
unrelated code, some time after our hooks returned -- classic stack-corruption signature). Cause:
§10/§11 described `0x006492d0`/`0x006492f0` as two independent vtable-callable entries
("SetConnectedPoleA"/"SetConnectedPoleB"). Wrong -- re-disassembling the full range shows
`0x006492d0`'s "pole A" code falls straight through to `0x006492f0` with no `RET` in between, and
the shared epilogue (`POP EDI; POP ESI; RET 0xc`) only exists once, at the very end. It's **one
function taking 3 stack args** (`poleA, poleB, byte`), writing `+0x4c`/`+0x50`/`+0x54` in a single
call -- exactly Mac's `SetConnectedPoles(poleA, poleB, byte)`. Calling `0x006492f0` directly (as
the DLL did) skips the real prologue, so its `POP`s pop bytes the caller never pushed. Fixed:
renamed `0x006492d0` to `cSC4PowerLineOccupant::SetConnectedPoles`, DLL now calls it once with all
3 args instead of twice with 2 each.

Lesson: `RET N` byte count and prologue/epilogue register pushes are the ground truth for "is this
really two callable things or one" -- a plausible-looking pair of "sibling" addresses at a
consistent offset gap isn't enough; trace the actual fallthrough/`RET` boundaries.

## 13. PoC result (2026-07-01): registration confirmed insufficient for rendering

Ran the `polelinetest` cheat in-game. Result: 8 poles got synthetic 6-point overrides, 6 pairs
correctly computed "2 extra strands in direction 2, vanilla already built 4" via the now-real
`DirectionBetween`/`StrandCount` logic, `CreateLineOccupant` ran (allocate + ctor + `SetPosition` +
`SetConnectedPoleA`/`SetConnectedPoleB`) with **no crash** for all 6 extra-strand builds. But no
extra wires were visible in-game.

This confirms — not just hypothesizes — §11 item 5: registering a `cSC4PowerLineOccupant` via
`SetConnectedPoleA/B` makes it a real, refcounted, non-crashing game object, but does **not** make
`DrawPowerlines` draw it. `DrawPowerlines` reads polyline data cached directly in each *pole's own*
`tConnection` entry (`this+0xac`/`+0xb0`), populated by `AddConnection`/`UpdateConnection`'s own
Bezier-curve-building loop — a `cSC4PowerLineOccupant`'s existence is orthogonal to that data path.

**Next real step, if pursued**: don't route extra strands through `cSC4PowerLineOccupant`
registration for rendering purposes at all (keep it only for the city-object/save-load side, which
is now proven functional). Instead, append additional polyline entries directly into the relevant
pole's own `tConnection.polylines` vector — the same `vector<vector<cS3DVector3>>` structure
`AddConnection` builds via `GetControlPoints` (already resolved, `0x0064a6c0`) and pushes into
`this+0x18`/`this+0x24`-relative offsets inside each `tConnection` (see §6 for the original
`AddConnection`/`DrawPowerlines` field layout). That inner structure wasn't independently
re-confirmed on Windows this session — the next scoped RE task, not a guess to act on yet.

## 12. PoC cheat code added (2026-07-01)

`PowerPoleCustomizationDirector.cpp` now has a `"polelinetest"` cheat code (search
`kCheatPoleLineTest`), following the `DateJumperDirector.cpp` pattern (`RegisterCheatCode` in
`PostAppInit`, dispatch via `DoMessage`'s `kCheatCodeMessageType` case, `GetData1()` for the cheat
ID). Toggling it makes every pole that goes through `InitConnectionPointsHook` (newly placed or
reloaded) get a synthetic 6-point override built from its own already-baked vanilla attach point,
duplicated with a small Y offset — no new art or exemplar needed. This isolates testing of exactly
the open question from §11 item 5: does `CreateLineOccupant` + `SetConnectedPoleA/B` registration
alone make `DrawPowerlines` render the extra strands, independent of the still-unresolved
exemplar-property-reading path. Every block is tagged `TEMPORARY PoC-ONLY` for easy removal once
that question is answered either way.

## 14. Real render path: append into tConnection.polylines (2026-07-01)

§13 confirmed `cSC4PowerLineOccupant` registration doesn't render. Real fix: `AddConnection`'s own
disassembly shows it builds each strand by calling `GetControlPoints` (`0x0064a6c0`, 2 endpoints ->
2 bezier control points + a length-based tessellation count) into a tessellator (renamed
`cSC4PowerPoleOccupant::TessellateBezierSegment`, `0x006493b0`, clean standalone decompile), then
appending the finished polyline via a vector-insert helper (renamed
`std_vector_polyline_list_insert`, `0x0064d020`) into the pole's own `tConnection.polylines`
(`vector<vector<cS3DVector3>>` at entry `+0x14`/`+0x18`) — confirmed as `insert(end(), 1, value)`
specifically (tail-relocate flag off), i.e. a plain append. Confirmed `0x0064d020` operates on the
*outer* vector (not a single polyline's own points) by decompiling its per-element copy helper
(renamed `std_vector_cS3DVector3_copy_construct`, `0x0050bcb0`): it deep-copies a `vector<cS3DVector3>`
(begin/end pointers, 3-float elements), which only makes sense one level up from a raw point.

Implemented in `PowerPoleCustomizationDirector.cpp`: `AppendPolylineToConnection()` builds a
temporary `{begin,end,cap}` view over its own heap buffer (never handed to the game — `insert`
deep-copies it, so no cross-allocator ownership issue) and calls the same three confirmed
functions. `TopUpExtraStrands()` now calls this for both endpoint poles instead of registering
`cSC4PowerLineOccupant` objects (that code was deleted from the file — the vtable mapping remains
here in §9/§10/§13a if city-object/save-load registration is wanted again later).

**Locating the right tConnection entry — got this wrong once already.** First attempt matched
entries by content, guessing `tConnection+0xc`/`+0x10` held the two endpoint pole pointers (based
on `AddConnection`'s existing-connection dedup check). Tested in-game: zero matches, every extra
strand skipped, no crash. Those fields are very likely packed cell coordinates, not pointers — the
same register-spilled variables in that decompile get compared against `__ftol2`-truncated
coordinates a few lines later, and that decompile was already flagged elsewhere in this file as
unreliable (unresolved `unaff_` registers even after fixing the function's prototype). Fixed by
dropping content-matching entirely: since the append always runs immediately after vanilla
`AddConnection`/`UpdateConnection` just touched the entry for this exact pair, it's reliably the
**last entry** in the pole's connections vector. Simpler and doesn't depend on the one offset pair
that's already been shown wrong.

Not yet verified in-game whether the *last-entry* assumption holds for `UpdateConnection` specifically
(rebuilding an existing connection, e.g. on exemplar/zone change, may update an entry in the middle
of the vector rather than the most recent one) — `AddConnection` (new placement) is the case tested
so far.

**Second in-game bug, same test round: wrong calling convention on `GetControlPoints`.** Declared
`__cdecl` based on how the decompiler rendered it (plain params, no visible `this`) — decompiler
rendering is not proof of calling convention. Its actual disassembly ends `ADD ESP,0x20; RET 0x24`
— `RET` with a nonzero immediate means the *callee* cleans the stack, i.e. `__stdcall`, not
`__cdecl`. Declaring it `__cdecl` made the DLL's call site *also* clean those 36 bytes after return,
double-popping the stack on every call — a `__report_gsfailure` (MSVC stack-cookie check) a short
time later, not immediately at the call site itself. Fixed by changing the typedef to `__stdcall`.
Cross-checked the other two new calls the same way: `TessellateBezierSegment` ends in a bare `RET`
(genuinely `__cdecl`, no fix needed), `PolylineListInsert` ends in `RET 0x14` matching `__thiscall`'s
callee-cleans-non-this-args behavior (already declared correctly). Lesson: `RET` vs `RET N` in the
real disassembly is the actual test for calling convention -- the decompiler's parameter rendering
is not.

**Third in-game bug, same test round: appending to both poles was wrong.** After the calling-
convention fix, no more crash, but roughly half of every batch of extra-strand appends still logged
"no tConnection entry found" -- specifically, always the *other* pole's side, never the `occupant`
side vanilla's own call had just touched. Root cause: `cSC4PowerLineTool::PlacePoles` calls
`AddConnection` twice per pole pair with arguments swapped (`this=poleA,other=poleB`, then
separately `this=poleB,other=poleA`) -- confirmed by this exact symptom, not just inferred -- and
each call only touches its own `this` pole's connections vector. `TopUpExtraStrands` was appending
to *both* poles per single hook firing, so the "other pole" append ran before that pole's own
separate `AddConnection` call (and thus its own fresh entry) existed yet. Fixed by appending only
to `occupant` (the `this` side) per hook firing -- the swapped call, already hooked at all 4 call
sites, naturally handles the other side when it fires moments later. Not yet re-tested in-game.

## Open next steps (updated)

1. Force `__thiscall` on `GetControlPoints`/`InitConnectionPoints` (both binaries) via
   `set_function_prototype`, then apply the struct as their `this` type too.
2. Resolve the `unk_*` fields in the Windows struct one at a time the same way `0xc4` and `0xb8` got
   resolved — decompile every function that touches that offset, don't assume Mac-offset-by-analogy.
3. Vtable recovery — not attempted yet. Each `vtbl*` field is currently an untyped `void *`; the actual
   vtable arrays exist in both binaries (referenced via `&PTR_QueryInterface_xxx` style labels in the
   ctors) and could be recovered as typed function-pointer arrays with per-slot names, using the many
   `(**(code**)(*this+OFFSET))(...)` call sites collected across this whole investigation as semantic
   evidence per slot. Sizable separate task — scope it fresh next session rather than rush it.
4. Cross-check all addresses above against Windows `SimCity 4.exe` 1.1.641 — done for the functions in
   §7; re-verify before writing a real patch.
5. Check whether `cSC4NetworkTool` already has an unused style-index field/cycle mechanism (for
   the multi-pole-type design in §6B).
6. Pin exact byte layout for the proposed new connection-point properties (property IDs from
   modder-reserved range, decide array-vs-fixed-struct encoding).
7. ~~Decide `GetModelInstanceID`'s instance-ID bit layout~~ — resolved 2026-07-03; see §15D.
8. Find `GetLineConnectionPoints`'s Windows equivalent — likely inlined by MSVC at both call sites
   (`AddConnection`/`UpdateConnection`) rather than a standalone function; not yet located as a
   separate symbol. If a hook needs a single chokepoint, patch both inline sites instead.

## 15. Investigation pass (2026-07-03): sag-per-style, inter-pole distance, fractional angles

Requested scope: make sag configurable per style/wire, inter-pole distance per style, and assess
fractional-angle support. Ghidra was offline this pass (no instance at 127.0.0.1:8089), so the two
items needing new addresses are scoped but not yet resolved — Ghidra queries listed below.

### A. Sag — per-wire already done; per-style is the gap

Per-wire sag already ships in `PowerPoleCustomizationDirector.cpp`: family defaults
`0xB22A0005` (sag scale) / `0xB22A0006` (max sag), per-wire arrays `0xB22A0008` /`0xB22A0009`,
applied in `AdjustControlPointSag` + `ApplyConnectionCustomization`. Nothing to add for per-wire.

Per-style has two disconnects: (1) `[PowerPoles.Name]` style sections carry only direction-mask →
instance-ID, no sag keys; (2) `gActiveStyleIndex` is global-at-placement-time and is **never
recorded per occupant** — it only steers `CreatePowerPole`'s instance pick, so at sag-apply time the
pole's style is unknown. Options: author sag on each style's exemplars (zero code), or add
`SagScale=`/`MaxSag=`/`WidthScale=` keys to the style section + a per-occupant style tag consulted in
`FindAppearanceOverride`. Decision pending.

### B. Inter-pole distance per style — IMPLEMENTED 2026-07-03

Shipped in `PowerPoleCustomizationDirector.cpp`: `[PowerPoles.<Name>]` sections accept
`InterPoleDistance=N` (cells, 1-50, vanilla 10). `DeterminePolePositions` (Win `0x00650840`) is hooked
at all 3 call sites; the hook saves the tool's `this+0x3a4` field, writes the active style's value,
runs vanilla, then restores. Verified in-game (builds + works). Original investigation notes below.

### B (original). Inter-pole distance per style — feasible, needs one RE pass

Distance = "Max cells between power poles" property `0x098B25C8` (=10) on the Utilities exemplar,
read at `InitPowerLineControlVariables` (Win `0x0064a170`) into a global static, consumed by
`DeterminePolePositions` (Win `0x00650840`). Unlike the sag case, style index is live during drag
(before poles exist), so no per-occupant plumbing — patch the static read to a `gActiveStyleIndex`
resolver, same shape as the existing `PoleStyleLookupHook`.
**Ghidra to resolve:** (1) which `0x00B46xxx` static `0x098B25C8` writes in
`InitPowerLineControlVariables` (§7 mapped the curve/sag statics but not max-cells/min-height/
curve-distance); (2) the read site inside `DeterminePolePositions` to patch.

### C. Fractional angles (FAR) — attach-rotation is cheap now, mesh/position are not

FAR angle = `arctan(1/n)`: FAR-6≈9.46°, FAR-3≈18.43°, FAR-2≈26.57°, FAR-1.5≈33.69°. Vanilla power
system knows only 4 orientations (`Get0To3Direction`). Three separable layers:

- **Wire endpoints:** already arbitrary (bezier between the two poles' world positions) — free.
- **Attach-point alignment:** table keyed by `direction&3` snaps cables to nearest of 4 orientations.
  **New insight:** this DLL already reimplements attach reads (`GetAttachPoint`) and has *both* poles
  in `ApplyConnectionCustomization`, so rotating each attach offset by the true bearing
  `atan2(dz,dx)` aligns cables at any angle — pure code, no art. Vanilla couldn't (flat translation
  add, no rotation matrix, §6C). This is the one cheap, high-value FAR win.
- **Pole mesh:** `GetModelInstanceID` picks ~4 discrete rotated S3D variants → mesh faces nearest
  45°, visual seam. Needs new art (or accept seam). Bit layout still unmapped.
- **Pole position:** grid-quantized (`ComputePolePosition = cellX*16`). Confirmed 2026-07-03 this is
  **NOT** a blocker for canonical FAR: those angles hit integer grid cells periodically
  (FAR-3=arctan(1/3)→cell (3,1); FAR-2→(2,1); FAR-6→(6,1); FAR-1.5→(3,2) over 2 periods). Poles land
  on real cells, just spaced 1-in-n. Earlier draft overstated this as a blocker.

**The actual blocker (user, 2026-07-03): can't drag PPs at FAR angles.** The drag tool snaps the
anchor→cursor line to 8 directions (H/V/diagonal, i.e. `Get0To3Direction`'s 4 undirected orientations)
and staircases anything else, so a FAR run is never produced in the first place — wire alignment and
pole position are both moot until placement can march poles along a 1-in-n slope.

**Ghidra to resolve (in priority order):**
1. ~~**Drag direction-snap site**~~ — RESOLVED 2026-07-03, see §16 below.
2. ~~`GetModelInstanceID` instance-ID bit layout~~ — RESOLVED 2026-07-03, see §15D below.
3. Pole yaw storage (`cS3DTransform` occupant+0x64 Mac / Win TBD) — sidestepped by deriving bearing
   from the two poles' positions; needed only if a stored yaw is preferred.

### D. `GetModelInstanceID` rotation layout — RESOLVED 2026-07-03

Mac reference: `cSC4ModelMakerUtility::GetModelInstanceID` at `0x002059d4`; the power-pole caller is
`cSC4PowerPoleOccupant::LoadModel` at `0x00255d0c`. For Resource Key Type 1 (`0x27812821`) it returns:

```text
baseInstance + LOD*0x100 + rotation*0x10
```

Windows confirms the same arithmetic in `FUN_00496830`, reached from the occupant overload
`FUN_00497180` and `cSC4PowerPoleOccupant::LoadModel` at `0x0064ad90`. Windows `LoadModel` passes a
one-quarter-turn correction when `dwDirectionFlags` is exactly `0x1` or `0x8`. This lets the same
logical straight model serve X and Z, and the same logical diagonal model serve both diagonals.

For FAR, quarter-turn rotation pairs `XP` with `ZN`, and `XN` with `ZP`. Those two pairs are mirrors
and cannot be transformed into one another by 90° rotation. Result: **two logical pole models per
ratio per family**, not four and not one. Each logical model still needs the ordinary camera-rotation
and LOD resources required by its RKT1 model.

## 16. Drag direction-snap located (2026-07-03) — the FAR-drag blocker

The 8-direction snap that prevents dragging power poles at FAR angles lives in the **base
`cSC4NetworkTool`**, not the power-line tool. Drag pipeline:

```
mouse drag → DrawNetworkLine → BreakIntoStraightAndDiagSegments   (snaps anchor→cursor to H/V/45°)
                             → ComputeDraggedCells                (rasterizes the cell path)
           → cSC4PowerLineTool::DeterminePolePositions            (consumes this+0x54 dragged steps,
                                                                    this+0x60 cells)
           → PlacePoles
```

`DrawNetworkLine` clears `this+0x54` (dragged-step vector) / `this+0x60` (cell vector) / `this+0x70`
(=0), calls `BreakIntoStraightAndDiagSegments`, stores the diagonal flag at `this+0x6c`, then (unless
straight-only) calls `ComputeDraggedCells`. `DeterminePolePositions` later reads exactly those
`this+0x54`/`+0x60` fields — confirmed the same offsets on both binaries.

**`BreakIntoStraightAndDiagSegments`** is the snap: computes `|dx|,|dy|` of anchor→cursor, forces the
cursor to the nearest of horizontal / vertical / 45°-diagonal (2:1-ratio test, plus a ±2 near-diagonal
correction), and emits 8-way direction codes (0–7) in its two out-params. A per-tool-instance flag
(`this+0x2b5` Win / `this[0x2f5]` Mac) selects straight+diagonal (0) vs straight-only (≠0).

### Confirmed addresses

| Function | Mac | Windows | Evidence |
|---|---|---|---|
| `cSC4NetworkTool::BreakIntoStraightAndDiagSegments` | `0x004cb7d4` | **`0x00637eb0`** (body –`0x00638153`) | leaf; identical 11-param `__thiscall`; sole caller = DrawNetworkLine; byte-identical snap logic |
| `cSC4NetworkTool::DrawNetworkLine` | `0x004cdd0a` | **`0x0063af40`** (body –`0x0063b120`) | already labeled `??DrawNetworkLine`; calls BreakInto + ComputeDraggedCells; `imul …,0x114` (sNetworkTypeInfo stride) |
| `cSC4NetworkTool::ComputeDraggedCells` | `0x004cc96a` | `0x00639790` (probable) | DrawNetworkLine callee; `imul …,0x114` consumer; rasterizes cells (does NOT snap) |
| `cSC4PowerLineTool::DeterminePolePositions` | `0x0022c98a` | `0x00650840` | §7 |

### Windows field offsets (on the tool instance)

- `this+0x54` dragged-step vector (stride 0xc), `this+0x60` cell vector (stride 8, `SC4Point<uint>`),
  `this+0x70` total step count, `this+0x74` straight step count, `this+0x78` valid step count.
- `this+0x118/0x11c` anchor-cell vector. `this+0x2b5` snap-mode flag.
- **`this+0x3a4` = Max cells between power poles** (item B: read at
  `*(this+0x3a4) <= (curStep - lastPoleStep)`; Mac `this+0x3f0`).
- `this+0x3a8` = min pole height float added to pole Y (Mac `this+0x3f4`).

### Implications for FAR-drag

- The snap is **shared by every network tool** (road/rail/street/power) — any patch to
  `BreakInto`/`DrawNetworkLine` must gate on the active tool being `cSC4PowerLineTool`, or it changes
  road/rail dragging too. The `this+0x2b5` mode byte is per-instance, so a third "FAR-allowed" mode
  could be encoded there for the power tool only.
- To lay poles along a 1-in-n FAR slope, `BreakInto` must be allowed to emit a FAR segment (not
  snapped to 8-way) AND `ComputeDraggedCells`/`DeterminePolePositions` must place poles only at the
  periodic integer FAR nodes ((3,1),(6,2)… — see §15C) rather than at every staircase corner. That's
  the implementation design for a follow-up; the snap itself is now fully mapped.

### Suggested Ghidra renames (pending confirmation, Windows program)

- `0x00637eb0` → `cSC4NetworkTool::BreakIntoStraightAndDiagSegments` (confidence: confirmed)
- `0x00639790` → `cSC4NetworkTool::ComputeDraggedCells` (confidence: strong)
- `0x0063af40` `??DrawNetworkLine` → `cSC4NetworkTool::DrawNetworkLine` (confidence: confirmed)

## 17. FAR-drag investigation (2026-07-03) — Path A confirmed viable

Goal: let the power tool drag along fractional angles (FAR-n = arctan(1/n)). Two candidate
architectures were on the table: (A) hook `DrawNetworkLine`, synthesize the dragged-step/cell
arrays, let vanilla `DeterminePolePositions` place poles; (B) skip vanilla and build the
pole/connection graph directly. **Path A wins** — every placement rule in
`DeterminePolePositions` was pinned and none of them breaks a synthetic FAR step sequence.

### A. What actually forces a pole in `DeterminePolePositions` (Win `0x00650840`)

Full decompile of the Mac twin (`0x0022c98a`, symbolized) cross-checked against Windows for the
load-bearing offsets. The function iterates steps `0..this+0x78`, and within each step iterates
that step's cell-index range. A pole is placed at a cell ONLY when one of these fires
(confidence: confirmed unless noted):

1. **Anchor match** — the cell equals the next unconsumed entry of the anchor-cell vector
   `this+0x118` (callers push the drag endpoints into it), OR it is the last cell of the last
   valid step. Forces a pole; consumes the anchor entry.
2. **Max-cells cadence** — evaluated only at a step's *primary cell*: forces a pole when
   `curStepIdx - stepIdxOfLastPole >= *(this+0x3a4)`. **The cadence counts STEP INDICES, not
   cells** (Windows read at `0x3a4` confirmed in §16; Mac twin uses `this+0x3f0` identically).
   Vanilla steps are ~1 cell wide along the drag's major axis, which is why the property reads
   as "max cells".
3. **Existing pole or crossing power line at the cell** (`FindPoleOccupant` /
   `CellIsOccupiedByPowerLine`). The `GetLineSlope` call everyone suspected of being a
   direction-break test is ONLY used here — axis-aligned-crossing dedup against
   `IntersectionIsInList`/`PoleIsInList`, gated to the straight region (`stepIdx < this+0x74`).
4. **Terrain line-of-sight break** — at a primary cell with no other force: casts a ray
   (renderer vtable `+0xf0`) from the last pole's position (+min-height `this+0x3a8`) toward
   the NEXT cell's position; if terrain intersects closer than the span length, forces a pole.
   This is the real "slope break" — it is a terrain-clipping test, not a path-direction test.
   Skipped entirely whenever the pole is already forced by 1–3 (the force flag short-circuits
   before the LOS branch).

Non-primary, non-forced cells are only appended to the current span's `tLineInfo` list
(cell-path bookkeeping for the wire) — they structurally cannot take a pole.

### B. `GetPrimaryCell` = Windows `FUN_0064f230` (confirmed)

Identical branch structure and offsets to Mac `0x0022bb3a`: reads step `param_1` from
`this+0x54` (stride 0xc). If `this+0x74 == 0 || stepIdx >= this+0x74` (diagonal region):
returns the step's cell whose `|cellX-anchorX| == |cellZ-anchorZ|` vs anchor
`this+0x118[param_2]`, else -1. Otherwise (straight region): **returns the step's first cell,
unconditionally.**

### C. Why Path A works

Feed the tool synthetic arrays shaped as: one dragged step per FAR period whose FIRST cell is
the period's integer node, a final single-cell step holding the last node, and
`this+0x74 = this+0x70 = stepCount` (everything "straight", so primary = first cell and the
§A pre-loop straight/diag-junction special case is skipped). Scope `this+0x3a4 = 1` for the
drag (the existing per-style hook already owns this field): rule 2 then forces a pole at every
step's primary cell = every FAR node, rule 1 covers both endpoints, rule 4 is short-circuited,
and no other rule can fire mid-period. `this+0x78` doesn't need writing — every caller sets
`this+0x78 = this+0x70` right after `DrawNetworkLine` returns (verified in `PlacePoles`
`0x00651560` and the commit wrapper `FUN_00652980`).

### D. Tool gating — `DrawNetworkLine` is virtual; patch one vtable slot

`0x0063af40` has **no direct call xrefs** — only DATA refs from 5 network-tool vtables
(`0x00aa8790`, `0x00aa99f0`, `0x00aa9f78`, `0x00aaa608`, `0x00aaa998`), i.e. every invocation
is a virtual call through slot `+0x48`. `cSC4PowerLineTool`'s ctor (`0x006503c0`, confirmed —
writes the class's statics and calls the `cSC4NetworkTool` base ctor with network type 5)
stores primary vtable **`0x00aa9f30`** at `this+0` and secondary `0x00aa9f20` at `this+4`.
`0x00aa9f30 + 0x48 = 0x00aa9f78` = the power tool's own `DrawNetworkLine` slot (content
verified `0x0063af40` by raw memory read; `Init` `0x00650630` sits at `0x00aa9f3c` in the same
vtable). **Patching that single dword hooks the power tool only** — road/rail/street keep
their own vtable slots untouched. No runtime class check needed.

### E. Windows `DrawNetworkLine` facts (disassembly-verified)

- `__thiscall`, 4 stack args `(SC4Point<uint>& start, SC4Point<uint>& end, char straightOnly?,
  int networkType)`, ends `RET 0x10`, returns success in AL.
- Clears steps (`this+0x54/0x58` begin/end, cap `0x5c`) and cells (`this+0x60/0x64`, cap
  `0x68`), zeroes `this+0x70`, calls `BreakIntoStraightAndDiagSegments` (`0x00637eb0`) with
  out-params `&this+0x70` (total steps), `&this+0x74` (straight steps), `&this+0x7c`,
  `&this+0x80` (8-way direction codes) + two locals; writes diag flag byte `this+0x6c`;
  honors straight-only mode byte `this+0x2b5`; calls `ComputeDraggedCells` (`0x00639790`);
  bounds-checks every cell against city dims `this+0x240/0x244` (clears everything and
  returns 0 on violation). `sNetworkTypeInfo` base = `0x00b452c8`, stride `0x114` (widths at
  `+0x1c/+0x20`).
- **`BreakIntoStraightAndDiagSegments` writes the snapped endpoint back into `end`** — callers
  consume the snapped point afterward, so a FAR hook must do the same.
- `tDraggedStep` = `{uint32 firstCellIdx, uint32 lastCellIdx, uint32 zeroed}` (stride 0xc;
  third dword set to 0 by `ComputeDraggedCells`, purpose untraced). Cell vector elements =
  `SC4Point<uint> {x, z}` (stride 8).
- `ComputeDraggedCells` (Mac decompile) opens a new step on every advance along the drag's
  major axis and extends the current step's cell bounds on minor-axis advances — a vanilla
  straight drag of L cells yields ~L one-cell steps, a 45° drag yields per-column two-cell
  steps. Its walk is greedy (straight run + diagonal tail), NOT slope-proportional — so
  reusing it for FAR rasterization would give L-shaped cell paths; a FAR hook must rasterize
  its own supercover staircase.

### F. Hook allocation strategy (design, not yet verified in-game)

The step/cell vectors use the game's allocator; the DLL must not push_back into them with its
own heap. Plan: the hook first calls the ORIGINAL `DrawNetworkLine` with a fake straight drag
of exactly the needed cell count (chosen along ±x toward map interior, so vanilla's own path
performs all allocation), then rewrites the step/cell contents in place (both element types
are PODs) and shrinks the two end pointers. Runtime-validates resulting capacity and falls
back to a plain vanilla call on any mismatch.

### G. Related facts picked up on the way

- `FUN_006520c0` (caller of `DeterminePolePositions` site `0x006522bf`) is the
  auto-connect/neighbor-connect path: walks a candidate cell list, pushes both endpoints into
  the anchor vector `this+0x118/0x11c/0x120`, virtual-calls `DrawNetworkLine` via slot
  `+0x48`, sets `this+0x78 = this+0x70`, then `DeterminePolePositions` + `PlacePoles`
  (confidence: strong).
- Power tool network type lives at `this+0x35c` (passed as `DrawNetworkLine`'s 4th arg).
- `Get0To3Direction` (`0x0061f4c0`): same z → 0; same x → 2; otherwise 1 or 3 by sign
  agreement — **any non-axis-aligned delta, including every FAR span, classifies as a
  diagonal (1 or 3)**. So FAR spans get the diagonal pole model + the diagonal attach-point
  table; the mesh will visually sit at 45° until real FAR art exists (known caveat), and cable
  attach offsets need a true-bearing rotation to align (implemented in the DLL as the
  attach-rotation feature).
- `FUN_0064f170` / `FUN_0064f1d0` are the pole-in-list scans over the `tPoleInfo` vector
  `this+0x36c..0x370` (stride 0x30) — by cell+flags and by occupant pointer respectively
  (confidence: strong; matches Mac `PoleIsInList` pair).

## 18. Zoned-building construction interrupts power lines — cause and preservation option

Zoning itself does not contain a power-line-specific removal path. The interruption occurs later,
when a zoned lot starts construction. Mac `cSC4LotDeveloper::StartConstruction` (`0x000f48d0`)
calls `ClearLotBlockingObjects` (`0x000f044a`). That function builds the lot's 3D bounding box,
queries every intersecting occupant, and passes each result to the generic occupant-removal service.
Both real `cSC4PowerPoleOccupant` objects and the hidden per-cell `cSC4PowerLineOccupant` objects
registered along a span are returned by that sweep.

Windows confirms the same path:

- `cSC4LotDeveloper::StartConstruction` twin: `FUN_006cc490` (confirmed by call shape and the
  construction-occupant GZCOM IDs `0xA97F909E/0x496E7636`);
- `ClearLotBlockingObjects` twin: `FUN_006c9ad0`, called from the construction path;
- spatial query: virtual call at `0x006c9be0`;
- unconditional removal loop: `0x006c9bf2..0x006c9c15`, with the removal service call at
  `0x006c9c07`.

The two relevant occupant identities are reliable and already exposed through `QueryInterface`:

- power pole CLSID/IID accepted by QI: `0x09C05C6A` (Windows compare at `0x0064a054`);
- hidden power-line occupant CLSID/IID: `0xC9C05C5D` (Windows compare at `0x0064935b`).

**Runtime result (2026-07-03): this was the wrong path.** Repeated empty-zone tests produced no
power occupant classifications in this loop. A WinDbg breakpoint at
`cSC4PowerLineOccupant::Shutdown` (`0x00649270`) captured the real chain:

`UpdateOnZoneChange 0x006522f0` -> reciprocal `RemoveConnection` calls at
`0x0065279f/0x006527a9` -> pole-side removal at `0x0064da00` -> hidden occupant removal at
`0x0064a560` -> generic occupant removal (`0x0049f240`) -> `Shutdown`.

Mac symbols confirm the Windows match: `cSC4PowerLineTool::UpdateOnZoneChange` (`0x0022e388`)
tests the changed zone rectangle, finds a connection whose cell path intersects it, calls
`cSC4PowerPoleOccupant::RemoveConnection` on both endpoints, and then attempts to reposition and
rebuild the line. That rebuild succeeding only for some geometry caused the misleading intermittent
result. The DLL no longer patches `ClearLotBlockingObjects`. Instead, `keepwires` patches the first
seven bytes of Windows `UpdateOnZoneChange`; disabled mode reproduces the original prologue, while
enabled mode returns immediately with `RET 0x14`, bypassing the complete remove/reposition/rebuild
transaction. Manual bulldozing and generic occupant removal remain untouched.

This deliberately allows buildings and power-line occupants to coexist spatially. It can preserve
continuity, but a building may visually intersect a pole or cable, and lot terrain levelling may
leave a retained pole at an undesirable height. For now it is runtime test state only: enter
`keepwires` to toggle it, then allow a zoned lot intersecting an existing line to grow. A more
polished policy would preserve crossing spans but relocate a pole that lies inside the building
footprint; that is a substantially larger graph-editing feature.

## 19. FAR neighbor connections fail — three confirmed causes

Windows `cSC4NetworkTool::AttemptNeighborConnections` is `0x00628bd0`. It replays the last two
anchor points through virtual `DrawNetworkLine` at `0x00628c44`, then reads the resulting final
8-way direction from `tool+0x7c` at `0x00628c5f`. It converts `direction >> 1` into an outward
cardinal step using tables at `0x00AA8390` (X = `[-1,0,1,0]`) and `0x00AA83A0`
(Z = `[0,-1,0,1]`). It also reads `tool+0x80` in the mixed straight/diagonal path. City dimensions
are `tool+0x240/+0x244`, confirmed independently in both `AttemptNeighborConnections` and
`DrawNetworkLine` (`0x0063b054`/`0x0063b067`).

The original FAR hook violated three assumptions:

1. It calls vanilla with a fake allocation drag, then rewrites cells but leaves `tool+0x7c/+0x80`
   describing the fake +X drag. Neighbor detection therefore tests the wrong city edge.
2. Its constants incorrectly read city dimensions at `tool+0x280/+0x284`; those offsets came from
   an earlier cross-binary layout assumption and are wrong for Windows 1.1.641.
3. It rounds the endpoint back to the nearest whole FAR period. Neighbor creation requires the
   terminal pole to occupy the boundary cell so that one outward cardinal step leaves the city.
   A FAR lattice node reaches that cell only when the anchor-to-edge distance happens to be divisible
   by the ratio's major step.

**Implemented fix:** the hook now uses `+0x240/+0x244`; when a FAR drag targets a city edge, it
preserves all complete FAR periods and synthesizes a final supercover transition to the actual
boundary cell. It also writes `+0x7c/+0x80` to the even
cardinal direction code for the crossed edge (`0=−X`, `2=−Z`, `4=+X`, `6=+Z`). Interior poles remain
on exact FAR-period nodes. The shortened terminal span is a transition segment and uses the normal
fallback model policy until dedicated transition art exists.
