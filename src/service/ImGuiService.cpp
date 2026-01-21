// ReSharper disable CppDFAUnreachableCode
// ReSharper disable CppDFAConstantConditions
#include "ImGuiService.h"

#include <algorithm>
#include <atomic>
#include <ddraw.h>
#include <ranges>
#include <winerror.h>

#include "cIGZFrameWorkW32.h"
#include "cIGZGraphicSystem2.h"
#include "cRZAutoRefCount.h"
#include "cRZCOMDllDirector.h"
#include "DX7InterfaceHook.h"
#include "GZServPtrs.h"
#include "imgui_impl_dx7.h"
#include "imgui_impl_win32.h"
#include "public/ImGuiServiceIds.h"
#include "utils/Logger.h"

namespace {
    std::atomic<ImGuiService*> g_instance{nullptr};
    std::atomic<DWORD> g_renderThreadId{0};

    struct Dx7ImGuiStateRestore {
        IDirect3DDevice7* device;
        bool hasStage0Coord;
        bool hasStage0Transform;
        bool hasStage1Coord;
        bool hasStage1Transform;
        bool hasAlphaTestEnable;
        DWORD stage0Coord;
        DWORD stage0Transform;
        DWORD stage1Coord;
        DWORD stage1Transform;
        DWORD alphaTestEnable;

        explicit Dx7ImGuiStateRestore(IDirect3DDevice7* deviceIn)
            : device(deviceIn)
            , hasStage0Coord(false)
            , hasStage0Transform(false)
            , hasStage1Coord(false)
            , hasStage1Transform(false)
            , hasAlphaTestEnable(false)
            , stage0Coord(0)
            , stage0Transform(0)
            , stage1Coord(0)
            , stage1Transform(0)
            , alphaTestEnable(0) {
            if (!device) {
                return;
            }
            hasStage0Coord = SUCCEEDED(device->GetTextureStageState(
                0, D3DTSS_TEXCOORDINDEX, &stage0Coord));
            hasStage0Transform = SUCCEEDED(device->GetTextureStageState(
                0, D3DTSS_TEXTURETRANSFORMFLAGS, &stage0Transform));
            hasStage1Coord = SUCCEEDED(device->GetTextureStageState(
                1, D3DTSS_TEXCOORDINDEX, &stage1Coord));
            hasStage1Transform = SUCCEEDED(device->GetTextureStageState(
                1, D3DTSS_TEXTURETRANSFORMFLAGS, &stage1Transform));
            hasAlphaTestEnable = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_ALPHATESTENABLE,
                    &alphaTestEnable));
        }

        ~Dx7ImGuiStateRestore() {
            if (!device) {
                return;
            }
            if (hasStage0Coord) {
                device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, stage0Coord);
            }
            if (hasStage0Transform) {
                device->SetTextureStageState(
                    0, D3DTSS_TEXTURETRANSFORMFLAGS, stage0Transform);
            }
            if (hasStage1Coord) {
                device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, stage1Coord);
            }
            if (hasStage1Transform) {
                device->SetTextureStageState(
                    1, D3DTSS_TEXTURETRANSFORMFLAGS, stage1Transform);
            }
            if (hasAlphaTestEnable) {
                device->SetRenderState(
                    D3DRENDERSTATE_ALPHATESTENABLE, alphaTestEnable);
            }
        }
    };
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ImGuiService::ImGuiService()
    : cRZBaseSystemService(kImGuiServiceID, 0)
    , gameWindow_(nullptr)
    , originalWndProc_(nullptr)
    , initialized_(false)
    , imguiInitialized_(false)
    , hookInstalled_(false)
    , warnedNoDriver_(false)
    , warnedMissingWindow_(false)
    , deviceLost_(false)
    , deviceGeneration_(0)
    , nextTextureId_(1) {}

ImGuiService::~ImGuiService() {
    auto expected = this;
    g_instance.compare_exchange_strong(expected, nullptr, std::memory_order_release);
}

uint32_t ImGuiService::AddRef() {
    return cRZBaseSystemService::AddRef();
}

uint32_t ImGuiService::Release() {
    return cRZBaseSystemService::Release();
}

