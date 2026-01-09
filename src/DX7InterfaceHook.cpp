#include "DX7InterfaceHook.h"
#include "utils/Logger.h"

#include "imgui.h"
#include "imgui_impl_dx7.h"
#include "imgui_impl_win32.h"

#include <atomic>
#include <Windows.h>

namespace {
    constexpr size_t kEndSceneVTableIndex = 6;
}

static std::atomic<DX7InterfaceHook::FrameCallback> s_FrameCallback{nullptr};
static std::atomic<IDirect3DDevice7*> s_HookedDevice{nullptr};
static std::atomic<HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice7*)> s_OriginalEndScene{nullptr};

static HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice7* device)
{
    auto* d3dx = DX7InterfaceHook::s_pD3DX.load(std::memory_order_acquire);
    auto* d3d = d3dx ? d3dx->GetD3DDevice() : nullptr;
    auto* dd = d3dx ? d3dx->GetDD() : nullptr;

    if (d3dx && (!d3d || !dd)) {
        LOG_WARN("EndSceneHook: D3DX interface not ready (d3dx={}, d3d={}, dd={}), clearing",
            static_cast<void*>(d3dx),
            static_cast<void*>(d3d),
            static_cast<void*>(dd));
        DX7InterfaceHook::s_pD3DX.store(nullptr, std::memory_order_release);
    }

    auto callback = s_FrameCallback.load(std::memory_order_acquire);
    if (callback) {
        callback(device);
    }
    auto originalEndScene = s_OriginalEndScene.load(std::memory_order_acquire);
    return originalEndScene ? originalEndScene(device) : S_OK;
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
    if (candidate && (!candidate->GetD3DDevice() || !candidate->GetDD())) {
        LOG_WARN("DX7InterfaceHook::CaptureInterface: D3DX interface not ready yet (d3dx={}, d3d={}, dd={})",
            static_cast<void*>(candidate),
            static_cast<void*>(candidate->GetD3DDevice()),
            static_cast<void*>(candidate->GetDD()));
        candidate = nullptr;
    }

    s_pD3DX.store(candidate, std::memory_order_release);
    return candidate != nullptr;
}

bool DX7InterfaceHook::InitializeImGui(const HWND hwnd)
{
    auto* d3dx = s_pD3DX.load(std::memory_order_acquire);
    if (!d3dx || !hwnd || !IsWindow(hwnd)) {
        LOG_ERROR("DX7InterfaceHook::InitializeImGui: invalid inputs (hwnd={}, is_window={}, d3dx={})",
            static_cast<void*>(hwnd),
            hwnd ? IsWindow(hwnd) : false,
            static_cast<void*>(d3dx));
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    auto* d3dDevice = d3dx->GetD3DDevice();
    auto* dd = d3dx->GetDD();
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
    auto* d3dx = s_pD3DX.load(std::memory_order_acquire);
    if (!d3dx) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: D3DX interface not captured");
        return false;
    }

    IDirect3DDevice7* device = d3dx->GetD3DDevice();
    if (!device) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: D3D device is null");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: device vtable is null");
        return false;
    }

    auto hookedDevice = s_HookedDevice.load(std::memory_order_acquire);
    auto origEndScene = s_OriginalEndScene.load(std::memory_order_acquire);
    if (hookedDevice == device && origEndScene) {
        return true;
    }

    auto* originalFunc = reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice7*)>(
        vtable[kEndSceneVTableIndex]);
    if (!originalFunc) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: original EndScene is null");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("DX7InterfaceHook::InstallSceneHooks: VirtualProtect EndScene failed (error: {})", GetLastError());
        return false;
    }

    // Store original function atomically before modifying vtable
    s_OriginalEndScene.store(originalFunc, std::memory_order_release);

    // Atomically swap the vtable entry using InterlockedExchange
    void* hookFunc = reinterpret_cast<void*>(&EndSceneHook);
    InterlockedExchange(reinterpret_cast<LONG*>(&vtable[kEndSceneVTableIndex]),
                       reinterpret_cast<LONG>(hookFunc));

    // Store hooked device atomically
    s_HookedDevice.store(device, std::memory_order_release);

    VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    LOG_INFO("DX7InterfaceHook::InstallSceneHooks: hooked EndScene at index {}", kEndSceneVTableIndex);
    return true;
}

void DX7InterfaceHook::SetFrameCallback(const FrameCallback callback)
{
    s_FrameCallback.store(callback, std::memory_order_release);
}

cISGLDX7D3DX* DX7InterfaceHook::GetD3DXInterface()
{
    return s_pD3DX.load(std::memory_order_acquire);
}

void DX7InterfaceHook::ShutdownImGui()
{
    auto hookedDevice = s_HookedDevice.load(std::memory_order_acquire);
    auto origEndScene = s_OriginalEndScene.load(std::memory_order_acquire);

    if (hookedDevice && origEndScene) {
        void** vtable = *reinterpret_cast<void***>(hookedDevice);
        if (vtable) {
            DWORD oldProtect = 0;
            if (VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                // Restore original function atomically
                InterlockedExchange(reinterpret_cast<LONG*>(&vtable[kEndSceneVTableIndex]),
                                   reinterpret_cast<LONG>(origEndScene));
                VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
            }
        }
    }
    ImGui_ImplDX7_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    s_FrameCallback.store(nullptr, std::memory_order_release);
    s_OriginalEndScene.store(nullptr, std::memory_order_release);
    s_HookedDevice.store(nullptr, std::memory_order_release);
    s_pD3DX.store(nullptr, std::memory_order_release);
}

// Static member definitions
std::atomic<cISGLDX7D3DX*> DX7InterfaceHook::s_pD3DX{nullptr};
