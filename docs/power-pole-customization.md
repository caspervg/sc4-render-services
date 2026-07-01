# Power-pole customization

`SC4PowerPoleCustomizationSample.dll` lets a power-pole prop exemplar describe its wire attachment
points and wire appearance. A prop with none of these properties uses the vanilla path unchanged.
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

**Texture ID safety warning**: unlike the half-extent, these are not validated against loaded
textures. An ID that doesn't resolve to a real texture in the game's cache walks a hash-bucket chain
to a null entry and gets dereferenced with no null check -- this crashes the game, it does not
render transparent. If you want no visible foundation, set the half-extent to `0` instead of trying
to point the texture IDs at nothing.

## Property definitions

The definitions are included in `.agents/new_properties.xml`. `Count="-3"` on attachment properties
enforces repeated XYZ groups; `Count="-1"` permits variable-length per-wire arrays. The selected
`0xB22A0000` through `0xB22A0009` IDs are unique in that vendored registry; the remainder of the
`0xB22A0000` through `0xB22A000F` block was also checked and is currently unused there.

The production exemplar reader, arbitrary 1-N strand rebuild, sag scaling/capping, and per-wire
width lookup are implemented. They still require an in-game validation pass before this sample
should be treated as release-ready. The temporary `polelinetest` cheat remains available for
isolated geometry testing on newly placed or reloaded poles.
