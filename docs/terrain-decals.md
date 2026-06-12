# Terrain Decal Service

This document covers the terrain decal functionality provided by
`SC4RenderServices.dll` and the public API exposed through
`cIGZTerrainDecalService`.

The service is currently gated to SimCity 4 version `1.1.641`. On unsupported
versions it is not registered, and `cIGZFrameWork::GetSystemService(...)` will
fail.

Relevant ini settings:
- `EnableTerrainDecalService=true`
- `EnableCustomTerrainDecalRenderer=true`
- `TerrainDecalCustomDefaultDepthOffset=2` (vanilla decals use `2`; shadows use `3`)
- `TerrainDecalShadowRecoveryOpacityScale=0.25` (post-shadow recovery redraw opacity; lower blends more softly)

## What It Adds

The terrain decal service adds a managed layer on top of SimCity 4 terrain
overlay decals.

Functionality:
- Create decals at runtime from a texture resource key and decal placement data.
- Target any of the game overlay managers:
  `StaticLand`, `StaticWater`, `DynamicLand`, or `DynamicWater`.
- Update and remove decals by a stable service-side ID instead of directly
  tracking raw overlay IDs.
- Persist managed decals into the city save and restore them on load.
- Apply optional UV sub-rectangle overrides for atlas-style textures.
- Support two UV behaviors:
  `StretchSubrect` stretches the chosen UV window across the whole decal.
  `ClipSubrect` clips rendering to the chosen UV window while keeping the base
  UV transform behavior.

In practice this lets a plugin treat terrain decals as game objects with a
small CRUD-style API rather than manually managing overlay-manager state,
save/load hooks, and renderer hook state.

## Public API

IDs and interface:
- Service ID: `kTerrainDecalServiceID` in `src/public/TerrainDecalServiceIds.h`
- Interface ID: `GZIID_cIGZTerrainDecalService` in
  `src/public/TerrainDecalServiceIds.h`
- Interface header: `src/public/cIGZTerrainDecalService.h`

Core types:
- `TerrainDecalId`: service-managed identifier for one decal.
- `TerrainDecalState`: full editable state for a decal.
- `TerrainDecalSnapshot`: `{ id, state }` pair used by `GetDecal` and
  `CopyDecals`.
- `TerrainDecalUvWindow`: optional UV sub-rectangle plus `TerrainDecalUvMode`.

Interface methods:
- `GetStateSize()`: returns the service's `TerrainDecalState` size.
- `GetSnapshotSize()`: returns the service's `TerrainDecalSnapshot` size.
- `CreateDecal(...)`: creates a decal in the active city and returns a new ID.
- `RemoveDecal(...)`: removes the decal and its runtime overlay.
- `GetDecal(...)`: fetches one snapshot by ID.
- `ReplaceDecal(...)`: replaces the state of an existing decal.
- `GetDecalCount()`: returns the current managed decal count.
- `CopyDecals(...)`: copies snapshots into a caller-provided buffer.

## Access Pattern

Acquire the service exactly like the other render services. The returned
interface is AddRef'd and must be `Release()`d by the caller.

```cpp
#include "public/TerrainDecalServiceIds.h"
#include "public/cIGZTerrainDecalService.h"

cIGZTerrainDecalService* terrainDecalService = nullptr;
if (fw->GetSystemService(kTerrainDecalServiceID,
                         GZIID_cIGZTerrainDecalService,
                         reinterpret_cast<void**>(&terrainDecalService))) {
    // use service
    terrainDecalService->Release();
}
```

## Creating a Decal

The main input type is `TerrainDecalState`.

Important fields:
- `textureKey`: `type/group/instance` for the decal texture resource.
- `overlayType`: which terrain overlay manager should own the decal.
- `decalInfo.center`: decal position in world X/Z space.
- `decalInfo.baseSize`: base size of the decal footprint.
- `decalInfo.rotationTurns`: rotation in turns, not degrees.
- `decalInfo.aspectMultiplier`, `uvScaleU`, `uvScaleV`, `uvOffset`: extra
  decal transform controls forwarded to the renderer.
- `opacity`, `enabled`, `color`, `drawMode`: common runtime properties.
- `flags` bit `kTerrainDecalFlagMirror` (`0x80000000`): mirrors the texture in
  place (`u' = 1 - u`, the SC4 lot-texture orientation convention). Combined
  with `rotationTurns` this reaches every mirrored orientation. Requires the
  custom clipped renderer.
- `hasUvWindow` + `uvWindow`: optional atlas/subrect override.

