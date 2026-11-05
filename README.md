# sc4-render-services

Render services and samples for SimCity 4 DLL plugins built on the gzcom-dll SDK: ImGui UI,
drawing into the city, the S3D camera, draw pass hooks and terrain decals.

Everything is drawn through the game's own graphics driver interface (`cIGZGDriver`), so
plugins work the same whichever driver the game uses: its built-in DirectX driver, SCD3D11 or
scvk.

Simtropolis file download page: https://community.simtropolis.com/files/file/37423-sc4renderservices-dll/

![Screenshot](docs/assets/preview.jpg)

## Renderer support

| Feature | Built-in DirectX | SCD3D11 | scvk |
|---|---|---|---|
| ImGui panels, docking, textures | yes | yes | yes |
| World drawing (`cIGZWorldDrawService`) | yes | yes | yes |
| Draw pass callbacks and draw context helpers | yes | yes | yes |
| S3D camera service | yes | yes | yes |
| Terrain decal service | yes | yes | yes |
| Terrain decal shadow recovery | yes | yes (stencil) | yes (stencil from 0.4.0) |
| Raw DirectX 7 access (`AcquireD3DInterfaces`) | yes | no | no |

`cIGZImGuiService::GetActiveDriverInfo` tells a plugin which driver is active.

## Installation

