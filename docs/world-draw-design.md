# World drawing without DX7: design

Status: draft for review. Follows step 5 of the generic backend work (ImGui through
`cIGZGDriver`) and extends the same idea to everything plugins draw in the city.

## Goal

Plugins that draw in the world today all reach for the DX7 device through
`AcquireD3DInterfaces`. That only works on the game's built-in DirectX driver. On SCD3D11 and
scvk those overlays simply disappear. This design gives plugins a world drawing API that goes
through the game's own draw context and graphics driver, so the same plugin code works on DX7,
SCD3D11 and scvk.

## Who needs what

Surveyed: Plop and Paint (`PaintOverlay`), Terrain Extensions (`OverlayRenderer`, slope and
contour renderers), the RS samples, the RS terrain decal service, and the upcoming Mac
submenus mod (as a preview of features Windows players will ask for).

| Client | What it draws | State | When |
|---|---|---|---|
| Plop and Paint | grids, shapes, markers, thick lines (crossed quads when vertical) | depth test on, no depth write, alpha blend, depth bias 8, no fog | end of PreDynamic |
| Terrain Extensions | overlays, slope cells, contour lines, labels | depth test off (always on top), alpha blend, depth bias 1, fog on | end of PostDynamic |
| Road decal sample | committed, selected and preview strips on the terrain | depth test LessEqual, no depth write, alpha blend, depth bias 1, fog on | end of PreDynamic |
| DrawService sample | world quad, screen lines, bounding boxes | depth test on, depth bias -16 to 16, screen-space lines | selectable |
| WorldProjection sample | ImGui labels and images placed with `WorldToScreen` | none, plus a DX7 depth readback for hiding labels | ImGui frame |
| Terrain decal service | clipped textured decals | game draw context, depth offset 2 to 4, texture transform | game decal pass |
| Mac mod (v3) | brush previews, grids, footprints, thumbnails at the cursor | ImGui 2D, no depth | after the UI |
| Mac mod (post FX) | colour grading, tilt shift, weather, night lights | shaders, scene copy, depth texture | at present |

What they share:

- Position + colour triangle lists, alpha blended, no culling, no lighting, no depth write.
  This covers almost every world draw.
- Depth test on or off, a small depth bias, optional fog so overlays fade like the scene.
- Thick lines built on the CPU as quads, terrain-height sampling per vertex, and layers that
  are rebuilt only when their content changes. Plop and Paint and Terrain Extensions each wrote
  their own version of all three.
- A clean camera. Today plugins draw with whatever matrices the game left behind. Terrain
  Extensions found that the model matrix can still hold the last object's transform and now
  resets the draw context and camera before every draw.
- Saving and restoring every state they touch (a copy of `D3D7StateGuard` in each repo).

## What the driver can and cannot do

`cIGZGDriver` is a fixed-function, OpenGL-1.x style interface. It covers matrices, blend,
depth, fog, polygon offset, stencil, texture stages and `DrawArrays`/`DrawElements`. It has no
render targets, no pixel or depth readback, no shaders and no blend equation (so no MAX blend).

The game's `SC4DrawContext` sits on top of the driver and keeps its own cache of the state it
has set. Calling the driver directly behind its back leaves that cache stale, which is why the
ImGui renderer has to resync it afterwards. World drawing should go through the draw context
instead, which keeps the cache correct by construction:

- `SC4DrawContext::DrawPrims` at `0x007D2990` takes `(prim, vertexFormat, count, vertices)`
  and is already used by the terrain decal renderer with format `0x0B`.
- Blend, depth test, depth mask, depth func, depth offset, cull, fog, lighting, texture and
  texture transform setters are already wrapped by `cIGZDrawService`.
- `SetDefaultRenderStateUnilaterally` and `SetCamera` give a known starting point.

Only things the draw context cannot express (if any turn up) fall back to direct driver calls,
followed by the same resync the ImGui renderer uses.

## Proposed API

A new interface on the existing draw service, `cIGZWorldDrawService`, with its own interface
ID and an API version so it can grow. Everything that builds geometry lives in a header-only
helper, so the binary interface stays small.

