#include "DX7InterfaceHook.h"
#include "utils/Logger.h"

#include "imgui.h"
#include "imgui_impl_dx7.h"
#include "imgui_impl_win32.h"

namespace {
    constexpr size_t kEndSceneVTableIndex = 6;
}

static DX7InterfaceHook::FrameCallback s_FrameCallback = nullptr;
static IDirect3DDevice7* s_HookedDevice = nullptr;
static HRESULT (STDMETHODCALLTYPE* s_OriginalEndScene)(IDirect3DDevice7*) = nullptr;

static HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice7* device)
{
    if (s_FrameCallback) {
        s_FrameCallback(device);
    }
    return s_OriginalEndScene ? s_OriginalEndScene(device) : S_OK;
}

bool DX7InterfaceHook::CaptureInterface(cIGZGDriver* pDriver)
{
    if (!pDriver) {
        LOG_ERROR("DX7InterfaceHook::CaptureInterface: null driver");
        return false;
    }

    const uint32_t driverClsid = pDriver->GetGZCLSID();

    if (driverClsid != kSCGDriverDirectX) {
        LOG_INFO("DX7InterfaceHook::CaptureInterface: unsupported driver clsid=0x{:08X}", driverClsid);
        return false;
    }

    // Offset 0x24C for this build (verified in logs)
    const auto driverPtr = static_cast<void*>(pDriver);
    cISGLDX7D3DX* candidate = *reinterpret_cast<cISGLDX7D3DX**>(
        static_cast<uint8_t*>(driverPtr) + 0x24C);

    if (!candidate) {
        LOG_ERROR("DX7InterfaceHook::CaptureInterface: D3DX pointer null at offset 0x24C");
    }

    s_pD3DX = candidate;
    return s_pD3DX != nullptr;
}

bool DX7InterfaceHook::InitializeImGui(const HWND hwnd)
{
    if (!s_pD3DX || !hwnd || !IsWindow(hwnd)) {
        LOG_ERROR("DX7InterfaceHook::InitializeImGui: invalid inputs (hwnd={}, is_window={}, s_pD3DX={})",
            static_cast<void*>(hwnd),
            hwnd ? IsWindow(hwnd) : false,
            static_cast<void*>(s_pD3DX));
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    auto* d3dDevice = s_pD3DX->GetD3DDevice();
    auto* dd = s_pD3DX->GetDD();
    if (!d3dDevice || !dd) {
        LOG_ERROR("DX7InterfaceHook::InitializeImGui: D3D interfaces not ready (device={}, dd={})",
            static_cast<void*>(d3dDevice),
            static_cast<void*>(dd));
        return false;
    }
    ImGui_ImplDX7_Init(d3dDevice, dd);
    ImGui_ImplDX7_CreateDeviceObjects();

    return true;
}

bool DX7InterfaceHook::InstallSceneHooks()
{
    if (!s_pD3DX) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: D3DX interface not captured");
        return false;
    }

    IDirect3DDevice7* device = s_pD3DX->GetD3DDevice();
    if (!device) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: D3D device is null");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: device vtable is null");
        return false;
    }

    if (s_HookedDevice == device && s_OriginalEndScene) {
        return true;
    }

    s_OriginalEndScene = reinterpret_cast<decltype(s_OriginalEndScene)>(vtable[kEndSceneVTableIndex]);
    if (!s_OriginalEndScene) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: original EndScene is null");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: VirtualProtect EndScene failed (error: {})", GetLastError());
        return false;
    }

    s_HookedDevice = device;
    vtable[kEndSceneVTableIndex] = reinterpret_cast<void*>(&EndSceneHook);
    VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    LOG_INFO("DX7InterfaceHook::InstallSceneHooks: hooked EndScene at index {}", kEndSceneVTableIndex);
    return true;
}

void DX7InterfaceHook::SetFrameCallback(const FrameCallback callback)
{
    s_FrameCallback = callback;
}

void DX7InterfaceHook::ShutdownImGui()
{
    if (s_HookedDevice && s_OriginalEndScene) {
        void** vtable = *reinterpret_cast<void***>(s_HookedDevice);
        if (vtable) {
            DWORD oldProtect = 0;
            if (VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                vtable[kEndSceneVTableIndex] = reinterpret_cast<void*>(s_OriginalEndScene);
                VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
            }
        }
    }
    ImGui_ImplDX7_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    s_FrameCallback = nullptr;
    s_OriginalEndScene = nullptr;
    s_HookedDevice = nullptr;
    s_pD3DX = nullptr;
}

// Static member definitions
cISGLDX7D3DX* DX7InterfaceHook::s_pD3DX = nullptr;
