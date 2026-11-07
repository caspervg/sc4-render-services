# Terrain Paint

Terrain paint covers an area of any shape on the ground with a texture or a colour: a
grass field with a soft edge, an asphalt car park with rounded corners, a white road marking.
It is part of the terrain decal service (`docs/terrain-decals.md`) and needs the same setup:
SimCity 4 `1.1.641`, `EnableTerrainDecalService=true` and
`EnableCustomTerrainDecalRenderer=true`.

A paint is a **shape** and a **style**:

- The shape is one or more polygon rings in the ground plane (x and z in metres). The area is
  everything inside an odd number of rings, so a ring inside another one is a hole.
- The style says how the area looks: up to eight textures, their size, turn and offset, a colour,
  an opacity, a soft edge, and the draw order.

The service drapes the paint over the terrain and follows later terrain changes. It draws the
paint like a decal, in the game's decal pass under the shadows, including the shadow recovery
pass. It also stores the paint in the city save.

## Getting the interface

```cpp
#include "public/TerrainDecalServiceIds.h"
#include "public/cIGZTerrainPaintService.h"

cIGZTerrainPaintService* paints = nullptr;
fw->GetSystemService(kTerrainDecalServiceID, GZIID_cIGZTerrainPaintService,
                     reinterpret_cast<void**>(&paints));
// ... use it, then paints->Release();
```

The call fails with an older `SC4RenderServices.dll`. That is how a plugin can tell whether terrain
paint is available.

## Creating a paint

```cpp
const TerrainPaintPoint points[] = {{100, 100}, {180, 100}, {180, 150}, {100, 150}};
const uint32_t ringSizes[] = {4};
const TerrainPaintShape shape{points, ringSizes, 1};

TerrainPaintStyle style{};
style.textures[0].key = cGZPersistResourceKey{0x7AB50E44, 0x0986135E, 0x00001000};
style.textureCount = 1;
style.featherMeters = 2.0f;   // fade over the outer 2 m
style.drawOrder = 10;         // above decals with a lower order

TerrainPaintId id{};
paints->CreatePaint(shape, &style, sizeof(style), &id);
```

Later calls take the id:
- `SetPaintShape` replaces the shape.
- `SetPaintStyle` replaces the style.
- `GetPaintStyle` reads the style back.
- `RemovePaint` deletes the paint.

Style calls take the struct size, so the style can grow by appending fields in a later build.

### Style fields

| Field | Meaning |
|---|---|
| `textures`, `textureCount` | 1 to 8 terrain textures. With more than one, every 16 m terrain cell of the area takes one of them, chosen by `weight`. The same `variationSeed` always picks the same textures. |
| `textureSizeMeters` | The texture repeats every this many metres. Default 16, like the game's tiles. |
| `textureRotationTurns`, `textureOffsetX/Z` | Turn (1 = a full turn) and shift of the texture. Both are fixed to the ground, so neighbouring paints line up. |
| `color` | Multiplies the texture, the way decal colours do. For a plain colour, use a white texture. |
| `opacity` | 0 to 1. |
| `featherMeters` | Width of the soft edge, inside the outline. 0 gives a hard edge. |
| `drawOrder` | Paints and decals draw in ascending order. Game decals count as 0. When the order is equal, game decals come first, then the rest in the order they were made. |
| `depthOffset` | As for decals; -1 uses the service default. |
| `enabled` | Hidden while false. It is still saved. |
| `flags` | `kTerrainPaintFlagTransient`: the paint is not saved. Use it for previews. |

## Draw order

Decals have a `drawOrder` too (`TerrainDecalState::drawOrder`). Paints and decals made through the
service sort together, so a texture decal can sit on top of a paint and the other way round. Two
conditions apply:
- They must share an overlay manager. Paints use `DynamicLand`, the decal default.
- The custom renderer must be on.

## How it is drawn

Each paint has one game overlay per texture. The overlay covers the paint's 16 m cells, and the
game culls it and orders it like any other decal. When the game asks for the overlay's terrain,
the renderer does not draw the overlay's square. It draws the paint instead:
- The area is cut per terrain cell, and split along the same diagonal as the terrain's two
  triangles, using [Clipper2](https://github.com/AngusJohnson/Clipper2).
- The pieces are triangulated with [earcut](https://github.com/mapbox/earcut.hpp).
- Each vertex takes its height and colour from the terrain.
- The soft edge is the vertex alpha: the distance to the outline divided by the feather width.
- Worn paint (`wear`, `wearPatchMeters`, `wearSeed` in the style) is cut out of each cell when the
  cell is first drawn: rough patches placed at random where the cell has paint, the same every time
  for the same seed. Their edges are hard, and the soft edge is still measured from the outline
  alone, so wear costs only what is on screen.

What a frame costs:
- Only cells the terrain view puts on screen are drawn, the same cells the game's own decal
  drawing uses.
- The cut is made once per cell row, the first time the row is on screen. The renderer keeps each
  piece with its weights on the terrain triangle, which do not depend on the heights.
- Each frame then reads the four corners of each visible cell and blends them. Heights and
  colours are always current, so terrain edits and lighting changes show up at once, with nothing
  to go stale.
- The shadow recovery pass draws again what the decal pass laid in the same frame.
- Changing a paint's shape or look moves and recolours its overlays. New overlays are made only
  when its textures change.
- The soft-edge bands are worked out per cell, from the part of the area near it, when the cell is
  first drawn. After a change of shape, a cell whose surroundings did not change keeps its cut, so
  dragging the end of a long line only cuts the cells near the end again.

To use the vertex alpha, the renderer changes two things for the paint's draw only:
- It takes the diffuse colour from the vertices instead of the material.
- It sets the texture to repeat.

The opacity goes into the vertex alpha as well.

## Textures that follow a path

By default the textures lie flat on the ground. A shape can carry a UV map instead
(`TerrainPaintShape::uvMap`): triangles whose corners give the texture coordinates at that point,
for example a strip along a track with u running along it and v across it. The renderer then:
- Cuts each piece of the area along the map's triangles, so the coordinates follow the triangles
  exactly. Parts of the area outside the map are left out.
- Puts the coordinates in the vertices, and has the decal stage read them through
  `SC4DrawContext::ClearTexTransform` (0x007D4420). After the draw it puts the decal's transform
  back with `SetTexTransform4`. Both go through the game's draw context, so its state cache stays
  right and every graphics driver gets the same calls.

The soft edge still comes from the area's outline. The style's texture size, turn and offset do not
apply. The map is saved with the paint (record minor version 1).

## Saving

Paints are saved in their own record next to the decals, type `0xE5C2B9A8`, group `TDCS`,
instance `2`. A service that does not know paints still loads the city's decals and leaves this
record alone.

The record format is defined by `src/service/decal/TerrainPaintCodec.h`. Every paint carries its
byte length, so a later minor version can add fields that an older reader skips.

## Limits

- The renderer needs the texture transform the game sets for decals; paints are not drawn on the
  water overlay managers.
- One paint holds at most about a million points.
- Paints in a city saved with a newer service draw nothing with an older one. They are kept in the
  save only until that older service saves the city.
