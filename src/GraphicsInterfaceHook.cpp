#include "GraphicsInterfaceHook.h"

#include <atomic>
#include <d3d.h>
#include <filesystem>
#include <Windows.h>

#include "imgui.h"
#include "imgui_impl_dx7.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "utils/Fonts.h"
#include "utils/Logger.h"

namespace {
    constexpr size_t kEndSceneVTableIndex = 6;
}

static std::atomic<GraphicsInterfaceHook::FrameCallback> s_FrameCallback{nullptr};
static std::atomic<IDirect3DDevice7*> s_HookedDevice{nullptr};
static std::atomic<HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice7*)> s_OriginalEndScene{nullptr};

static HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice7* device)
{
    auto callback = s_FrameCallback.load(std::memory_order_acquire);
    if (callback) {
        callback(device);
    }
    auto originalEndScene = s_OriginalEndScene.load(std::memory_order_acquire);
    return originalEndScene ? originalEndScene(device) : S_OK;
}

bool GraphicsInterfaceHook::CaptureInterface(cIGZGDriver* pDriver)
{
    if (!pDriver) {
        LOG_ERROR("GraphicsInterfaceHook::CaptureInterface: null driver");
        return false;
    }

    const uint32_t driverClsid = pDriver->GetGZCLSID();

    if (driverClsid != kSCGDriverDirectX) {
        LOG_INFO("GraphicsInterfaceHook::CaptureInterface: unsupported driver clsid=0x{:08X}", driverClsid);
        return false;
    }

    // Offset 0x24C for this build (verified in logs)
    const auto driverPtr = static_cast<void*>(pDriver);
    cISGLDX7D3DX* candidate = *reinterpret_cast<cISGLDX7D3DX**>(
        static_cast<uint8_t*>(driverPtr) + 0x24C);

    if (!candidate) {
        return false;
    }
    if (!candidate->GetD3DDevice() || !candidate->GetDD()) {
        return false;
    }

    auto* current = s_pD3DX.load(std::memory_order_acquire);
    if (candidate != current) {
        if (candidate) {
            candidate->AddRef();
        }
        current = s_pD3DX.exchange(candidate, std::memory_order_acq_rel);
        if (current) {
            current->Release();
        }
    }
    return candidate != nullptr;
}

