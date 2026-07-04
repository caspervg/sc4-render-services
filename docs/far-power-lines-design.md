# FAR power lines — style-table design note

Status: FAR model routing implemented (2026-07-04). Automatic FAR dragging, Shift-constrained
regular dragging, attach-point true-bearing rotation, and named `REG.*` parsing were already in.
Now added: named `FAR-<ratio>.<orient>` parsing + a `[PowerPoles.FAR]` default section
(`ParseFarHeadingKeys`), the resolution-order lookup (`ResolveFarInstance`), drag-time heading
capture (`gFarDragRatioIndex`/`gFarDragOrient`) feeding `ResolvePoleInstanceForDirectionMask`, and
the per-endpoint attach-yaw refactor with an optional `kPropAttachBasisDegrees` exemplar property
(`ComputeSpanBearing`/`AttachBasisAngle`, replacing the single `ComputeYawDelta`). Not yet done:
in-game verification, and authoring pole prop exemplars that point the `FAR-*` keys at real angled
S3D art.

## Art source found (2026-07-04)

`Documents/SimCity 4/Plugins/P3/MTO_P3_01` (the "P3" set) contains complete FAR-angle power-pole
meshes: e.g. `MTO_P3_01_03_NL266` = *"P3 138 kV Wood H-Frame Pole Single-Circuit 26.6-degree
Tangent"* (a straight FAR-2 run pole), plus turn/transition pieces (`TL266L634` etc.) and an FA3
(18.4-degree) set. Meshes live in `MTO_P3_ModelsVol01` (S3D group `0x5AD0E817`); the P3 exemplars
reference them via `ResourceKeyType1`. They are authored as **plop lots** (Exemplar Type 0x02, group
`0x1E78397A`) -- the classic can't-drag-FAR workaround this feature removes -- not as pole props in
the power-pole group. To wire `FAR-*` routing to this art, author prop exemplars in the pole group
whose RKT1 points at the same P3 S3D model TGIs, then name those instance IDs in the `FAR-*` keys.
These P3 models carry no custom attach-point properties, so a routed pole uses the vanilla diagonal
attach table (nominal 45-degree basis) and the existing single-basis yaw rotation already aligns its
cables; `kPropAttachBasisDegrees` is only needed if a future exemplar bakes attach points in the FAR
basis itself.

## What already works without new data

- **Cables**: correct at any angle, code-only. `ApplyConnectionCustomization` computes the span's
  true bearing, and when it deviates from the direction's nominal angle (0/45/90/135°) it rebuilds
  the strands with each attach offset yaw-rotated onto the real span direction. Applies to vanilla
  4-point tables and custom attach-point properties alike.
- **Pole placement**: FAR nodes are integer grid cells, so `ComputePolePosition`'s grid
  quantization is a non-issue. Pole cadence converts the max-cells value into whole FAR periods.

## Player interaction: automatic snap with Shift constraint

FAR ratio is not a separate pole style or a mode the player cycles. During an ordinary power-line
drag, the hook selects the closest supported heading from the regular axes, 45° diagonals, and all
configured FAR ratios. The active pole style then resolves the corresponding `REG.*` or `FAR-*`
model automatically.

Holding **Shift** restricts the current drag to regular orthogonal/diagonal headings and sends it
through the original vanilla path. Pressing or releasing Shift during a drag updates subsequent
previews and the committed result. Automatic selection uses angular distance in the first octant
and reflects the result into the other seven; a 1° hysteresis band keeps the preview from flickering
between adjacent headings near a snap boundary. Player-facing labels should use the integer slope
and angle (`2:1 — 26.6°`) rather than requiring familiarity with the internal `FAR-2` name.

## The gap: pole *mesh* orientation