### Draw callbacks

```cpp
struct SC4WorldVertex {
    float x, y, z;     // world space, metres
    uint32_t color;    // 0xAARRGGBB
    float u, v;        // ignored when the batch has no texture
};

enum class SC4WorldBlend : uint8_t { Alpha, Additive, Multiply, Premultiplied, Opaque };

struct SC4WorldDrawState {
    SC4WorldBlend blend = SC4WorldBlend::Alpha;
    bool depthTest = true;      // false draws on top of everything
    bool depthWrite = false;
    int8_t depthBias = 1;       // same scale as the game's depth offset
    bool cullBackFaces = false;
    bool fog = false;           // fade with distance like the scene
    ImGuiTextureHandle texture{};  // zero id means untextured
};

using WorldDrawCallback = void (*)(cIGZWorldDrawContext& ctx, void* userData);

bool RegisterWorldDrawCallback(DrawServicePass pass, WorldDrawCallback cb, void* userData,
                               uint32_t* outToken);
void UnregisterWorldDrawCallback(uint32_t token);
```

RS runs these callbacks at the end of the chosen pass. Before the first callback it resets the
draw context, applies the active camera and an identity model transform. After the last one it
restores the default render state so the game carries on from a known state. Clients pick the
pass explicitly. There is no default, because the choice decides what the overlay hides behind
(end of PreDynamic: behind buildings but not cars, end of PostDynamic: behind everything).

`cIGZWorldDrawContext` is only valid during the callback:

```cpp
void DrawTriangles(const SC4WorldDrawState&, const SC4WorldVertex*, uint32_t count);
void DrawIndexedTriangles(const SC4WorldDrawState&, const SC4WorldVertex*, uint32_t vertexCount,
                          const uint16_t* indices, uint32_t indexCount);
void DrawLines(const SC4WorldDrawState&, const SC4WorldLinePoint*, uint32_t count,
               float width, SC4LineWidthUnit unit);  // Metres or Pixels
```

`DrawLines` expands to quads inside RS. With `Pixels` it uses the current camera to keep a
fixed screen width, which neither client can do today. Vertical segments get the crossed quad
treatment that Plop and Paint uses.

### Retained layers

Both big clients keep geometry that changes rarely and draw it every frame. RS can own that:

```cpp
uint32_t CreateWorldLayer(DrawServicePass pass, const SC4WorldDrawState& state, int32_t order);
bool SetWorldLayerTriangles(uint32_t layer, const SC4WorldVertex*, uint32_t count);
bool SetWorldLayerVisible(uint32_t layer, bool visible);
void DestroyWorldLayer(uint32_t layer);
```

Layers are drawn in `order` within their pass, before that pass's callbacks. This replaces
`PaintOverlay`'s layer list and `OverlayDrawManager` without either plugin keeping a renderer.

### Geometry helpers (header only)

`WorldGeometryBuilder.h` builds `SC4WorldVertex` arrays:

- `AddQuad`, `AddThickLine` (metres), `AddCircle`, `AddFilledPolygon`, `AddMarker`
- terrain draped versions that sample `cISTETerrain::GetAltitude` along the path, with a lift
  above the ground and optional subdivision so long lines follow hills
- colour helpers that turn RGBA floats into the packed vertex colour

These are the routines that Plop and Paint and Terrain Extensions each wrote separately.

### Textures

The ImGui service's managed textures become usable in world draws too, so one handle works
in `ImGui::Image` and in `SC4WorldDrawState::texture`. ImGui service API 6 adds:

- `UpdateTexture(handle, pixels)` through `LoadTextureLevel`. The WorldProjection sample and
  the Mac mod both replace a texture every frame today.
- `ImGuiTextureDesc` options for repeat wrapping and mipmaps, for ground fills and patterns.

### Picking

- `PickTerrain(screenX, screenY, float out[3])` using the camera eye ray and terrain altitude.
  The Mac mod rebuilds a 20 by 12 grid of game pick calls every frame to fake this.
- `GetTerrainHeight(x, z)` as a thin wrapper.
- `IsPointerOverImGui()` on the ImGui service. Detecting the game's own UI windows is left for
  later research.