bool GraphicsInterfaceHook::InitializeImGui(const HWND hwnd, const ImGuiInitSettings& settings)
{
    auto* d3dx = s_pD3DX.load(std::memory_order_acquire);
    if (!d3dx || !hwnd || !IsWindow(hwnd)) {
        LOG_ERROR("GraphicsInterfaceHook::InitializeImGui: invalid inputs (hwnd={}, is_window={}, d3dx={})",
            static_cast<void*>(hwnd),
            hwnd ? IsWindow(hwnd) : false,
            static_cast<void*>(d3dx));
        return false;
    }

    auto* d3dDevice = d3dx->GetD3DDevice();
    auto* dd = d3dx->GetDD();
    if (!d3dDevice || !dd) {
        LOG_ERROR("GraphicsInterfaceHook::InitializeImGui: D3D interfaces not ready (device={}, dd={})",
            static_cast<void*>(d3dDevice),
            static_cast<void*>(dd));
        return false;
    }

    if (ImGui::GetCurrentContext()) {
        ImGuiIO& existingIo = ImGui::GetIO();
        if (existingIo.BackendRendererUserData) {
            ImGui_ImplDX7_Shutdown();
        }
        if (existingIo.BackendPlatformUserData) {
            ImGui_ImplWin32_Shutdown();
        }
        ImGui::DestroyContext();
        LOG_WARN("GraphicsInterfaceHook::InitializeImGui: destroyed stale ImGui state before reinitializing");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    bool win32Initialized = false;
    bool dx7Initialized = false;

    const auto cleanupOnFailure = [&]() {
        if (dx7Initialized) {
            ImGui_ImplDX7_Shutdown();
        }
        if (win32Initialized) {
            ImGui_ImplWin32_Shutdown();
        }
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    };

    if (settings.keyboardNav) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    }

    // Apply theme
    if (settings.theme == "light") {
        ImGui::StyleColorsLight();
    } else if (settings.theme == "classic") {
        ImGui::StyleColorsClassic();
    } else {
        ImGui::StyleColorsDark();
    }

    // Apply UI scale
    if (settings.uiScale != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(settings.uiScale);
    }

    // Configure font rendering
    ImFontConfig fontConfig;
    fontConfig.OversampleH = static_cast<ImS8>(settings.fontOversample);
    fontConfig.OversampleV = static_cast<ImS8>(settings.fontOversample);
    fontConfig.PixelSnapH = true;
    fontConfig.GlyphExtraAdvanceX = 0.0f;

    const float scaledFontSize = settings.fontSize * settings.uiScale;

    // Load font: custom TTF file or built-in ProggyVector
    ImFont* font = nullptr;
    if (!settings.fontFile.empty()) {
        const std::filesystem::path fontPath(settings.fontFile);
        if (std::filesystem::exists(fontPath)) {
            font = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), scaledFontSize, &fontConfig);
            if (font) {
                LOG_INFO("GraphicsInterfaceHook::InitializeImGui: loaded custom font from {}", fontPath.string());
            } else {
                LOG_WARN("GraphicsInterfaceHook::InitializeImGui: failed to load font from {}, falling back to built-in", fontPath.string());
            }
        } else {
            LOG_WARN("GraphicsInterfaceHook::InitializeImGui: font file not found: {}, falling back to built-in", fontPath.string());
        }
    }

    if (!font) {
        font = io.Fonts->AddFontFromMemoryCompressedTTF(
            ProggyVector_compressed_data, ProggyVector_compressed_size, scaledFontSize, &fontConfig);
        if (font) {
            LOG_INFO("GraphicsInterfaceHook::InitializeImGui: loaded built-in font (size={})", scaledFontSize);
        } else {
            LOG_WARN("GraphicsInterfaceHook::InitializeImGui: failed to load built-in font, will use the d3d7imgui default");
        }
    }

    if (font) {
        io.FontDefault = font;
    }

    if (!ImGui_ImplWin32_Init(hwnd)) {
        LOG_ERROR("GraphicsInterfaceHook::InitializeImGui: ImGui_ImplWin32_Init failed");
        cleanupOnFailure();
        return false;
    }
    win32Initialized = true;

    if (!ImGui_ImplDX7_Init(d3dDevice, dd)) {
        LOG_ERROR("GraphicsInterfaceHook::InitializeImGui: ImGui_ImplDX7_Init failed");
        cleanupOnFailure();
        return false;
    }
    dx7Initialized = true;

    if (!ImGui_ImplDX7_CreateDeviceObjects()) {
        LOG_ERROR("GraphicsInterfaceHook::InitializeImGui: ImGui_ImplDX7_CreateDeviceObjects failed");
        cleanupOnFailure();
        return false;
    }

    return true;
}