bool ImGuiService::QueryInterface(uint32_t riid, void** ppvObj) {
    if (riid == GZIID_cIGZImGuiService) {
        *ppvObj = static_cast<cIGZImGuiService*>(this);
        AddRef();
        return true;
    }

    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

bool ImGuiService::Init() {
    if (initialized_) {
        return true;
    }

    Logger::Initialize("SC4ImGuiService", "");
    LOG_INFO("ImGuiService: initialized");
    SetServiceRunning(true);
    initialized_ = true;
    g_instance.store(this, std::memory_order_release);
    return true;
}

bool ImGuiService::Shutdown() {
    if (!initialized_) {
        return true;
    }

    {
        std::lock_guard panelLock(panelsMutex_);
        for (const auto& panel : panels_) {
            if (panel.desc.on_shutdown) {
                panel.desc.on_shutdown(panel.desc.data);
            }
        }
        panels_.clear();
    }

    // Clean up all textures before shutting down ImGui
    {
        std::lock_guard textureLock(texturesMutex_);
        for (auto& texture : textures_ | std::views::values) {
            if (texture.surface) {
                texture.surface->Release();
                texture.surface = nullptr;
            }
        }
        textures_.clear();
    }

    RemoveWndProcHook_();
    DX7InterfaceHook::SetFrameCallback(nullptr);
    DX7InterfaceHook::ShutdownImGui();

    imguiInitialized_ = false;
    hookInstalled_ = false;
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    SetServiceRunning(false);
    initialized_ = false;
    return true;
}

bool ImGuiService::OnTick(uint32_t) {
    if (!initialized_) {
        return true;
    }

    if (EnsureInitialized_()) {
        InitializePanels_();
    }
    return true;
}

bool ImGuiService::OnIdle(uint32_t) {
    return OnTick(0);
}

uint32_t ImGuiService::GetServiceID() const {
    return serviceID;
}

uint32_t ImGuiService::GetApiVersion() const {
    return kImGuiServiceApiVersion;
}

void* ImGuiService::GetContext() const {
    return ImGui::GetCurrentContext();
}

bool ImGuiService::RegisterPanel(const ImGuiPanelDesc& desc) {
    if (!desc.on_render) {
        LOG_WARN("ImGuiService: rejected panel {} (null on_render)", desc.id);
        return false;
    }

    {
        std::lock_guard lock(panelsMutex_);
        const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
            return entry.desc.id == desc.id;
        });
        if (it != panels_.end()) {
            LOG_WARN("ImGuiService: rejected panel {} (duplicate id)", desc.id);
            return false;
        }

        panels_.push_back(PanelEntry{desc, false});
        SortPanels_();
    }

    if (imguiInitialized_) {
        InitializePanels_();
    }
    LOG_INFO("ImGuiService: registered panel {} (order={})", desc.id, desc.order);
    return true;
}

bool ImGuiService::UnregisterPanel(uint32_t panelId) {
    std::lock_guard lock(panelsMutex_);
    const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
        return entry.desc.id == panelId;
    });
    if (it == panels_.end()) {
        LOG_WARN("ImGuiService: unregister failed for panel {}", panelId);
        return false;
    }

    if (it->desc.on_unregister) {
        it->desc.on_unregister(it->desc.data);
    }

    panels_.erase(it);
    LOG_INFO("ImGuiService: unregistered panel {}", panelId);
    return true;
}

bool ImGuiService::SetPanelVisible(const uint32_t panelId, const bool visible) {
    std::lock_guard lock(panelsMutex_);
    const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
        return entry.desc.id == panelId;
    });
    if (it == panels_.end()) {
        return false;
    }

    if (it->desc.visible == visible) {
        return true;
    }

    it->desc.visible = visible;
    if (it->desc.on_visible_changed) {
        it->desc.on_visible_changed(it->desc.data, visible);
    }
    return true;
}

bool ImGuiService::AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) {
    if (!outD3D || !outDD) {
        return false;
    }

    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    if (!d3dx) {
        return false;
    }

    auto* d3d = d3dx->GetD3DDevice();
    auto* dd = d3dx->GetDD();
    if (!d3d || !dd) {
        return false;
    }

    d3d->AddRef();
    dd->AddRef();
    *outD3D = d3d;
    *outDD = dd;
    return true;
}