## Terrain decals

The decal service is already mostly independent of the driver:

- The hooks sit in game code (`cSTEOverlayManager` call sites), not in the driver.
- `ClippedTerrainDecalRenderer` draws with `SC4DrawContext::DrawPrims`, `SetDepthOffset` and
  `SetTexTransform4`, all of which go through the active driver.
- The overlay manager sample only calls `cISTEOverlayManager`.

The one DX7 part is shadow recovery. After the game draws shadows, managed decals are drawn
again so shadows don't hide them completely. There are two ways this runs today:

1. **Composite** (DX7 only): draw all decals into an offscreen target, then blend that image
   over the scene once at 25 percent. Overlapping decals blend with each other first, so
   the result looks like one layer.
2. **Replay**: draw each decal again after the shadows at 25 percent of its own opacity.
   Overlapping decals stack, so overlaps come out slightly darker than in the composite.

When the D3DX object is missing, which is the case on SCD3D11 and scvk since step 4,
`CompositeManagedDecalsAfterShadows_` returns false and the replay runs instead. So decals
should already work on every driver, with the replay look for shadows.

### What the composite actually computes

The offscreen target is cleared to transparent black and the decals are drawn into it with
normal alpha blending. That image is then blended over the scene with its alpha scaled by
`k` (0.25). For a single decal with colour `C` and alpha `a` that gives
`S * (1 - k*a*a) + k * a*a*a * C`. Where decals are opaque (`a = 1`) the result is
`S * (1 - k) + k * C_top`, using only the topmost decal. The replay instead adds one
`k * a` layer per decal, which is why overlaps come out darker.

### Doing it through the game's own driver

The driver interface has no offscreen targets, but every driver also implements two extension
interfaces the game requires, and SCD3D11 has a real stencil buffer (`D24_UNORM_S8_UINT`, with
`StencilFunc`/`StencilOp`/`ClearStencil` implemented):

- `cIGZGSnapshotExtension` (`0xE69BFE2A`): `CopyColorBuffer` reads the colour buffer back to
  the CPU. The game uses it for snapshots and region thumbnails.
- `cIGZGBufferRegionExtension` (`0x669565FE`): saves part of the colour, depth or stencil
  buffer and copies it back later. The game uses it to avoid redrawing the whole scene. The
  copy back is a plain copy, it cannot blend.

Options, best first:

1. **Stencil, one recovery layer per pixel.** Clear stencil, then draw the replay from the
   topmost decal down, with the stencil test passing only where the value is still 0 and
   setting it to 1 on pass. An alpha test at a very low threshold keeps fully transparent
   texels from claiming pixels. Each pixel then gets exactly one recovery layer, from its
   topmost decal, which matches the composite for opaque decals and their overlaps. It runs
   entirely on the GPU and costs one extra stencil clear. The difference that remains is at
   soft, partly transparent decal edges, where the composite's `a*a*a` falloff is weaker
   than the replay's `a`. The replay can match that by drawing with `k * a*a*a` instead of
   `k * a`, computed per decal from its slot opacity. The texture's own alpha still enters
   once, so soft texture edges stay slightly stronger than the composite.
   Needs: the replay geometry buffered so it can be drawn in reverse order (the hook already
   computes it per decal), and stencil state set through the driver, then reset.
2. **Exact composite through a CPU round trip.** Draw the decals at full strength over the
   scene, read the result back with `CopyColorBuffer`, put the saved scene back with a buffer
   region, and draw the readback at 25 percent with `BitBltAlpha` or as a texture. This gives
   exactly the composite on every driver, but it reads a full frame back to the CPU each time
   decals are drawn. That stalls the GPU, so it is a fallback for screenshots at most.
3. **Plain replay** as today, when there is no stencil buffer.

The Ghidra checks (below) settle which driver gets what. The game never uses stencil, so
clearing it is safe. But only SCD3D11 actually has a stencil buffer:

