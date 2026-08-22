# Power-pole customization

`SC4PowerPoleCustomizationSample.dll` lets a power-pole prop exemplar describe its wire attachment
points and wire appearance. A prop with none of these properties retains vanilla geometry and
appearance, apart from the DLL's global water-buoy placement correction described below.
The implementation currently targets SimCity 4 version 1.1.641.

## Recommended authoring model

Put the three simple appearance properties on a cohort shared by one pole family. Put attachment
points on each model's prop exemplar because they depend on that model's crossarm geometry. Most
families only need these scalar defaults:

| ID | Property | Type | Meaning |
|---|---|---|---|
| `0xB22A0004` | Power Pole Wire Width Scale | Float32 | Multiplies vanilla's zoom-dependent width. `1` is vanilla; `0` hides the wires. |
| `0xB22A0005` | Power Pole Wire Sag Scale | Float32 | Multiplies vanilla's span-dependent sag. `1` is vanilla; `0` is straight. |
| `0xB22A0006` | Power Pole Wire Maximum Sag | Float32 | Caps vertical sag after scaling, in meters. |

This keeps the common case small: a family can set width and sag once, while every span still gets
the game's normal length-dependent curve.

## Attachment points

Each direction property is a Float32 array of repeated model-local `X, Y, Z` triples. The DLL infers
the wire count from the array length; do not add a count value.

| ID | Property |
|---|---|
| `0xB22A0000` | Power Pole Wire Attach Points Direction 0 |
| `0xB22A0001` | Power Pole Wire Attach Points Direction 1 |
| `0xB22A0002` | Power Pole Wire Attach Points Direction 2 |
| `0xB22A0003` | Power Pole Wire Attach Points Direction 3 |
| `0xB22A000D` | Power Pole Attach Basis Degrees |

Example with four wires:

```text
-4.5, 12.0, 0.0,
-1.5, 12.0, 0.0,
 1.5, 12.0, 0.0,
 4.5, 12.0, 0.0
```

Direction is the undirected line orientation returned by the game, so both endpoints use the same
direction index. A missing direction falls back to that model's vanilla four attachment points.
When two models expose different counts, the span uses the smaller count. Wire index `0` at one
endpoint connects to index `0` at the other, and so on; keep ordering consistent across a family.
`Power Pole Attach Basis Degrees` records the in-plane heading for which custom points were authored
(normalized modulo 180 degrees); omit it for vanilla-basis points. The DLL rotates each endpoint's
points independently onto an off-angle/FAR span.

The DLL accepts at most `MaxPointsPerDirection` entries from each array. The default is 8 and the
INI permits 1 through 15.

## Optional per-wire overrides

Only define these when individual conductors really differ, such as a thinner shield/ground wire.
They use the same wire-index order as the attachment arrays:

| ID | Property | Type | Fallback for missing entries |
|---|---|---|---|
| `0xB22A0007` | Power Pole Wire Width Scales | Float32 array | Power Pole Wire Width Scale |
| `0xB22A0008` | Power Pole Wire Sag Scales | Float32 array | Power Pole Wire Sag Scale |
| `0xB22A0009` | Power Pole Wire Maximum Sags | Float32 array | Power Pole Wire Maximum Sag |

For example, `0.6, 1, 1, 1` makes only wire 0 thinner. Arrays may be shorter than the wire count;
the remaining wires inherit the scalar family value. All appearance values must be finite and
non-negative.

Appearance belongs to the span but is authored on pole families. For a mixed-family connection,
the active pole that owns the rendered connection supplies appearance; if it has none, the other
endpoint supplies it. Authors should therefore use compatible defaults on families intended to
connect seamlessly.

## Foundation pad (floor + retaining walls)

Every pole draws a flat pad plus two retaining walls under it to hide terrain z-fighting on slopes.
Vanilla hardcodes this to a fixed 10m x 10m pad (5.0 half-extent) and two fixed texture IDs,
regardless of the pole's model or exemplar. These three properties override that:

| ID | Property | Type | Meaning |
|---|---|---|---|
| `0xB22A000A` | Power Pole Foundation Half Extent | Float32 | Pad half-extent in meters. `5.0` matches vanilla. `0` collapses the pad and walls to a degenerate point -- the safe way to hide the foundation entirely, since it needs no texture to resolve. Clamped to 50. |
| `0xB22A000B` | Power Pole Foundation Floor Texture ID | Uint32 | Overrides the floor's texture-cache lookup key (vanilla `0x0912220E`). |
| `0xB22A000C` | Power Pole Foundation Wall Texture ID | Uint32 | Overrides the walls' texture-cache lookup key (vanilla `0x08080004`). |
| `0xB22A0011` | Power Pole Foundation Follow Span | Bool | `true` yaw-rotates the pad (floor and walls) so it follows the pole's span bearing instead of staying axis-aligned. The rotation is the minimal deviation from the nearest 90-degree step, so walls keep their vanilla sides; junction poles whose connections disagree on a bearing stay axis-aligned. Rebuilt live whenever a connection is added, updated, removed, or loaded. Pairs well with FAR poles. |

**Texture ID validation (2026-07-19)**: custom texture IDs are validated and registered at load.
The engine keeps its own power-pole texture registry seeded with only the four vanilla IDs, and
`Draw()` null-derefs on any ID missing from it -- so the DLL now accepts a custom ID only when an
FSH resource with **type `0x7AB50E44`, group `0x1ABE787D`, instance = the texture ID** exists in the
resource manager, then registers the ID with the engine's registry and triggers a binding reload.
An ID that fails validation logs a warning and keeps the vanilla texture (never a crash). Practical
consequence for authors: ship pad textures as FSH in group `0x1ABE787D`. If you want no visible
foundation, set the half-extent to `0`; that path needs no texture at all.

## Wire textures

The wire strands and the coarse water-crossing wires DrawPowerlines renders use two hardcoded
texture IDs; these properties override them per pole:

| ID | Property | Type | Meaning |
|---|---|---|---|
| `0xB22A000F` | Power Line Wire Texture ID | Uint32 | Replaces the strand texture (vanilla `0xAA9DA78E`) on every connection this pole render-owns. |
| `0xB22A0010` | Power Line Water Wire Texture ID | Uint32 | Replaces the water-crossing/coarse wire texture (vanilla `0xC9AF0FCD`) the engine draws where a span crosses water. |

Both are validated and engine-registered exactly like the foundation textures: the ID must exist
as FSH `0x7AB50E44` in group `0x1ABE787D` or the pole keeps the vanilla texture (warning logged,
never a crash). When the two endpoint exemplars disagree, the render-owning pole's properties win;
a pole without wire-texture properties inherits the other endpoint's.

Water-crossing wires are also kept geometrically aligned: whenever the DLL rebuilds a connection's
strands (custom attach points, sag, or an off-nominal FAR bearing), it now rebuilds the engine's
coarse water-crossing polyline list with the same attach points, strand count, yaw, and sag, so
spans over water match their custom geometry instead of reverting to the vanilla 4-wire shape.

### Water-crossing buoys

The engine draws one billboarded ball per point (except the last) of each water-crossing polyline,
sized from the per-zoom wire-width table times a fixed 4.0. Vanilla's uniform layout therefore put
its first ball exactly on a pole's attach node; the DLL now rebuilds every water-crossing span with
a strictly-interior layout -- for B balls they sit at even fractions k/(B+1) of the span, so no
ball ever renders on a pole (this fix applies to all spans, customized or not). Two properties
tune the balls per pole:

| ID | Property | Type | Meaning |
|---|---|---|---|
| `0xB22A0012` | Power Line Water Buoy Size Scale | Float32 | Scales the ball quads (1.0 = vanilla). |
| `0xB22A0013` | Power Line Water Buoy Point Count | Uint32 | Points per water-crossing strand; balls drawn = points - 1, evenly spaced inside the span. Vanilla 4 (= 3 balls at 1/4, 1/2, 3/4). Clamped to 2-17 (1-16 balls). |

Like all appearance properties these live on pole prop exemplars, so different `[PowerPoles.*]`
styles get different buoy density/size simply by routing to different exemplars. The render-owning
pole's exemplar wins; otherwise the far endpoint's applies.

## Model quarter-turn correction

Power-pole RKT1 models normally receive a mask-derived quarter-turn from the game. For some paired
FAR headings the game rotates the wrong member of the pair. The following property toggles that
0/1 decision before camera rotation and model-variant selection are calculated:

| ID | Property | Type | Meaning |
|---|---|---|---|
| `0xB22A000E` | Power Pole Invert Model Quarter Turn | Bool | `true` exchanges the unrotated and quarter-turned model choices. Absent or `false` preserves vanilla behavior. |