bool ImGuiService::IsDeviceReady() const {
    if (!imguiInitialized_) {
        return false;
    }
    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    return d3dx && d3dx->GetD3DDevice() && d3dx->GetDD();
}

uint32_t ImGuiService::GetDeviceGeneration() const {
    return deviceGeneration_.load(std::memory_order_acquire);
}

void ImGuiService::RenderFrameThunk_(IDirect3DDevice7* device) {
    auto* instance = g_instance.load(std::memory_order_acquire);
    if (instance) {
        instance->RenderFrame_(device);
    }
}

void ImGuiService::RenderFrame_(IDirect3DDevice7* device) {
    static auto loggedFirstRender = false;
    const DWORD threadId = GetCurrentThreadId();
    const DWORD prevThreadId = g_renderThreadId.load(std::memory_order_acquire);
    if (prevThreadId == 0) {
        g_renderThreadId.store(threadId, std::memory_order_release);
        LOG_DEBUG("ImGuiService::RenderFrame_: render thread id set to {}", threadId);
    } else if (prevThreadId != threadId) {
        LOG_WARN("ImGuiService::RenderFrame_: render thread id changed ({} -> {})",
            prevThreadId, threadId);
        g_renderThreadId.store(threadId, std::memory_order_release);
    }
    if (!imguiInitialized_) {
        return;
    }

    {
        std::lock_guard lock(panelsMutex_);
        if (panels_.empty()) {
            return;
        }
    }

    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    if (!d3dx || device != d3dx->GetD3DDevice()) {
        return;
    }
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    InitializePanels_();

    auto* dd = d3dx->GetDD();
    if (!dd) {
        return;
    }

    // Check for device loss
    HRESULT hr = dd->TestCooperativeLevel();
    if (hr == DDERR_SURFACELOST || hr == DDERR_WRONGMODE) {
        // Treat only explicit device loss conditions as device lost
        if (!deviceLost_) {
            OnDeviceLost_();
        }
        return;  // Skip rendering when device is lost
    } else if (FAILED(hr)) {
        // Non-device-loss failure: skip rendering but do not change deviceLost_ state
        return;
    } else if (deviceLost_) {
        OnDeviceRestored_();
    }

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX7_NewFrame();
    ImGui::NewFrame();

    {
        std::lock_guard panelUpdateLock(panelsMutex_);
        for (auto& panel : panels_) {
            if (panel.desc.visible && panel.desc.on_update) {
                panel.desc.on_update(panel.desc.data);
            }
        }
    }

    {
        std::lock_guard panelRenderLock(panelsMutex_);
        for (auto& panel : panels_) {
            if (panel.desc.visible && panel.desc.on_render) {
                // Push the panel's font if specified
                if (panel.desc.fontId != 0) {
                    if (auto* font = static_cast<ImFont*>(GetFont(panel.desc.fontId))) {
                        ImGui::PushFont(font, 0.0f);  // 0.0f preserves current font size
                    }
                }

                panel.desc.on_render(panel.desc.data);

                // Pop the font if we pushed one
                if (panel.desc.fontId != 0) {
                    if (auto* font = static_cast<ImFont*>(GetFont(panel.desc.fontId))) {
                        ImGui::PopFont();
                    }
                }
            }
        }
    }

    // Preserve game render state that we override for ImGui's draw pass.
    Dx7ImGuiStateRestore stateRestore(device);

    // Reset texture coordinate generation/transform state that can be left
    // dirty by the game and cause garbled font sampling by the ImGui backend.
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());

    if (!loggedFirstRender) {
        LOG_INFO("ImGuiService: rendered first frame with {} panel(s)", panels_.size());
        loggedFirstRender = true;
    }
}