Minimal example:

```cpp
TerrainDecalState state{};
state.textureKey = cGZPersistResourceKey{
    0x7AB50E44, 0x1ABE787D, 0xAA40173A
};
state.overlayType = cISTETerrainView::tOverlayManagerType::DynamicLand;
state.decalInfo.center = cS3DVector2{512.0f, 512.0f};
state.decalInfo.baseSize = 16.0f;
state.decalInfo.rotationTurns = 0.0f;
state.decalInfo.aspectMultiplier = 1.0f;
state.decalInfo.uvScaleU = 1.0f;
state.decalInfo.uvScaleV = 1.0f;
state.decalInfo.uvOffset = 0.0f;
state.opacity = 1.0f;
state.enabled = true;
state.color = cS3DVector3(1.0f, 1.0f, 1.0f);

TerrainDecalId id{};
if (!terrainDecalService->CreateDecal(&state, sizeof(state), &id)) {
    // no active city, invalid state, missing overlay manager, or version mismatch
}
```

## Updating and Enumerating Decals

Use `ReplaceDecal(...)` to update an existing decal.

```cpp
TerrainDecalSnapshot snapshot{};
if (terrainDecalService->GetDecal(id, &snapshot, sizeof(snapshot))) {
    snapshot.state.opacity = 0.5f;
    snapshot.state.decalInfo.rotationTurns += 0.125f;
    terrainDecalService->ReplaceDecal(id, &snapshot.state, sizeof(snapshot.state));
}
```

Use `GetDecalCount()` plus `CopyDecals(...)` to enumerate the current managed
set.

```cpp
const uint32_t count = terrainDecalService->GetDecalCount();
std::vector<TerrainDecalSnapshot> decals(count);
const uint32_t copied = terrainDecalService->CopyDecals(
    decals.data(),
    count,
    sizeof(TerrainDecalSnapshot));
decals.resize(copied);
```

## Persistence Model

Managed terrain decals are persisted into the city save as a sidecar record.

Behavior:
- On save, the service serializes the current managed snapshot list.
- On load, the service reads the sidecar back into memory.
- After the city finishes initializing, the service recreates runtime overlays
  from the loaded snapshot list.

This means plugin authors do not need to implement their own save/load path for
terrain decals if `cIGZTerrainDecalService` is the source of truth.

## UV Window Support

`TerrainDecalUvWindow` lets a decal use a sub-rectangle of the source texture.

Fields:
- `u1`, `v1`, `u2`, `v2`: normalized UV bounds.
- `mode = TerrainDecalUvMode::StretchSubrect`:
  the chosen sub-rect is stretched across the full decal.
- `mode = TerrainDecalUvMode::ClipSubrect`:
  the renderer clips to the chosen sub-rect instead.

Example:

```cpp
state.hasUvWindow = true;
state.uvWindow.u1 = 0.0f;
state.uvWindow.v1 = 0.0f;
state.uvWindow.u2 = 0.5f;
state.uvWindow.v2 = 0.5f;
state.uvWindow.mode = TerrainDecalUvMode::StretchSubrect;
```

## Current Constraints and Caveats

- The service only works on SimCity 4 `1.1.641`.
- Runtime create/update operations require an active city and a valid target
  overlay manager.
- `baseSize` must be greater than zero.
- UV window bounds must satisfy `u1 <= u2` and `v1 <= v2`.
- `flags` are preserved in snapshots and save data; the mirror bit
  (`kTerrainDecalFlagMirror`) is consumed by the custom renderer, but
  `TerrainDecalState::flags` is not pushed into the game overlay manager
  during create/update.
- The service owns decal persistence only for decals created through this API.
  Existing unmanaged overlays are not automatically imported into the registry.

## Recommended Usage Pattern

- Treat `cIGZTerrainDecalService` as the authoritative store for plugin-managed
  decals.
- Keep your own higher-level metadata keyed by `TerrainDecalId`.
- Use `GetDecal`/`CopyDecals` when rebuilding UI state.
- Use `ReplaceDecal` instead of manipulating overlay managers directly when a
  decal is already under service management.
- Use an ImGui or other editor panel to inspect and tweak `TerrainDecalState`
  in place.

## Sample

See `src/sample/TerrainDecalSampleDirector.cpp` for a complete example:
- acquiring `cIGZTerrainDecalService`
- acquiring `cIGZImGuiService`
- listing decals
- editing `TerrainDecalState`
- creating, duplicating, updating, and removing decals