This correction is persistent because it is read from the pole's exemplar whenever the model is
loaded, including zoom changes and save-game reloads. It changes only model-variant selection; wire
attachment bearings remain controlled by the attachment properties.

## Switching pole styles in-game (Tab / Shift-Tab)

When one or more `[PowerPoles.<Name>]` styles are loaded, **Tab** cycles the active style forward and
**Shift-Tab** backward while the power-line tool is selected and owns keyboard focus; the change
applies to subsequent placement. A connection extended or merged at an existing pole can restyle
that touched junction to the active family; the switch does not bulk-restyle untouched poles. Tab
is consumed without switching during an active left-button drag so the preview and committed model
cannot disagree. Dialogs and text inputs keep Tab, and with no styles Tab is passed through
untouched. This works with no other mods. See
`docs/power-line-style-ui-design.md` for the hook details.

Because the INI parser normalizes section names to lowercase, a style may set `DisplayName=...`
for preferred overlay/log capitalization. Every nonzero `REG.*`/`FAR-*` instance
is validated at startup as a prop exemplar (type `0x6534284A`) in the vanilla power-pole group
`0x088E1962`; a missing resource is disabled and falls back rather than creating an invalid pole.

An optional on-screen overlay (top-left) shows the active style, the inter-pole distance, the live
drag heading (e.g. `FA-2  26.6 deg`), whether Shift is constraining to regular headings, and which
FAR ratios the active style supports. The overlay requires the ImGui service
(`SC4RenderServices` / `imgui.dll`); when it is not installed the overlay simply does not appear and
everything else — including Tab switching — keeps working. `imgui.dll` is delay-loaded, so this DLL
never hard-depends on it.

## Property definitions

For exemplar-authoring definitions, `Count="-3"` on attachment properties enforces repeated XYZ
groups and `Count="-1"` permits variable-length per-wire arrays. The
`0xB22A0000` through `0xB22A0013` IDs are reserved by this feature (the original conflict check
covered `0xB22A0000`-`0xB22A000F` against the vendored registry; `0xB22A0010`/`0xB22A0011` were
verified absent from the same registry on 2026-07-19, and `0xB22A0012`/`0xB22A0013` are the buoy
properties documented above).

The production exemplar reader, arbitrary 1-N strand rebuild, sag scaling/capping, per-wire width
lookup, FAR dragging/routing, and the Tab/Shift-Tab style switch were all validated in-game
(2026-07-19). Pole lifetime is handled: a destructor hook drops a pole's customization the moment
the engine destroys it, connection-removal hooks re-key surviving spans after vector compaction,
and all remaining city-scoped state is flushed on city shutdown. Saved connections are rehydrated
after city initialization so widths, textures, buoy settings, geometry, and current exemplar data
apply immediately after reload rather than waiting for an incidental connection update.

## Runtime diagnostic cheat

Enter `keepwires` in the cheat box to toggle the experimental zone-change bypass. While enabled,
lot development does not remove or reroute power lines crossing the changed zone. This can preserve
continuity for testing, but buildings or terrain leveling may visually intersect retained poles;
the setting is process-runtime state and is disabled by default.

## Example content

The optional authoring asset
`assets/power-pole-customization/SC4PowerPoleFA2Example.dat`, when included in a checkout or release,
contains two ready-made FA-2 pole prop exemplars in the vanilla power-pole group `0x088E1962`:

| Instance | Exemplar | Model (S3D) |
|---|---|---|
| `0xB07EFA20` | `PowerPole_FA2_P3_NL266` | `5AD0E817-E4586C0B-00030000` (P3 26.6-degree tangent, NL) |
| `0xB07EFA21` | `PowerPole_FA2_P3_NR266` | `5AD0E817-44586CF1-00030000` (mirrored NR twin) |

It is not copied by the current CMake deploy target; install it manually if present. The exemplars
require the P3 model pack (`MTO_P3_ModelsVol01`) for the meshes. The matching (commented)
`[PowerPoles.FAR]` preset in `SC4PowerPoleCustomization.ini` routes `FAR-2.XP/ZN` to the NL prop
and `FAR-2.XN/ZP` to the NR prop; if a heading renders 90 degrees off in-game, swap the pairs or
set `0xB22A000E` on one prop.