1. Copy `SC4RenderServices.dll` and `SC4RenderServices.ini` into your **Plugins** folder
   (e.g. `Documents\SimCity 4\Plugins\`).
2. (Optional) Copy any sample DLLs you want into the same **Plugins** folder.
3. Edit `SC4RenderServices.ini` to customise logging, appearance, and which services are
   loaded. See [Configuration](#configuration) below.

Older versions also installed `imgui.dll` in the game's **Apps** folder. ImGui is now built into
`SC4RenderServices.dll`, so that file is no longer used. Delete it once every plugin you use
that depends on SC4RenderServices has been updated: plugins built for the old version still
need it to load, and they get no ImGui service from this version either way.

The services require SimCity 4 version `1.1.641`.

## Configuration

`SC4RenderServices.ini` must sit next to `SC4RenderServices.dll` in the Plugins folder. All
settings are optional. If the file is missing or a key is absent, the default value is used.

```ini
[SC4RenderServices]
; Logging verbosity.
; Valid values: trace, debug, info, warn, error, critical, off
LogLevel=info

; Write logs to file. Set to false for MSVC debug output only.
LogToFile=true

; ImGui base font size in pixels. Valid range: 8.0 - 32.0
FontSize=13.0

; Custom .ttf font file path (relative to DLL folder).
; Leave empty to use the built-in ProggyVector font.
FontFile=

; Font oversampling level (1-3). Higher = crisper text, more memory.
FontOversample=2

; ImGui color theme. Valid values: dark, light, classic
Theme=dark

; Enable ImGui keyboard navigation.
KeyboardNav=true

; Global UI scale factor. Valid range: 0.5 - 3.0
UIScale=1.0

; File that stores ImGui window positions and the docking layout (relative to DLL folder).
; Leave empty to not save the layout between sessions.
LayoutFile=SC4RenderServices_layout.ini

; Show a built-in status panel and the Dear ImGui demo window on startup.
; Useful for verifying that the service installed correctly.
ShowDemoPanel=false

; Enable or disable individual services.
; The world draw service is part of the draw service and follows EnableDrawService.
EnableImGuiService=true
EnableS3DCameraService=true
EnableDrawService=true
EnableTerrainDecalService=true

; Enables the custom terrain decal renderer path used to improve vanilla
; terrain decal rendering with UV subrect support, clipping, and depth offsets.
EnableCustomTerrainDecalRenderer=true

; Default depth offset applied to custom-rendered terrain decals.
; Vanilla terrain decals use 2. Shadows use 3. Valid range: 0 - 64.
TerrainDecalCustomDefaultDepthOffset=2

; Opacity scale for the post-shadow terrain decal recovery redraw.
; Lower values blend more softly with shadows. 0 turns the redraw off. Valid range: 0.0 - 1.0.
TerrainDecalShadowRecoveryOpacityScale=0.25

; Draws the shadow recovery redraw once per pixel, from the topmost decal, using the
; stencil buffer, so overlapping decals do not come out darker. Used with graphics drivers
; that have a stencil buffer, such as SCD3D11 and scvk 0.4.0 or later. The game's built-in
; DirectX driver keeps its own method. Set to false to draw every decal again instead.
TerrainDecalShadowRecoveryUseStencil=true
```

## Build

Prereqs:
- CMake 4.0+
- Visual Studio 2022 (Win32 toolchain, SimCity 4 only loads 32-bit DLLs)
- Submodules: `git submodule update --init --recursive`

With the presets:
```
cmake --preset vs2022-win32-release
cmake --build --preset vs2022-win32-release-all
```

Or by hand:
```
cmake -S . -B cmake-build-debug-visual-studio -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug-visual-studio --config Debug
```

Pass `-DCMAKE_BUILD_TYPE=Debug` for a Debug build: it picks the matching debug C runtime.
Only `SC4RenderServices.dll` is built by default. Add `-DSC4RS_BUILD_SAMPLES=ON` to build the
sample DLLs too.
When the build finds SimCity 4 it copies the DLLs into your Plugins folder. Set the `CI`
environment variable to turn that off.

### As a dependency

A plugin can add this repository with `add_subdirectory`. Pass `EXCLUDE_FROM_ALL` so only what
the plugin links gets built, and link the exported ImGui with `sc4rs_use_imgui`:
```cmake
add_subdirectory(vendor/sc4-render-services EXCLUDE_FROM_ALL)
sc4rs_use_imgui(MyPlugin)
```
As a subproject it leaves the generator platform to the parent, never builds the samples, and
does not deploy to the Plugins folder. It reuses the parent's `spdlog` or `spdlog::spdlog` target
and its `gzcom2` or `gzcom_dll` target when those exist, so that gzcom-dll must be at least as new
as the one in `vendor/gzcom-dll`.

## Outputs

Required (deployed to `...\Documents\SimCity 4\Plugins\`):
- `SC4RenderServices.dll`
- `SC4RenderServices.ini`

Optional samples (built with `-DSC4RS_BUILD_SAMPLES=ON`, deployed to `...\Documents\SimCity 4\Plugins\`):
- `SC4ImGuiSample.dll` (basic panel)
- `SC4ImGuiSampleCity.dll` (city-view only)
- `SC4ImGuiSampleDemo.dll` (ImGui demo window)
- `SC4ImGuiTextureSample.dll` (texture management example)
- `SC4WorldDrawSample.dll` (every world drawing feature)
- `SC4RoadDecalSample.dll` (road marking tool on world draw layers)
- `SC4WorldProjectionSample.dll` (world projection helpers, depth readback on the built-in
  DirectX driver only)
- `SC4S3DCameraDebugSample.dll` (camera debug tooling)
- `SC4OverlayManagerSample.dll` (overlay manager example)
- `SC4DrawServiceSample.dll` (draw service UI + hooks)
- `SC4TerrainDecalSample.dll` (terrain decal editor and API sample)
- `SC4CameraViewInputSample.dll` (camera view input control)
- `SC4DateJumperSample.dll` (date jumper panel)

## Provided Services

`SC4RenderServices.dll` registers five services, all gated on SimCity 4 version `1.1.641`.

Detailed reference: [docs/services.md](docs/services.md)

Guides:
- World drawing: [docs/world-drawing.md](docs/world-drawing.md)
- Terrain decals: [docs/terrain-decals.md](docs/terrain-decals.md)
- Updating a plugin from the `imgui.dll` version: [docs/migrating.md](docs/migrating.md)

### 1) ImGui service

- Service ID / IID: `kImGuiServiceID`, `GZIID_cIGZImGuiService`
  (`src/public/ImGuiServiceIds.h`)
- Interface: `cIGZImGuiService` (`src/public/cIGZImGuiService.h`)
- Provides panel registration, per-frame callbacks, font registration, managed textures for
  `ImGui::Image()`, and which graphics driver is active. ImGui is the docking branch.
- Examples: `SC4ImGuiSample.dll`, `SC4ImGuiSampleDemo.dll`,
  `SC4ImGuiTextureSample.dll`, `SC4ImGuiSampleCity.dll`

### 2) World draw service

- Service ID / IID: `kWorldDrawServiceID`, `GZIID_cIGZWorldDrawService`
  (`src/public/cIGZWorldDrawService.h`)
- Interface: `cIGZWorldDrawService`, plus the header-only `SC4WorldMeshBuilder`
  (`src/public/SC4WorldMeshBuilder.h`)
- Draws shapes, lines and textures into the 3D city: layers that SC4RenderServices draws every
  frame, and per-frame draw callbacks. Also picks terrain under the mouse.
- Examples: `SC4WorldDrawSample.dll`, `SC4RoadDecalSample.dll`

### 3) S3D camera service

- Service ID / IID: `kS3DCameraServiceID`, `GZIID_cIGZS3DCameraService`
  (`src/public/S3DCameraServiceIds.h`)
- Interface: `cIGZS3DCameraService` (`src/public/cIGZS3DCameraService.h`)
- Provides camera creation/wrapping, world/screen projection helpers, viewport
  and transform operations.
- Examples: `SC4WorldProjectionSample.dll`, `SC4S3DCameraDebugSample.dll`

### 4) Draw service

- Service ID / IID: `kDrawServiceID`, `GZIID_cIGZDrawService`
  (`src/public/cIGZDrawService.h`)
- Interface: `cIGZDrawService` (`src/public/cIGZDrawService.h`)
- Provides draw-context wrapping, render-pass callbacks
  (`PreStatic`...`PostDynamic`), and helpers for render state, mesh/model draw
  calls, and primitive drawing. For drawing your own shapes, the world draw service is
  simpler and handles the render state for you.
- Example: `SC4DrawServiceSample.dll`

### 5) Terrain decal service

- Service ID / IID: `kTerrainDecalServiceID`,
  `GZIID_cIGZTerrainDecalService`
  (`src/public/TerrainDecalServiceIds.h`)
- Interface: `cIGZTerrainDecalService`
  (`src/public/cIGZTerrainDecalService.h`)
- Provides managed create/update/remove/enumerate APIs for terrain decals,
  save/load persistence for managed decals, and optional UV sub-rectangle
  overrides.
- Example: `SC4TerrainDecalSample.dll`

## Usage

- Query a service via `cIGZFrameWork::GetSystemService` with its service ID and IID. The
  returned interface is AddRef'd, so `Release()` it when done.
- To use ImGui, include `vendor/imgui/imgui.h` and link the ImGui that
  `SC4RenderServices.dll` exports. In this repository, `sc4rs_use_imgui(target)` in
  `CMakeLists.txt` does everything. A plugin in its own repository needs the same four
  things:
  - define `IMGUI_USER_CONFIG="ImGuiUserConfig.h"` and `IMGUI_DLL_IMPORT`, with `src/public`
    on the include path
  - link `SC4RenderServices.lib` and `delayimp.lib`
  - link with `/DELAYLOAD:SC4RenderServices.dll`
  - compile `src/public/SC4RenderServicesDelayLoad.cpp` into the plugin

  Delay loading makes the plugin independent of the order the game loads DLLs in. Only call
  ImGui once you hold the ImGui service.
- Register a panel with `ImGuiPanelDesc` and render using `ImGui::*`.
- The service owns the ImGui context. Do not call `ImGui::CreateContext()` or
  `ImGui::DestroyContext()` in clients.

## Render Callbacks

Four callback models are available:

- `cIGZImGuiService` panel callbacks (`on_init`, `on_update`, `on_render`,
  `on_visible_changed`, `on_shutdown`, `on_unregister`, `on_device_lost`,
  `on_device_restored`) for panel lifecycle and per-frame UI drawing.
- `cIGZImGuiService::QueueRender(...)` for one-shot work on the next ImGui
  frame. The optional cleanup callback runs after execution (or during
  shutdown if still queued).
- `cIGZWorldDrawService::RegisterDrawCallback(...)` to draw into the city at the end of a
  pass every frame. See [docs/world-drawing.md](docs/world-drawing.md).
- `cIGZDrawService::RegisterDrawPassCallback(...)` for raw renderer pass hooks.
  Callback signature is `void(DrawServicePass pass, bool begin, void* userData)`.
  The callback is invoked twice per pass: before the game pass (`begin=true`)
  and after it (`begin=false`).

Minimal draw-pass callback pattern:

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

## Public API

Client-facing headers live under `src/public`:

- `cIGZImGuiService.h` (service interface + `ImGuiPanelDesc`) and `ImGuiServiceIds.h`
- `ImGuiPanel.h` and `ImGuiPanelAdapter.h` (optional class-style adapter)
- `ImGuiTexture.h` (RAII texture wrapper)
- `GraphicsDriverInfo.h` (which graphics driver is active)
- `cIGZWorldDrawService.h` and `SC4WorldMeshBuilder.h` (world drawing)
- `cIGZDrawService.h`, `cIGZS3DCameraService.h`, `cIGZTerrainDecalService.h` and their ID
  headers

Class-based usage (optional):
```cpp
class MyPanel final : public ImGuiPanel {
public:
    void OnInit() override {}
    void OnRender() override {
        ImGui::Begin("Class Panel");
        ImGui::TextUnformatted("Hello from a class-based panel.");
        ImGui::End();
    }
};

void RegisterPanel(cIGZFrameWork* fw) {
    cIGZImGuiService* service = nullptr;
    if (!fw->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                              reinterpret_cast<void**>(&service))) {
        return;
    }

    auto* panel = new MyPanel();
    ImGuiPanelDesc desc = ImGuiPanelAdapter<MyPanel>::MakeDesc(panel, 0x12345678, 100, true);
    if (!service->RegisterPanel(desc)) {
        delete panel;
    }
    service->Release();
}
```

Example (minimal):
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
    desc.on_shutdown = nullptr;
    desc.data = nullptr;
    service->RegisterPanel(desc);
    service->Release();
}
```