`Get0To3Direction` classifies every non-axis-aligned span as diagonal (1 or 3), so a FAR span gets
the diagonal pole model, visually rotated 45° while the wires run at e.g. 26.57°. The engine picks
the mesh via the direction-mask → instance-ID table (`[PowerPoles]` / `[PowerPoles.<Name>]`), and
`GetModelInstanceID` only selects among pre-authored rotated S3D variants — a correct FAR pole
mesh therefore **requires new art**. Code can only route to that art once it exists.

## Pipeline already in place

Model choice happens in `CreatePowerPole` (Win `0x00650140`): 4-bit direction mask → instance ID.
The DLL already intercepts both lookup sites (`ResolvePoleInstanceForDirectionMask`, the polestyle
hook), so routing to FAR models is a resolver extension — no new RE needed for selection itself.

## Proposed data model: named regular masks and FAR-heading buckets

Do not expose the engine's `0x00`–`0x0f` direction-mask keys in the new style format. They are
compact for the game, but opaque to an author. Use `REG.<connections>` keys for the 16 regular
connection combinations and `FAR-<ratio>.<orient>` keys for exact FAR headings. Both still compile
to the engine-facing lookup data internally.

```ini
[PowerPoles.Steel]
REG.None=0x06000205
REG.X=0x06000205
REG.DP=0x06000405
REG.X+DP=0x06000305
REG.Z=0x06000205
REG.X+Z=0x06000105
; ...remaining regular combinations may be supplied or inherited

FAR-2.XP=0x06000505
FAR-2.ZN=0x06000505        ; perpendicular: same model, engine quarter-turn
FAR-2.XN=0x06000605
FAR-2.ZP=0x06000605        ; perpendicular: same model, engine quarter-turn
InterPoleDistance=14
```

- **Regular direction tokens**: the four bits of the engine mask become four short, documented
  tokens. `X` is the undirected x axis, `Z` is the undirected z axis, `DP` is the positive
  diagonal (+x,+z), and `DN` is the negative diagonal (+x,−z). `+` means that a pole carries more
  than one kind of connection; it is not a directed vector addition.
- **Canonical token order**: combinations are always written `X`, `DP`, `Z`, `DN`, in that order.
  Thus `REG.X+Z` is valid and canonical; `REG.Z+X` is rejected rather than creating aliases for
  the same mask. Keys should otherwise be parsed case-insensitively.
- **All regular keys**: these are the complete 16 masks. They are 16 connection cases, not 16
  necessarily distinct models — multiple keys may intentionally name the same instance ID, as
  Maxis's table does.

  ```text
  REG.None
  REG.X
  REG.DP
  REG.X+DP
  REG.Z
  REG.X+Z
  REG.DP+Z
  REG.X+DP+Z
  REG.DN
  REG.X+DN
  REG.DP+DN
  REG.X+DP+DN
  REG.Z+DN
  REG.X+Z+DN
  REG.DP+Z+DN
  REG.X+DP+Z+DN
  ```

