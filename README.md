# sc4-render-services

Render services and samples (ImGui, Draw, S3D Camera) for SimCity 4 DLL plugins using the gzcom-dll SDK.

![Screenshot](docs/assets/preview.jpg)

## Build

Prereqs:
- CMake 4.0+
- Visual Studio (Win32 toolchain)

Steps (from repo root):
```
cmake -S . -B cmake-build-debug-visual-studio -G "Visual Studio 17 2022"
cmake --build cmake-build-debug-visual-studio --config Debug
```

## Outputs

Required:
- `imgui.dll` (deployed to `...\SimCity 4 Deluxe Edition\Apps`)
- `SC4RenderServices.dll` (deployed to `...\Documents\SimCity 4\Plugins\`)

Optional samples (deployed to `...\Documents\SimCity 4\Plugins\`):
- `SC4ImGuiSample.dll` (basic panel)
- `SC4ImGuiSampleCity.dll` (city-view only)
- `SC4ImGuiSampleDemo.dll` (ImGui demo window)
- `SC4ImGuiTextureSample.dll` (texture management example)
- `SC4WorldProjectionSample.dll` (world projection helpers)
- `SC4S3DCameraDebugSample.dll` (camera debug tooling)
- `SC4OverlayManagerSample.dll` (overlay manager example)
- `SC4DrawServiceSample.dll` (draw service UI + hooks)
- `SC4RoadDecalSample.dll` (road decal rendering)

## Provided Services

`SC4RenderServices.dll` registers three services (currently gated for SimCity 4
version `1.1.641`).

Detailed reference: [docs/services.md](docs/services.md)

### 1) ImGui service

- Service ID / IID: `kImGuiServiceID`, `GZIID_cIGZImGuiService`
  (`src/public/ImGuiServiceIds.h`)
- Interface: `cIGZImGuiService` (`src/public/cIGZImGuiService.h`)
- Provides panel registration, per-frame callbacks, font registration, DX7
  access, and managed texture APIs for `ImGui::Image()`.
- Examples: `SC4ImGuiSample.dll`, `SC4ImGuiSampleDemo.dll`,
  `SC4ImGuiTextureSample.dll`, `SC4ImGuiSampleCity.dll`

### 2) S3D camera service

- Service ID / IID: `kS3DCameraServiceID`, `GZIID_cIGZS3DCameraService`
  (`src/public/S3DCameraServiceIds.h`)
- Interface: `cIGZS3DCameraService` (`src/public/cIGZS3DCameraService.h`)
- Provides camera creation/wrapping, world/screen projection helpers, viewport
  and transform operations.
- Examples: `SC4WorldProjectionSample.dll`, `SC4S3DCameraDebugSample.dll`

### 3) Draw service

- Service ID / IID: `kDrawServiceID`, `GZIID_cIGZDrawService`
  (`src/public/cIGZDrawService.h`)
- Interface: `cIGZDrawService` (`src/public/cIGZDrawService.h`)
- Provides draw-context wrapping, render-pass callbacks
  (`PreStatic`...`PostDynamic`), and helpers for render state, mesh/model draw
  calls, and primitive drawing.
- Examples: `SC4DrawServiceSample.dll`, `SC4RoadDecalSample.dll`

## Usage

- Link your DLL against `imgui.dll` and include `vendor/d3d7imgui/ImGui/imgui.h`.
- Query the service via `cIGZFrameWork::GetSystemService` with `kImGuiServiceID`
  and `GZIID_cIGZImGuiService` from `src/public/ImGuiServiceIds.h`.
- Register a panel with `ImGuiPanelDesc` and render using `ImGui::*`.
- The service owns the ImGui context; do not call `ImGui::CreateContext()` or
  `ImGui::DestroyContext()` in clients.

## Render Callbacks

Two callback models are available:

- `cIGZImGuiService` panel callbacks (`on_init`, `on_update`, `on_render`,
  `on_visible_changed`, `on_shutdown`, `on_unregister`, `on_device_lost`,
  `on_device_restored`) for panel lifecycle and per-frame UI drawing.
- `cIGZImGuiService::QueueRender(...)` for one-shot work on the next ImGui
  frame. The optional cleanup callback runs after execution (or during
  shutdown if still queued).
- `cIGZDrawService::RegisterDrawPassCallback(...)` for renderer pass hooks.
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

- `src/public/cIGZImGuiService.h` (service interface + `ImGuiPanelDesc`)
- `src/public/ImGuiServiceIds.h` (service IDs and API version)
- `src/public/ImGuiPanel.h` and `src/public/ImGuiPanelAdapter.h` (optional class-style adapter)

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

The service provides a comprehensive texture management API that handles device loss, memory management, and automatic recreation. This is the **recommended** way to create textures for use with `ImGui::Image()`.

#### Features

- **Automatic device loss handling**: Textures survive Alt+Tab and resolution changes
- **Generation tracking**: Old texture handles are automatically invalidated after device reset
- **Memory safety**: Source pixel data is stored for automatic recreation
- **Fallback support**: Automatically falls back to system memory if video memory is exhausted
- **RAII wrapper**: Optional `ImGuiTexture` class for automatic lifetime management

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

// Create texture descriptor
ImGuiTextureDesc desc{};
desc.width = 128;
desc.height = 128;
desc.pixels = myRGBA32Data;  // Must remain valid during call
desc.useSystemMemory = false;  // Prefer video memory

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

#### Device Generation Pattern

When the DirectX device is reset (Alt+Tab, resolution change), the device generation increments. Old texture handles become invalid:

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

The `ImGuiTexture` RAII wrapper handles this automatically by returning `nullptr` from `GetID()` when generation changes.

#### Safety Notes

- **Always check return values**: `GetTextureID()` can return `nullptr` if device is lost
- **Store source data**: The service keeps a copy, but you may want to keep your own for modification
- **RGBA32 format**: Pixel data must be in RGBA32 format (4 bytes per pixel: R, G, B, A)
- **Thread safety**: Not thread-safe - call from render thread only
- **Lifetime**: Release textures before unregistering panels or shutting down
- **Generation mismatch**: Handles from old device generations return `nullptr` from `GetTextureID()`

#### Performance Considerations

- **Video memory preferred**: Set `useSystemMemory = false` for better performance (default)
- **Automatic fallback**: Service falls back to system memory if video memory is exhausted
- **On-demand recreation**: Surfaces are recreated lazily when first accessed after device loss
- **Minimal overhead**: Device loss detection uses existing cooperative level checks

#### Error Handling

The API is designed to fail gracefully:

```cpp
// Creation can fail - check handle
ImGuiTextureHandle handle = service->CreateTexture(desc);
if (handle.id == 0) {
    LOG_ERROR("Texture creation failed");
    return;
}

// GetTextureID returns nullptr on failure
void* texId = service->GetTextureID(handle);
if (!texId) {
    // Device lost, generation mismatch, or surface recreation failed
    // Check IsTextureValid() to distinguish:
    if (!service->IsTextureValid(handle)) {
        // Handle is stale (generation mismatch)
        // Need to create new texture
    }
}
```

See `src/sample/ImGuiTextureSample.cpp` for a complete working example.

### DX7 interface access

**Note**: For texture creation, prefer using the [Safe Texture Management API](#safe-texture-management) instead of manually managing DirectDraw surfaces. The texture API handles device loss automatically.

If you need DirectX 7 interfaces for advanced use cases, use `cIGZImGuiService::AcquireD3DInterfaces`. The service AddRef()s both
interfaces; callers must Release() them when done. Prefer acquiring per
operation/frame rather than caching across frames. Use
`IsDeviceReady()` and `GetDeviceGeneration()` to detect when cached resources
must be recreated.

Example:
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

- The service is a `cIGZSystemService` registered on `OnStart`, and it is added to
  the tick list via `cIGZFrameWork::AddToTick`. Rendering is driven from the
  DX7 EndScene hook once the D3D interface is available. For more info on Services, 
  see their [gzcom-dll Wiki entry](https://github.com/nsgomez/gzcom-dll/wiki/Service).
- Panels are registered with a unique `id` and a simple `on_render(void* data)`
  callback. Use the `data` pointer for per-panel state instead of globals.
- The ImGui context is owned by the service. Clients should not call
  `ImGui::CreateContext()` or `ImGui::DestroyContext()`. If you need to check
  readiness, call `cIGZImGuiService::GetContext()`.
- Use the `visible` flag in `ImGuiPanelDesc` to control initial visibility.

## Notes

- `imgui.dll` must live in the SC4 `Apps` folder to be loaded as a dependency.
- The service owns the ImGui context and backend initialization. Clients do not
  need to call `ImGui::CreateContext()` or `ImGui::DestroyContext()` or worry about hooking into
  the DirectX7 etc.