| Driver | Shadow recovery |
|---|---|
| Built-in DX7 | composite, as today (its depth buffer normally has no stencil) |
| SCD3D11 | stencil replay (falls back to plain replay on a device without `D24S8`) |
| scvk 0.3.0 | plain replay (no stencil buffer, stencil calls do nothing) |

The stencil replay goes through the driver on every driver anyway. Where stencil is missing,
it simply turns into the plain replay. If scvk ever adds a stencil buffer, it gets the better
look without any change in RS.

Plan for decals:

- Test the decal sample on SCD3D11 and scvk and confirm the replay path runs there today.
- Prototype the stencil replay behind a setting on SCD3D11 and compare it side by side with
  the DX7 composite.
- Test with SCD3D11's and scvk's own live shadows turned on.
- Log once which shadow recovery method is in use, in plain words, so players know what to
  expect.

### Stencil replay as built

`TerrainDecalHook::ReplayManagedDecalsAfterShadows_`, behind
`TerrainDecalShadowRecoveryUseStencil` (on by default):

- **No reordering.** Instead of drawing the decals from the top down, the game's
  `cSTEOverlayManager::DrawDecals` (`0x00736790`) runs twice in its own order. Each decal gets a
  number from 1 to 255 in draw order. The claim pass draws with colour and depth writes off,
  alpha test `> 0`, and stencil `ALWAYS` + `REPLACE`, so each pixel ends up holding the number
  of the last, topmost decal that covers it with a visible texel. The draw pass draws normally
  with stencil `EQUAL` to the decal's own number. Both passes see the same decals in the same
  order, so the numbers line up. This avoids buffering geometry, textures and texture transforms
  per decal.
- **Per-draw state.** `DrawDecals` applies a packed render state per decal through
  `SC4DrawContext::SetRenderState` (`0x007D4530`), cached at ctx`+0x80`. Bits: `0x1` colour
  write, `0x2` depth write, `0x4` depth test, `0x8` alpha test (forced off when the renderer
  option byte at `[[ctx+0x38]+0xC]+0x11C` is set), `0x10` blend, `0x20` texture, `0x40` cull,
  `0x380` depth func, bits 10-12 alpha func, bits 21-24 alpha reference in fifteenths
  (`0x00ABB4CC` = 1/15), bits 13-20 blend factors. The claim pass changes colour write, depth
  write and alpha test on the driver right before the decal's `DrawPrims` and restores them from
  that cache right after, so the cache stays true. `SetRenderState` itself can't be used for
  this: it ends by resetting texture wrap, filtering and both texture transforms.
- **Stencil support** comes from `cIGZGDriver::GetVideoModeInfo(sGDMode&)`,
  `supportsStencilBuffer` at `+0x21`, cached per driver object. SCD3D11 reports whether its
  device has `D24S8`. scvk has always reported true. Before 0.4.0 its stencil functions were
  empty and its `ColorMask` worked, so there the claim pass drew nothing and the draw pass was
  the plain replay. From 0.4.0 it gets the full method with no change here. The mode
  is read into a padded buffer because the game's own record may be longer than the header's.
- The stencil is cleared with `ClearStencil(0)` + `Clear(0x2000)` before the claim pass, and
  stencil test is disabled again afterwards. The built-in DX7 driver keeps the composite and
  never reaches this path.

## Out of scope

These need render targets, readback or shaders. The fixed-function driver interface offers
none of them:

- Offscreen rendering and compositing, other than the decal composite on DX7.
- Reading back depth or colour. The WorldProjection depth readback only exists to hide labels
  behind the scene, which a depth-tested world draw now does directly. It stays as a DX7-only
  demo.
- Post processing like the Mac mod's colour grading, tilt shift, depth of field and weather.
  This would need an optional extension interface implemented by SCD3D11 and scvk themselves
  (scene colour copy, depth as a texture, offscreen targets, custom shaders). Worth proposing to
  their maintainers later, not something RS can do alone.
- Live 3D model previews and in-game model thumbnails. Both need either offscreen targets or
  drawing game models through the game's own code, which is separate research.
- MAX blending.

`AcquireD3DInterfaces` stays for these DX7-only features and returns false on other drivers,
as it does now.

## Migration