bool ImGuiService::EnsureInitialized_() {
    if (imguiInitialized_) {
        return true;
    }

    cIGZGraphicSystem2Ptr pGS2;
    if (!pGS2) {
        return false;
    }

    cIGZGDriver* pDriver = pGS2->GetGDriver();
    if (!pDriver) {
        if (!warnedNoDriver_) {
            LOG_WARN("ImGuiService: graphics driver not available yet");
            warnedNoDriver_ = true;
        }
        return false;
    }

    if (pDriver->GetGZCLSID() != kSCGDriverDirectX) {
        if (!warnedNoDriver_) {
            LOG_WARN("ImGuiService: not a DirectX driver, skipping initialization");
            warnedNoDriver_ = true;
        }
        return false;
    }

    if (!DX7InterfaceHook::CaptureInterface(pDriver)) {
        LOG_ERROR("ImGuiService: failed to capture D3DX interface");
        return false;
    }
    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    if (!d3dx || !d3dx->GetD3DDevice() || !d3dx->GetDD()) {
        LOG_WARN("ImGuiService: D3DX interface not ready yet (d3dx={}, d3d={}, dd={})",
            static_cast<void*>(d3dx),
            static_cast<void*>(d3dx ? d3dx->GetD3DDevice() : nullptr),
            static_cast<void*>(d3dx ? d3dx->GetDD() : nullptr));
        return false;
    }

    cRZAutoRefCount<cIGZFrameWorkW32> pFrameworkW32;
    if (!RZGetFrameWork()->QueryInterface(GZIID_cIGZFrameWorkW32, pFrameworkW32.AsPPVoid())) {
        return false;
    }
    if (!pFrameworkW32) {
        return false;
    }

    HWND hwnd = pFrameworkW32->GetMainHWND();
    if (!hwnd || !IsWindow(hwnd)) {
        if (!warnedMissingWindow_) {
            LOG_WARN("ImGuiService: game window not ready yet");
            warnedMissingWindow_ = true;
        }
        return false;
    }

    if (!DX7InterfaceHook::InitializeImGui(hwnd)) {
        LOG_ERROR("ImGuiService: failed to initialize ImGui backends");
        return false;
    }

    imguiInitialized_ = true;
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    warnedNoDriver_ = false;
    warnedMissingWindow_ = false;

    if (!InstallWndProcHook_(hwnd)) {
        LOG_WARN("ImGuiService: failed to install WndProc hook");
    }
    DX7InterfaceHook::SetFrameCallback(&ImGuiService::RenderFrameThunk_);
    DX7InterfaceHook::InstallSceneHooks();
    LOG_INFO("ImGuiService: ImGui initialized and scene hooks installed");
    return true;
}

void ImGuiService::InitializePanels_() {
    if (!imguiInitialized_) {
        return;
    }

    for (auto& panel : panels_) {
        if (!panel.initialized) {
            if (panel.desc.on_init) {
                panel.desc.on_init(panel.desc.data);
            }
            panel.initialized = true;
        }
    }
}

void ImGuiService::SortPanels_() {
    std::sort(panels_.begin(), panels_.end(), [](const PanelEntry& a, const PanelEntry& b) {
        return a.desc.order < b.desc.order;
    });
}

bool ImGuiService::InstallWndProcHook_(HWND hwnd) {
    if (hookInstalled_) {
        return true;
    }

    originalWndProc_ = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!originalWndProc_) {
        return false;
    }

    gameWindow_ = hwnd;
    if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ImGuiService::WndProcHook))) {
        originalWndProc_ = nullptr;
        gameWindow_ = nullptr;
        return false;
    }

    hookInstalled_ = true;
    return true;
}

void ImGuiService::RemoveWndProcHook_() {
    if (hookInstalled_ && gameWindow_ && originalWndProc_) {
        SetWindowLongPtrW(gameWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWndProc_));
    }

    hookInstalled_ = false;
    originalWndProc_ = nullptr;
    gameWindow_ = nullptr;
}

LRESULT CALLBACK ImGuiService::WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui::GetCurrentContext() != nullptr) {
        LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        if (imguiResult) {
            return imguiResult;
        }

        ImGuiIO& io = ImGui::GetIO();
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) && io.WantCaptureMouse) {
            return 0;
        }
        if (((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR) && io.WantCaptureKeyboard) {
            return 0;
        }
    }

    auto* instance = g_instance.load(std::memory_order_acquire);
    if (!instance || !instance->originalWndProc_) {
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return CallWindowProcW(instance->originalWndProc_, hWnd, msg, wParam, lParam);
}