### Safe Texture Management

The service creates textures through the active graphics driver and keeps a copy of the pixels,
so it can recreate them when the driver loses them. This is the **recommended** way to create
textures for `ImGui::Image()` and for textured world drawing.

#### Features

- **Device loss handling**: textures are recreated after Alt+Tab, a resolution change or a new
  driver object
- **Generation tracking**: old texture handles are invalidated after a device reset
- **Memory safety**: source pixel data is stored for automatic recreation
- **RAII wrapper**: optional `ImGuiTexture` class for automatic lifetime management

#### Basic Usage (RAII Wrapper - Recommended)

```cpp
#include "public/ImGuiTexture.h"

class MyPanel {
    cIGZImGuiService* service_;
    ImGuiTexture myTexture_;

    void OnInit() {
        // Generate RGBA32 pixel data (4 bytes per pixel)
        std::vector<uint8_t> pixels(128 * 128 * 4);
        // ... fill pixel data ...

        // Create texture - automatically manages lifetime
        if (!myTexture_.Create(service_, 128, 128, pixels.data())) {
            // Handle error
        }
    }

    void OnRender() {
        // Get texture ID - returns nullptr if device was lost
        void* texId = myTexture_.GetID();
        if (texId) {
            ImGui::Image(texId, ImVec2(128, 128));
        } else {
            ImGui::TextUnformatted("Texture unavailable");
        }
    }

    // Texture is automatically released in destructor
};
```