- **FAR key format**: `FAR-<ratio>.<orient>`, ratio ∈ {1.5, 2, 3, 6} (the drag PoC's table). A span is
  undirected (looks identical from both ends), so per ratio only **4 distinct orientations**
  exist — 1:n tilted off the x axis with the minor step in +z or −z (`XP`/`XN`), and the mirrored
  n:1 pair off the z axis with the minor step in +x or −x (`ZP`/`ZN`). For example, FAR-2 maps
  `XP=(2,1)`, `XN=(2,−1)`, `ZP=(1,2)`, and `ZN=(−1,2)`. The full key set is those four orientations
  for each supported ratio: 16 FAR keys total.
- **Heading capture**: the drag hook already knows `(ratio, orientation)` when it snaps the drag;
  stash that selection in drag-scoped state for `PlacePoles`/`CreatePowerPole`. The direction mask
  alone cannot express a FAR heading. Do not treat that transient state as occupant metadata,
  however: authored mesh and attachment-basis headings must be recoverable from the selected
  instance/exemplar when an occupant is initialized or reloaded from a saved city.
- **Resolution order** (extended `ResolvePoleInstanceForDirectionMask`):
  1. exact `FAR-<ratio>.<orient>` key in the active style;
  2. same key in a new `[PowerPoles.FAR]` default/fallback section;
  3. **nearest-45° fallback**: the active style's `REG.*` entry for the mask the span already
     classifies to;
  4. the built-in vanilla entry if that regular key is also absent. Sparse style sections therefore
     degrade without unexpectedly changing pole family before the final fallback.
- **Junctions/merges**: a pole whose cell already carries other connections ORs direction masks
  together (multi-bit mask) and always takes the vanilla mask path. FAR buckets apply only to
  clean two-connection mid-line poles.

## Two real wrinkles

**Mesh heading and attachment basis are separate.** Selecting a FAR-authored mesh does not by
itself prove that its attachment-point table was authored in the same basis. Record both pieces of
metadata for each FAR instance/exemplar. Connection rebuilding must calculate a separate delta at
each endpoint: `deltaA = spanBearing − attachBasisA`, `deltaB = spanBearing − attachBasisB`. The
current single `ComputeYawDelta` value cannot correctly handle a mixed FAR/fallback span. Metadata
must be reconstructed during `InitConnectionPoints`, not retained only in a pointer side table from
the original drag, so save/reload remains correct.

**Endpoint/transition poles.** A FAR run's end pole often also carries a straight continuation —
the mask calls it a junction, the art would want a purpose-built transition model. Cheap and
recommended initially: let it fall through to the vanilla catchall (works, mildly ugly). Proper
transition models (`FAR-2.XP+straight` etc.) are combinatorial; do not model them until the base
set proves out.

Neighbor connections add another transition case: the engine requires the terminal cell to lie on
the city boundary, while an exact FAR lattice does not always land there. Edge drags therefore keep
all complete FAR periods and use one shortened final span to the boundary cell. The tool reports
that crossed edge as an outward cardinal direction for the vanilla neighbor-connection routine.

## Art bill

Confirmed in both binaries: Resource Key Type 1 (`0x27812821`) resolves an S3D instance as
`baseInstance + LOD*0x100 + rotation*0x10`. On Windows the core selector is `FUN_00496830`; the
power-pole path is `cSC4PowerPoleOccupant::LoadModel` at `0x0064ad90` through `FUN_00497180`.
`LoadModel` supplies an extra quarter-turn for direction masks `0x1` and `0x8`, which is how one
vanilla straight model serves X/Z and one diagonal model serves both diagonals.

For a FAR ratio, a quarter-turn pairs perpendicular headings but cannot produce their mirror:

- `XP` and `ZN` may reference one logical pole model;
- `XN` and `ZP` may reference a second logical pole model.

The art bill is therefore **2 logical pole models per family per supported ratio**, each with the
normal camera-rotation and LOD S3D resources expected by its RKT1 model. Keep all four FAR keys in
the style table, but allow the perpendicular pairs to alias the same prop instance ID as shown in
the example. One logical model cannot cover both slope signs without reflection or free yaw.

There is an extra mask-order wrinkle: `XP/ZP` classify to mask `0x2` (no engine turn), while
`XN/ZN` classify to mask `0x8` (engine +90 degrees). Which perpendicular pair needs correction is
determined by the source model's authored basis. In-game validation of the P3 wood H-frame tangents
shows that its `XP/ZN` (NR/negative-slope) pair needs the model exemplar's `Power Pole Invert Model
Quarter Turn` property (`0xB22A000E`) set to true; its `XN/ZP` (NL/positive-slope) pair must leave it
absent. The `LoadModel` call-site hook XORs the engine's 0/1 choice without changing model resources
or relying on drag-scoped state.

Do not attempt runtime mesh rotation via `SetTransform`: `GetModelInstanceID`'s variant selection
and the baked lighting/LOD assets are not built for free yaw, and docs §6C documents the
cable-detach problem that approach causes.