// Texture management implementation

ImGuiTextureHandle ImGuiService::CreateTexture(const ImGuiTextureDesc& desc) {  
    const DWORD threadId = GetCurrentThreadId();
    const DWORD renderThreadId = g_renderThreadId.load(std::memory_order_acquire);
    if (renderThreadId != 0 && renderThreadId != threadId) {
        LOG_WARN("ImGuiService::CreateTexture: called off render thread (tid={}, render_tid={})",
            threadId, renderThreadId);
    } else {
        LOG_DEBUG("ImGuiService::CreateTexture: thread id {}", threadId);
    }
    // Validate parameters
    if (desc.width == 0 || desc.height == 0 || !desc.pixels) {
        LOG_ERROR("ImGuiService::CreateTexture: invalid parameters (width={}, height={}, pixels={})",
            desc.width, desc.height, static_cast<const void*>(desc.pixels));
        return ImGuiTextureHandle{0, 0};
    }

    // Check for potential integer overflow in size calculation
    // Ensure width * height doesn't overflow when computing pixel count
    if (desc.height > SIZE_MAX / desc.width) {
        LOG_ERROR("ImGuiService::CreateTexture: dimensions would overflow (width={}, height={})",
            desc.width, desc.height);
        return ImGuiTextureHandle{0, 0};
    }

    const size_t pixelCount = static_cast<size_t>(desc.width) * desc.height;

    // Ensure pixelCount * 4 doesn't overflow when computing byte size
    if (pixelCount > SIZE_MAX / 4) {
        LOG_ERROR("ImGuiService::CreateTexture: texture too large ({} pixels)", pixelCount);
        return ImGuiTextureHandle{0, 0};
    }

    if (!IsDeviceReady()) {
        LOG_WARN("ImGuiService::CreateTexture: device not ready, texture will be created on-demand");
    }

    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);

    // Create managed texture entry
    ManagedTexture tex;
    tex.id = nextTextureId_++;
    tex.width = desc.width;
    tex.height = desc.height;
    tex.creationGeneration = currentGen;
    tex.useSystemMemory = desc.useSystemMemory;
    tex.surface = nullptr;
    tex.needsRecreation = false;

    // Store source pixel data for recreation after device loss
    const size_t dataSize = pixelCount * 4;  // RGBA32
    tex.sourceData.resize(dataSize);
    std::memcpy(tex.sourceData.data(), desc.pixels, dataSize);

    // Attempt initial surface creation
    if (IsDeviceReady() && !deviceLost_) {
        if (!CreateSurfaceForTexture_(tex)) {
            LOG_WARN("ImGuiService::CreateTexture: surface creation failed, will retry later (id={})", tex.id);
            tex.needsRecreation = true;
        }
    } else {
        tex.needsRecreation = true;
    }

    const uint32_t textureId = tex.id;

    {
        std::lock_guard lock(texturesMutex_);
        textures_.emplace(textureId, std::move(tex));
    }

    LOG_INFO("ImGuiService::CreateTexture: created texture id={} ({}x{}, gen={})",
        textureId, desc.width, desc.height, currentGen);

    return ImGuiTextureHandle{textureId, currentGen};
}

