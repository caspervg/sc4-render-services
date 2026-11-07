# Service Reference

This document covers the services registered by `SC4RenderServices.dll`:
ImGui, World Draw, S3D Camera, Draw, and Terrain Decal. All services are currently gated to
SimCity 4 version `1.1.641`. If the version check fails, the service is not
registered and `cIGZFrameWork::GetSystemService` will fail.

## Common access pattern

Acquire a service with `cIGZFrameWork::GetSystemService` using the service ID and IID. The returned interface is AddRef'd; callers must `Release()`.

Example:
```cpp
cIGZImGuiService* service = nullptr;
if (fw->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                         reinterpret_cast<void**>(&service))) {
    // use service
    service->Release();
}
```

## ImGui Service

IDs and interface:
- Service ID: `kImGuiServiceID` in `src/public/ImGuiServiceIds.h`
- Interface ID: `GZIID_cIGZImGuiService` in `src/public/ImGuiServiceIds.h`
- Interface header: `src/public/cIGZImGuiService.h`

Threading and lifecycle:
- Initialization happens after the game creates its graphics driver and window. ImGui draws
  through the driver's `cIGZGDriver` interface, so it works with the built-in DirectX driver,
  SCD3D11 and scvk.
- ImGui is the docking branch, built into `SC4RenderServices.dll`. Link it as described in the
  README's Usage section.
- Panel callbacks and `QueueRender` callbacks run on the render thread between `ImGui::NewFrame()` and `ImGui::EndFrame()`.
- `GetContext()` returns the ImGui context pointer or `nullptr` when not ready.

Panel registration and callbacks:
- `RegisterPanel` copies the descriptor; update state through your `data` pointer or unregister and re-register.
- `on_init` runs once after ImGui initializes.
- `on_update` and `on_render` run each frame when `visible` is true.
- `on_visible_changed` runs when `SetPanelVisible` toggles the flag.
- `on_shutdown` runs during service shutdown.
- `on_unregister` runs when you explicitly unregister.
- `on_device_lost` and `on_device_restored` run after a device loss or restore.
- Callbacks are invoked without holding internal locks. If you register/unregister/modify visibility inside a callback, the change applies on the next frame because the render loop uses a snapshot.

Render queue:
- `QueueRender` runs a one-shot callback on the next frame.
- The optional cleanup runs immediately after the callback or during shutdown if still queued.

Texture API:
- `CreateTexture` stores RGBA32 source pixels and returns an `ImGuiTextureHandle`.
- `GetTextureID` returns the driver's texture name as an opaque `void*` for `ImGui::Image()`,
  or `nullptr` if the device is lost or the handle is stale. The same handle can texture world
  drawing through `SC4WorldStyle::texture`.
- `ReleaseTexture` frees the underlying surface and removes the handle.
- `IsTextureValid` returns false if the handle is stale or the device is lost.
- Texture calls must be made on the render thread.
- Device resets increment `GetDeviceGeneration`. Handles with older generations are invalid.
  A reset happens when the driver loses its textures or the game creates a new driver object,
  for example on a video mode change.

Fonts:
- `RegisterFont` requires ImGui to be initialized and a unique font ID.
- Fonts are stored inside ImGui; the service only tracks IDs and pointers.
- Register/unregister fonts on the render thread.

Graphics driver:
- `GetActiveDriverInfo` describes the active driver: its kind (`BuiltInDirectX`,
  `BuiltInOpenGL`, `SCD3D11`, `Scvk` or `Unknown`), class ID, display name and module path.
  It returns false until the game has created its driver.

DX7 access:
- `AcquireD3DInterfaces` returns AddRef'd `IDirect3DDevice7` and `IDirectDraw7` pointers. It
  only succeeds with the built-in DirectX driver.
- Always `Release()` both interfaces when done.
- Prefer acquiring per operation instead of caching across frames.