- **Road decal sample**: port first, as the reference client for the callback API.
- **DrawService sample**: port the world quad and screen lines. Keep the bounding box demo,
  which already uses the draw context.
- **WorldProjection sample**: add a depth-tested variant of the overlay through the new API.
  Keep the readback demo marked as DX7 only.
- **Terrain Extensions and Plop and Paint**: not changed by this work, since those repos are
  off limits. Their maintainers can move `OverlayRenderer` and `PaintOverlay` onto layers and the
  geometry helpers. This document and the samples are the migration guide.

## Verification

Each step is checked in game on the built-in DirectX driver, SCD3D11 and scvk:

- the road decal sample: strips sit on the terrain, hide behind hills, fade with fog
- a contour-style layer drawn on top of everything with depth test off
- pixel-width lines keep their width while zooming
- a textured ground quad using an ImGui texture handle
- terrain decals with shadow recovery, comparing composite (DX7) and replay
- nothing else in the scene changes: no stray transforms on buildings, no lost fog, no
  texture left bound on the UI

## Checks done in Ghidra

All in `SimCity 4.exe`. Draw context methods are `__thiscall` on `SC4DrawContext`, whose
driver pointer sits at `+0x30` and whose cached state flags sit at `+0x80`. Driver vtable
offsets follow `cIGZGDriver.h` (`DrawArrays` `0x0C`, `InterleavedArrays` `0x14`, `Fog`
`0x78`/`0x7C`, `Enable` `0xB0`, `Disable` `0xB4`, `TexStage` `0xCC`).

**Vertex format.** `SC4DrawContext::DrawPrims` (`0x007D2990`, `RET 0x10`) is a plain pass
through: `driver->InterleavedArrays(format, 0, vertices)` and then
`driver->DrawArrays(prim, 0, count)`. It applies no state of its own, so any format the
driver accepts works, including `0x0A` (24 bytes, colour at 12, UV at 16), which the ImGui
renderer already validates at start-up. `SC4WorldVertex` is laid out to match it.

**Other draw context wrappers.** Checked against `RET` sizes, every `cIGZDrawService` thunk
matches the game except four, now fixed in `cIGZDrawService`:
- `SetVertexBuffer` (`0x007D2970`) zeroes its second stack argument and jumps to
  `driver->InterleavedArrays(format, 0, vertices)`, so it takes `(format, unused, vertices)`.
- `SetTransparency` (`0x007D2A20`) jumps to `driver->AlphaMultiplier(float)` (slot `0x84`), so
  it takes the opacity as a float.
- `DrawPrimsIndexedRaw` (`0x007D29F0`, `RET 0xC`) is
  `driver->DrawElements(prim, count, 3, indices)` with 16-bit client-memory indices.
- `DrawPrims` had the right argument count but was declared as
  `(prim, startVertex, primitiveCount, flags)`.

The first three were declared with the wrong number of arguments, so calling them through the
old interface unbalanced the stack.

**Batch size on the built-in DX7 driver.** Its `DrawArrays` (`0x008857E0`) copies the vertices
into a dynamic vertex buffer through `cD3DXContext::DrawPrimitive` (`0x00889E00`). That buffer
is 64 KB (`0x10000 / stride`, set at `0x00889910`), and `Reserve` (`0x008899B0`) returns null
for a draw that does not fit, which drops it with no error. The driver stores the vertices in
its own layout, which is larger than our 24 bytes: batches of 2730 still lost their first
batch in game. That path handles layouts of up to 32 bytes, so world draws are split into
batches of at most 2046 vertices (`0x10000 / 32`, rounded down to whole triangles).
SCD3D11 and scvk have no such limit. Found when the larger showcase layers were missing on
DX7 only.

**State goes to the driver at once.** The flag setters write to the driver immediately and
only skip the call when the cached flag already matches:
- `EnableDepthTestFlag` (`0x007D27B0`): `Enable/Disable(1)`, flag `0x4`
- `EnableDepthMaskFlag` (`0x007D2800`): `DepthMask`, flag `0x2`
- `EnableBlendStateFlag` (`0x007D4010`): `Enable/Disable(4)`, flag `0x10`. Enabling is skipped
  when a byte at `[[ctx+0x38]+0x0C]+0xFC` is set, which looks like a global "no blending"
  option.