bool ImGuiService::CreateSurfaceForTexture_(ManagedTexture& tex) {
    if (!IsDeviceReady()) {
        return false;
    }

    // Acquire D3D interfaces with RAII cleanup
    IDirect3DDevice7* d3d = nullptr;
    IDirectDraw7* dd = nullptr;
    if (!AcquireD3DInterfaces(&d3d, &dd)) {
        LOG_ERROR("ImGuiService::CreateSurfaceForTexture_: failed to acquire D3D interfaces");
        return false;
    }

    // RAII cleanup for interfaces
    struct D3DCleanup {
        IDirect3DDevice7* d3d;
        IDirectDraw7* dd;
        ~D3DCleanup() {
            if (d3d) d3d->Release();
            if (dd) dd->Release();
        }
    } cleanup{d3d, dd};

    // Set up surface descriptor
    DDSURFACEDESC2 ddsd{};
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    ddsd.dwWidth = tex.width;
    ddsd.dwHeight = tex.height;
    ddsd.ddsCaps.dwCaps = DDSCAPS_TEXTURE;

    // Use video memory or system memory based on flag
    if (tex.useSystemMemory) {
        ddsd.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    } else {
        ddsd.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY;
    }

    // 32-bit ARGB pixel format
    ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    ddsd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    ddsd.ddpfPixelFormat.dwRGBBitCount = 32;
    ddsd.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    ddsd.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    ddsd.ddpfPixelFormat.dwBBitMask = 0x000000FF;
    ddsd.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;

    IDirectDrawSurface7* surface = nullptr;
    HRESULT hr = dd->CreateSurface(&ddsd, &surface, nullptr);

    // Fallback to system memory if video memory is exhausted
    if (hr == DDERR_OUTOFVIDEOMEMORY && !tex.useSystemMemory) {
        LOG_WARN("ImGuiService::CreateSurfaceForTexture_: video memory exhausted, falling back to system memory (id={})", tex.id);
        ddsd.ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
        ddsd.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
        hr = dd->CreateSurface(&ddsd, &surface, nullptr);
        if (SUCCEEDED(hr)) {
            tex.useSystemMemory = true;
        }
    }

    if (FAILED(hr) || !surface) {
        LOG_ERROR("ImGuiService::CreateSurfaceForTexture_: CreateSurface failed (hr=0x{:08X}, id={})", hr, tex.id);
        return false;
    }

    // Lock surface for writing
    DDSURFACEDESC2 lockDesc{};
    lockDesc.dwSize = sizeof(lockDesc);
    hr = surface->Lock(nullptr, &lockDesc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("ImGuiService::CreateSurfaceForTexture_: Lock failed (hr=0x{:08X}, id={})", hr, tex.id);
        surface->Release();
        return false;
    }

    // Copy pixel data row-by-row (respecting lPitch)
    const auto* srcPixels = reinterpret_cast<const uint8_t*>(tex.sourceData.data());
    auto* dstPixels = static_cast<uint8_t*>(lockDesc.lpSurface);
    const uint32_t srcPitch = tex.width * 4;  // RGBA32
    const uint32_t dstPitch = lockDesc.lPitch;

    for (uint32_t y = 0; y < tex.height; ++y) {
        std::memcpy(dstPixels + y * dstPitch, srcPixels + y * srcPitch, srcPitch);
    }

    surface->Unlock(nullptr);

    // Clean up old surface if it exists
    if (tex.surface) {
        tex.surface->Release();
    }

    tex.surface = surface;
    tex.needsRecreation = false;
    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    tex.creationGeneration = currentGen;

    LOG_INFO("ImGuiService::CreateSurfaceForTexture_: surface created successfully (id={}, gen={})",
        tex.id, currentGen);
    return true;
}

void* ImGuiService::GetTextureID(ImGuiTextureHandle handle) {
    // Check device generation first - return nullptr if mismatch
    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    if (handle.generation != currentGen) {
        return nullptr;
    }

    // Check device lost flag
    if (deviceLost_) {
        return nullptr;
    }

    std::lock_guard lock(texturesMutex_);

    // Find texture by ID
    auto it = textures_.find(handle.id);

    if (it == textures_.end()) {
        return nullptr;
    }

    ManagedTexture& tex = it->second;

    // Recreate surface if needed
    if (tex.needsRecreation || !tex.surface) {
        if (!CreateSurfaceForTexture_(tex)) {
            LOG_WARN("ImGuiService::GetTextureID: failed to recreate surface (id={})", tex.id);
            return nullptr;
        }
    }

    // Validate surface is not lost
    // IsLost() returns DD_OK (S_OK) if surface is valid, DDERR_SURFACELOST if lost; other return values
    // (including unexpected error codes) are not explicitly handled here and are treated as a valid surface.
    if (tex.surface) {
        HRESULT hr = tex.surface->IsLost();
        if (hr == DDERR_SURFACELOST) {
            LOG_WARN("ImGuiService::GetTextureID: surface is lost (id={})", tex.id);
            tex.surface->Release();
            tex.surface = nullptr;
            tex.needsRecreation = true;
            return nullptr;
        }
    }

    return static_cast<void*>(tex.surface);
}