Usage snippet:
```cpp
static void RenderPanel(void* data) {
    ImGui::Begin("My Panel");
    ImGui::TextUnformatted("Hello from a client DLL.");
    ImGui::End();
}

void RegisterPanel(cIGZFrameWork* fw) {
    cIGZImGuiService* service = nullptr;
    if (!fw->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                              reinterpret_cast<void**>(&service))) {
        return;
    }

    ImGuiPanelDesc desc{};
    desc.id = 0x12345678;
    desc.order = 100;
    desc.visible = true;
    desc.on_render = &RenderPanel;
    service->RegisterPanel(desc);
    service->Release();
}
```

Larger examples:
- `../src/sample/ImGuiSampleDirector.cpp`
- `../src/sample/ImGuiSampleDemoDirector.cpp`
- `../src/sample/ImGuiTextureSample.cpp`
- `../src/sample/OverlayManagerSampleDirector.cpp`

## World Draw Service

IDs and interface:
- Service ID: `kWorldDrawServiceID` in `src/public/cIGZWorldDrawService.h`
- Interface ID: `GZIID_cIGZWorldDrawService` in `src/public/cIGZWorldDrawService.h`
- Interface header: `src/public/cIGZWorldDrawService.h`, helper `src/public/SC4WorldMeshBuilder.h`

Capabilities:
- Layers: triangles and lines kept inside the service and drawn at the end of a draw pass every
  frame. Fill them when your data changes.
- Draw callbacks: run at the end of a pass every frame, for geometry that changes every frame.
- Styles cover blending, depth, depth bias, fog, culling and an optional texture from the ImGui
  service. Lines are either a fixed pixel width facing the camera or a fixed width in metres
  lying flat.
- `PickTerrain` and `GetTerrainHeight` for placing geometry on the ground.

Important behavior:
- Registered together with the draw service, and only when `EnableDrawService=true`.
- Draws through `cIGZGDriver`, so it works with every driver. The service restores the game's
  render state after drawing.
- The built-in DirectX driver drops any single draw over 2046 vertices. The service splits
  larger draws for you.

Guide: `world-drawing.md`

Larger examples:
- `../src/sample/WorldDrawSampleDirector.cpp`
- `../src/sample/road-decal/RoadDecalSampleDirector.cpp`

## S3D Camera Service

IDs and interface:
- Service ID: `kS3DCameraServiceID` in `src/public/S3DCameraServiceIds.h`
- Interface ID: `GZIID_cIGZS3DCameraService` in `src/public/S3DCameraServiceIds.h`
- Interface header: `src/public/cIGZS3DCameraService.h`

Camera handles:
- Handles include a version tag to prevent cross-build use.
- `WrapCamera` returns a non-owned handle for a game-managed camera.
- `WrapActiveRendererCamera` returns the active renderer camera if available.
- `CreateCamera` is not supported by the built-in service and returns a null handle.
- `DestroyCamera` is a no-op for wrapped handles.

Projection helpers:
- `Project`, `UnProject`, and `WorldToScreen` are thin wrappers around the game camera.
- Functions return false or null when the handle is invalid or the camera is unavailable.

Viewport and transforms:
- Viewport, subview, ortho, view volume, and transform helpers map to the underlying `cS3DCamera` methods.
- Call these on the main or render thread only.

Usage snippet:
```cpp
cIGZS3DCameraService* cameraService = nullptr;
if (!fw->GetSystemService(kS3DCameraServiceID, GZIID_cIGZS3DCameraService,
                          reinterpret_cast<void**>(&cameraService))) {
    return;
}

auto handle = cameraService->WrapActiveRendererCamera();
float sx = 0.0f;
float sy = 0.0f;
if (cameraService->WorldToScreen(handle, worldX, worldY, worldZ, sx, sy)) {
    // use screen coordinates
}
cameraService->Release();
```

Larger examples:
- `../src/sample/WorldProjectionSampleDirector.cpp`
- `../src/sample/S3DCameraDebugSampleDirector.cpp`

## Draw Service