- `EnableTextureStateFlag` (`0x007D3EF0`): for stage 0, `TexStage(0)` then `Enable/Disable(5)`,
  flag `0x20`. Stage 1 uses flag `0x10000000` and only touches the driver when the
  multitexture capability byte at `[[ctx+0x38]+0x0C]+0x17C` is set.

So "texture off" through the draw context really disables texturing on stage 0, and the
cache stays correct with no resync. Driver state IDs, confirmed against SCD3D11's
`eGDCapability`: 0 alpha test, 1 depth test, 2 stencil test, 3 cull, 4 blend, 5 texture, 6 fog.

**Reset.** `SetDefaultRenderStateUnilaterally` (`0x007D5230`) resets every texture stage
(`TexStage`, `TexStageMatrix`, `TexStageCoord`), invalidates the flag cache and reapplies the
default packed state. That covers colour mask, depth test, depth mask and depth function,
alpha test and alpha function, blend and blend function, cull and texture. It does not touch
fog.

**Fog.** `SetFog(enable, colour, start, end)` (`0x007D32D0`) treats a null colour and
`FLT_MAX` (`0x7F7FFFFF`) for start or end as "keep the current value". Enabling it calls
`Enable(6)` and `Fog(0, 2)` (linear). The 3D renderer's `Draw` (`0x007CB530`) turns fog on at
`0x007CB6CC` with the scene's colour and range (renderer `+0xEC`, `+0x100`, `+0x104`, only
when its flag at `+0xE9` is set) and turns it off at `0x007CB87D`, after the pass hooks. So
inside a pass callback, `SetFog(true, nullptr, FLT_MAX, FLT_MAX)` gives exactly the scene's
fog. The current fog state is cached at `ctx+0xA0`. RS reads it so that `fog = true` follows
the scene: on when the scene has fog, off when the player turned it off. Afterwards RS
restores whatever was there.

**Stencil.**
- The game never turns stencil test on. Scanning every `Enable`/`Disable` slot call for state
  2 finds a single real driver call, `Disable(2)` at `0x0098EAA1`, in the graphics system's
  default state setup. The other matches pass a different number of arguments, so they are
  not driver calls. This scan finds immediate arguments only, but the packed state path uses
  only IDs 0, 1, 3, 4 and 5. scvk's author reached the same conclusion independently ("The
  game never calls its stencil functions").
- Clearing stencil during the decal pass is therefore safe. The clear mask bits are `0x1000`
  depth, `0x2000` stencil, `0x4000` colour.
- **Built-in DX7:** the depth surface takes the first format `EnumZBufferFormats` lists
  (callback `0x0088BDC0` copies it and stops). That is normally 16-bit with no stencil. RS can
  confirm it at run time from the Z surface's pixel format (`DDPF_STENCILBUFFER`). The
  composite stays the method here either way.
- **SCD3D11:** uses `D24_UNORM_S8_UINT` when the device supports it, otherwise `D32_FLOAT`.
  It implements `StencilFunc`, `StencilOp`, `StencilMask` and `ClearStencil` fully, and only
  enables stencil when the buffer has it. Comparison IDs follow OpenGL order (0 never, 1 less,
  2 equal, 3 less-equal, 4 greater, 5 not-equal, 6 greater-equal, 7 always). Stencil op IDs are
  0 keep, 1 replace, 2 increment, 3 decrement, 4 invert.
- **scvk 0.3.0:** picks `D16_UNORM` first (D32 float with `Use32BitDepth`), so in practice it
  has no stencil. Its `StencilFunc`, `StencilOp`, `StencilMask` and `ClearStencil` are empty.
  The stencil replay therefore runs as the plain replay there, with no harm done.

**Live shadows.** SCD3D11 and scvk both have optional shadow-map shadows of their own
(`cGDriver_LiveShadows.cpp`). Shadow recovery should be tested with those turned on as well,
since the game's own shadow pass may then look different or be replaced.