bool GraphicsInterfaceHook::InitializeImGuiD3D11(const HWND hwnd, ID3D11Device* device,
                                             ID3D11DeviceContext* context,
                                             const ImGuiInitSettings& settings)
{
    if (!hwnd || !IsWindow(hwnd) || !device || !context) return false;
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& existingIo = ImGui::GetIO();
        if (existingIo.BackendRendererUserData) ImGui_ImplDX11_Shutdown();
        if (existingIo.BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (settings.keyboardNav) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (settings.theme == "light") ImGui::StyleColorsLight();
    else if (settings.theme == "classic") ImGui::StyleColorsClassic();
    else ImGui::StyleColorsDark();
    if (settings.uiScale != 1.0f) ImGui::GetStyle().ScaleAllSizes(settings.uiScale);

    ImFontConfig fontConfig;
    fontConfig.OversampleH = static_cast<ImS8>(settings.fontOversample);
    fontConfig.OversampleV = static_cast<ImS8>(settings.fontOversample);
    fontConfig.PixelSnapH = true;
    float const fontSize = settings.fontSize * settings.uiScale;
    ImFont* font = nullptr;
    if (!settings.fontFile.empty() && std::filesystem::exists(settings.fontFile)) {
        font = io.Fonts->AddFontFromFileTTF(settings.fontFile.c_str(), fontSize, &fontConfig);
    }
    if (!font) {
        font = io.Fonts->AddFontFromMemoryCompressedTTF(
            ProggyVector_compressed_data, ProggyVector_compressed_size, fontSize, &fontConfig);
    }
    if (font) io.FontDefault = font;

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    return true;
}

bool GraphicsInterfaceHook::InstallSceneHooks()
{
    auto* d3dx = s_pD3DX.load(std::memory_order_acquire);
    if (!d3dx) {
        LOG_ERROR("GraphicsInterfaceHook::InstallSceneHooks: D3DX interface not captured");
        return false;
    }

    IDirect3DDevice7* device = d3dx->GetD3DDevice();
    if (!device) {
        LOG_ERROR("GraphicsInterfaceHook::InstallSceneHooks: D3D device is null");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable) {
        LOG_ERROR("GraphicsInterfaceHook::InstallSceneHooks: device vtable is null");
        return false;
    }

    auto hookedDevice = s_HookedDevice.load(std::memory_order_acquire);
    auto origEndScene = s_OriginalEndScene.load(std::memory_order_acquire);
    if (hookedDevice == device && origEndScene) {
        return true;
    }

    if (hookedDevice && origEndScene && hookedDevice != device) {
        void** oldVtable = *reinterpret_cast<void***>(hookedDevice);
        if (oldVtable) {
            DWORD oldProtect = 0;
            if (VirtualProtect(&oldVtable[kEndSceneVTableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                InterlockedExchange(reinterpret_cast<LONG*>(&oldVtable[kEndSceneVTableIndex]),
                                   reinterpret_cast<LONG>(origEndScene));
                VirtualProtect(&oldVtable[kEndSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
            }
        }
        hookedDevice->Release();
        s_HookedDevice.store(nullptr, std::memory_order_release);
        s_OriginalEndScene.store(nullptr, std::memory_order_release);
    }

    auto* originalFunc = reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice7*)>(
        vtable[kEndSceneVTableIndex]);
    if (!originalFunc) {
        LOG_ERROR("GraphicsInterfaceHook::InstallSceneHooks: original EndScene is null");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("GraphicsInterfaceHook::InstallSceneHooks: VirtualProtect EndScene failed (error: {})", GetLastError());
        return false;
    }

    // Store original function atomically before modifying vtable
    s_OriginalEndScene.store(originalFunc, std::memory_order_release);

    // Atomically swap the vtable entry using InterlockedExchange
    void* hookFunc = reinterpret_cast<void*>(&EndSceneHook);
    InterlockedExchange(reinterpret_cast<LONG*>(&vtable[kEndSceneVTableIndex]),
                       reinterpret_cast<LONG>(hookFunc));

    device->AddRef();
    // Store hooked device atomically
    s_HookedDevice.store(device, std::memory_order_release);

    VirtualProtect(&vtable[kEndSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
    LOG_INFO("GraphicsInterfaceHook::InstallSceneHooks: hooked EndScene at index {}", kEndSceneVTableIndex);
    return true;
}

void GraphicsInterfaceHook::SetFrameCallback(const FrameCallback callback)
{
    s_FrameCallback.store(callback, std::memory_order_release);
}

cISGLDX7D3DX* GraphicsInterfaceHook::GetD3DXInterface()
{
    return s_pD3DX.load(std::memory_order_acquire);
}

void GraphicsInterfaceHook::ShutdownImGui()
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
        hookedDevice->Release();
    }
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX7_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    s_FrameCallback.store(nullptr, std::memory_order_release);
    s_OriginalEndScene.store(nullptr, std::memory_order_release);
    s_HookedDevice.store(nullptr, std::memory_order_release);
    if (auto* d3dx = s_pD3DX.exchange(nullptr, std::memory_order_acq_rel)) {
        d3dx->Release();
    }
}

// Static member definitions
std::atomic<cISGLDX7D3DX*> GraphicsInterfaceHook::s_pD3DX{nullptr};