#### Manual API Usage

```cpp
#include "public/cIGZImGuiService.h"

ImGuiTextureDesc desc{};
desc.width = 128;
desc.height = 128;
desc.pixels = myRGBA32Data;  // Must remain valid during call

// Create texture - service stores pixel data internally
ImGuiTextureHandle handle = service->CreateTexture(desc);
if (handle.id == 0) {
    // Handle error
}

// In render loop:
void* texId = service->GetTextureID(handle);
if (texId) {
    ImGui::Image(texId, ImVec2(128, 128));
}

// When done:
service->ReleaseTexture(handle);
```

`GetTextureID` returns the driver's texture name cast to `void*`. Treat it as an opaque
`ImTextureID`. `ImGuiTextureDesc::useSystemMemory` is ignored: the driver decides where the
texture lives.

#### Device Generation Pattern

When the graphics device is reset, the device generation increments and old texture handles
become invalid:

```cpp
void OnRender() {
    static uint32_t lastGen = 0;
    uint32_t currentGen = service->GetDeviceGeneration();

    if (currentGen != lastGen) {
        // Device was reset - recreate textures
        RecreateMyTextures();
        lastGen = currentGen;
    }

    // Use textures normally...
}
```

The `ImGuiTexture` RAII wrapper handles this automatically by returning `nullptr` from
`GetID()` when generation changes.

