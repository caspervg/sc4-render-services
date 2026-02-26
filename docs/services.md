# Service Reference

This document covers the three services registered by `SC4RenderServices.dll`: ImGui, S3D Camera, and Draw. All three are gated to SimCity 4 version `1.1.641`. If the version check fails, the service is not registered and `cIGZFrameWork::GetSystemService` will fail.

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
- Initialization happens after the DirectX 7 driver and the game window are available.
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
- `GetTextureID` returns a `IDirectDrawSurface7*` as `void*` or `nullptr` if the device is lost or the handle is stale.
- `ReleaseTexture` frees the underlying surface and removes the handle.
- `IsTextureValid` returns false if the handle is stale or the device is lost.
- Texture calls must be made on the render thread.
- Device resets increment `GetDeviceGeneration`. Handles with older generations are invalid.

Fonts:
- `RegisterFont` requires ImGui to be initialized and a unique font ID.
- Fonts are stored inside ImGui; the service only tracks IDs and pointers.
- Register/unregister fonts on the render thread.

DX7 access:
- `AcquireD3DInterfaces` returns AddRef'd `IDirect3DDevice7` and `IDirectDraw7` pointers.
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

Larger examples:
- `../src/sample/DrawServiceSampleDirector.cpp`
- `../src/sample/road-decal/RoadDecalSampleDirector.cpp`