void ImGuiService::ReleaseTexture(ImGuiTextureHandle handle) {
    std::lock_guard lock(texturesMutex_);
    return;

    auto it = textures_.find(handle.id);

    if (it == textures_.end()) {
        return;
    }

    if (it->second.surface) {
        it->second.surface->Release();
        it->second.surface = nullptr;
    }

    LOG_INFO("ImGuiService::ReleaseTexture: released texture (id={})", handle.id);
    textures_.erase(it);
}

bool ImGuiService::IsTextureValid(ImGuiTextureHandle handle) const {
    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    if (handle.generation != currentGen) {
        return false;
    }

    if (deviceLost_) {
        return false;
    }

    std::lock_guard lock(texturesMutex_);
    return textures_.find(handle.id) != textures_.end();
}

void ImGuiService::OnDeviceLost_() {
    deviceLost_ = true;

    // Invalidate all texture surfaces
    {
        std::lock_guard lock(texturesMutex_);
        for (auto& tex : textures_ | std::views::values) {
            if (tex.surface) {
                tex.surface->Release();
                tex.surface = nullptr;
            }
            tex.needsRecreation = true;
        }
    }

    ImGui_ImplDX7_InvalidateDeviceObjects();

    {
        std::lock_guard lock(texturesMutex_);
        LOG_WARN("ImGuiService::OnDeviceLost_: device lost, invalidated {} texture(s)", textures_.size());
    }
}

void ImGuiService::OnDeviceRestored_() {
    deviceLost_ = false;

    // Increment device generation to invalidate old handles
    uint32_t newGen = deviceGeneration_.fetch_add(1, std::memory_order_release) + 1;

    ImGui_ImplDX7_CreateDeviceObjects();

    LOG_INFO("ImGuiService::OnDeviceRestored_: device restored (new gen={}), textures will recreate on-demand", newGen);
}

void ImGuiService::InvalidateAllTextures_() {
    std::lock_guard lock(texturesMutex_);
    for (auto& tex : textures_ | std::views::values) {
        if (tex.surface) {
            tex.surface->Release();
            tex.surface = nullptr;
        }
        tex.needsRecreation = true;
    }
}

bool ImGuiService::RegisterFont(uint32_t fontId, const char* filePath, float size) {
    if (!filePath || size <= 0.0f) {
        LOG_ERROR("ImGuiService::RegisterFont: invalid arguments (fontId={}, size={})", fontId, size);
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts) {
        LOG_ERROR("ImGuiService::RegisterFont: ImGui not initialized");
        return false;
    }

    std::lock_guard lock(fontsMutex_);

    // Check if font ID already registered
    if (fonts_.contains(fontId)) {
        LOG_WARN("ImGuiService::RegisterFont: font ID {} already registered", fontId);
        return false;
    }

    // Configure font with improved rendering
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = true;

    // Load the font
    ImFont* font = io.Fonts->AddFontFromFileTTF(filePath, size, &fontConfig);
    if (!font) {
        LOG_ERROR("ImGuiService::RegisterFont: failed to load font from '{}'", filePath);
        return false;
    }

    fonts_[fontId] = { fontId, font };
    LOG_INFO("ImGuiService::RegisterFont: registered font ID {} from '{}' (size={})", fontId, filePath, size);
    return true;
}

bool ImGuiService::UnregisterFont(uint32_t fontId) {
    std::lock_guard lock(fontsMutex_);

    if (!fonts_.contains(fontId)) {
        LOG_WARN("ImGuiService::UnregisterFont: font ID {} not found", fontId);
        return false;
    }

    fonts_.erase(fontId);
    LOG_INFO("ImGuiService::UnregisterFont: unregistered font ID {}", fontId);
    return true;
}

void* ImGuiService::GetFont(const uint32_t fontId) const {
    if (fontId == 0) {
        // Return default font
        return ImGui::GetIO().FontDefault;
    }

    std::lock_guard lock(fontsMutex_);
    auto it = fonts_.find(fontId);
    if (it == fonts_.end()) {
        return nullptr;
    }

    return it->second.font;
}