#### Safety Notes

- **Always check return values**: `GetTextureID()` can return `nullptr` if device is lost
- **RGBA32 format**: Pixel data must be in RGBA32 format (4 bytes per pixel: R, G, B, A)
- **Thread safety**: Not thread-safe - call from the game's main thread only
- **Lifetime**: Release textures before unregistering panels or shutting down
- **Generation mismatch**: Handles from old device generations return `nullptr` from
  `GetTextureID()`

#### Error Handling

```cpp
ImGuiTextureHandle handle = service->CreateTexture(desc);
if (handle.id == 0) {
    LOG_ERROR("Texture creation failed");
    return;
}

void* texId = service->GetTextureID(handle);
if (!texId) {
    // Device lost, generation mismatch, or recreation failed.
    if (!service->IsTextureValid(handle)) {
        // Handle is stale (generation mismatch): create a new texture
    }
}
```

See `src/sample/ImGuiTextureSample.cpp` for a complete working example.

### Graphics driver info

```cpp
SC4GraphicsDriverInfo driver{};
if (service->GetActiveDriverInfo(driver)) {
    // driver.kind: BuiltInDirectX, BuiltInOpenGL, SCD3D11, Scvk or Unknown
    // driver.name: a display name, driver.modulePath: the DLL that implements it
}
```

The game's driver class ID cannot tell drivers apart (SCD3D11 reports the built-in DirectX
ID, scvk the OpenGL one), so the service identifies the driver by the module that implements it.

### DX7 interface access

`cIGZImGuiService::AcquireD3DInterfaces` hands out the built-in DirectX driver's
`IDirect3DDevice7` and `IDirectDraw7`. It returns false with any other driver, so treat it as
an optional extra and prefer the world draw service and the texture API, which work everywhere.

The service AddRef()s both interfaces and callers must Release() them. Acquire them per
operation rather than caching them across frames.

```cpp
IDirect3DDevice7* d3d = nullptr;
IDirectDraw7* dd = nullptr;
if (service->AcquireD3DInterfaces(&d3d, &dd)) {
    // use d3d/dd
    d3d->Release();
    dd->Release();
}
```

## Developer notes

- The services are `cIGZSystemService`s registered on `OnStart`. ImGui draws once per frame
  from a hook on the game's per-frame `cIGZGDriver::Flush` call in `cGZGraphicSystem::Draw`, so
  it does not appear in region thumbnails or photos. For more info on Services, see their
  [gzcom-dll Wiki entry](https://github.com/nsgomez/gzcom-dll/wiki/Service).
- Panels are registered with a unique `id` and a simple `on_render(void* data)`
  callback. Use the `data` pointer for per-panel state instead of globals.
- The ImGui context is owned by the service. Clients should not call
  `ImGui::CreateContext()` or `ImGui::DestroyContext()`. If you need to check
  readiness, call `cIGZImGuiService::GetContext()`.
- Use the `visible` flag in `ImGuiPanelDesc` to control initial visibility.
- Reverse engineering notes for the driver backend are in
  [docs/generic-backend-re.md](docs/generic-backend-re.md) and
  [docs/world-draw-design.md](docs/world-draw-design.md).
