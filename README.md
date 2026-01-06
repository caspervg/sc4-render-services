# sc4-imgui-service

ImGui service and samples for SimCity 4 DLL plugins using the gzcom-dll SDK.

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
- `SC4ImGuiService.dll` (deployed to `...\Documents\SimCity 4\Plugins\`)

Optional samples (deployed to `...\Documents\SimCity 4\Plugins\`):
- `SC4ImGuiSample.dll` (basic panel)
- `SC4ImGuiSampleCity.dll` (city-view only)
- `SC4ImGuiSampleDemo.dll` (ImGui demo window)

## Usage

- Link your DLL against `imgui.dll` and include `vendor/d3d7imgui/ImGui/imgui.h`.
- Query the service via `cIGZFrameWork::GetSystemService` with `kImGuiServiceID`
  and `GZIID_cIGZImGuiService` from `src/public/ImGuiServiceIds.h`.
- Register a panel with `ImGuiPanelDesc` and render using `ImGui::*`.
- The service owns the ImGui context; do not call `ImGui::CreateContext()` or
  `ImGui::DestroyContext()` in clients.

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

### DX7 interface access

If you need DirectX 7 interfaces (e.g., to create textures for `ImGui::Image`),
use `cIGZImGuiService::AcquireD3DInterfaces`. The service AddRef()s both
interfaces; callers must Release() them when done. Prefer acquiring per
operation/frame rather than caching across frames. Use
`IsDeviceReady()` and `GetDeviceGeneration()` to detect when cached textures
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