IDs and interface:
- Service ID: `kDrawServiceID` in `src/public/cIGZDrawService.h`
- Interface ID: `GZIID_cIGZDrawService` in `src/public/cIGZDrawService.h`
- Interface header: `src/public/cIGZDrawService.h`

Draw context handles:
- `WrapDrawContext` and `WrapActiveRendererDrawContext` return a version-tagged handle.
- Most API calls are no-ops if the handle is invalid or from a different game version.

Render pass callbacks:
- `RegisterDrawPassCallback` installs call-site patches for the requested pass.
- Callbacks are invoked twice per pass: `begin=true` before the game pass and `begin=false` after.
- The callback list is snapshotted at the start of the pass. If you unregister during a callback, you may still receive the `begin=false` call for that pass; the change takes effect on the next pass.
- Keep callbacks short; they run on the render thread.

Render state helpers:
- The remaining methods are thin wrappers around the game render context (materials, textures, fog, lighting, primitives, etc.).
- These functions assume you pass a valid draw context handle.

Usage snippet:
```cpp
static void OnDrawPass(DrawServicePass pass, bool begin, void* userData) {
    if (pass == DrawServicePass::Dynamic && begin) {
        // Inject custom draw setup/work before Dynamic pass.
    }
}

uint32_t token = 0;
if (drawService->RegisterDrawPassCallback(DrawServicePass::Dynamic,
                                          &OnDrawPass, myData, &token)) {
    // Keep token and call UnregisterDrawPassCallback(token) during shutdown.
}
```

For drawing your own geometry, prefer the World Draw Service: it sets and restores the render
state for you and works with every graphics driver.

Larger example:
- `../src/sample/DrawServiceSampleDirector.cpp`

## Terrain Decal Service

IDs and interface:
- Service ID: `kTerrainDecalServiceID` in `src/public/TerrainDecalServiceIds.h`
- Interface ID: `GZIID_cIGZTerrainDecalService` in
  `src/public/TerrainDecalServiceIds.h`
- Interface header: `src/public/cIGZTerrainDecalService.h`

Capabilities:
- Create, update, remove, and enumerate managed terrain decals.
- Persist managed decals into the city save and recreate them on load.
- Target static or dynamic land/water overlay managers.
- Apply optional UV sub-rectangle overrides for atlas-style textures.
- Layer decals and terrain paints by draw order.
- Paint areas of any shape with a texture or a colour and a soft edge, through
  `cIGZTerrainPaintService` (`GZIID_cIGZTerrainPaintService`, same service ID).
  See `docs/terrain-paint.md`.

Important behavior:
- Struct-based terrain decal calls take an explicit struct size so the single
  interface can grow by appending fields in a later build.
- `CreateDecal` and `ReplaceDecal` operate on the active city only.
- `GetDecal` and `CopyDecals` return service-managed snapshots.
- The service is currently intended for plugin-managed decals, not for
  discovering or taking ownership of arbitrary pre-existing terrain overlays.

Minimal usage:
```cpp
cIGZTerrainDecalService* terrainDecalService = nullptr;
if (!fw->GetSystemService(kTerrainDecalServiceID,
                          GZIID_cIGZTerrainDecalService,
                          reinterpret_cast<void**>(&terrainDecalService))) {
    return;
}

TerrainDecalState state{};
state.textureKey = cGZPersistResourceKey{0x7AB50E44, 0x1ABE787D, 0xAA40173A};
state.overlayType = cISTETerrainView::tOverlayManagerType::DynamicLand;
state.decalInfo.center = cS3DVector2{512.0f, 512.0f};
state.decalInfo.baseSize = 16.0f;
state.decalInfo.aspectMultiplier = 1.0f;
state.decalInfo.uvScaleU = 1.0f;
state.decalInfo.uvScaleV = 1.0f;
state.opacity = 1.0f;
state.enabled = true;

TerrainDecalId id{};
terrainDecalService->CreateDecal(&state, sizeof(state), &id);
terrainDecalService->Release();
```

Larger example:
- `../src/sample/TerrainDecalSampleDirector.cpp`

Detailed reference:
- `terrain-decals.md`
