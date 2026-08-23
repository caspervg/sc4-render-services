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
#include "GraphicsInterfaceHook.h"
#include "GZServPtrs.h"
#include "imgui_impl_dx7.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "public/ImGuiServiceIds.h"
#include "SCGLDX11Hook.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

namespace {
    std::atomic<ImGuiService*> g_instance{nullptr};
    std::atomic<DWORD> g_renderThreadId{0};

    bool IsDeviceLostResult_(const HRESULT hr) {
        switch (hr) {
        case DDERR_SURFACELOST:
        case DDERR_WRONGMODE:
        case DDERR_NOEXCLUSIVEMODE:
        case DDERR_EXCLUSIVEMODEALREADYSET:
            return true;
        default:
            return FAILED(hr);
        }
    }

    bool IsRenderThreadCallAllowed_(const char* operation, const bool allowBeforeRenderThreadKnown) {
        const DWORD threadId = GetCurrentThreadId();
        const DWORD renderThreadId = g_renderThreadId.load(std::memory_order_acquire);

        if (renderThreadId == 0) {
            if (allowBeforeRenderThreadKnown) {
                return true;
            }

            LOG_WARN("{}: render thread is not established yet", operation);
            return false;
        }

        if (renderThreadId != threadId) {
            LOG_ERROR("{}: called off render thread (tid={}, render_tid={})", operation, threadId, renderThreadId);
            return false;
        }

        return true;
    }

}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ImGuiService::ImGuiService()
    : cRZBaseSystemService(kImGuiServiceID, 0)
      , gameWindow_(nullptr)
      , originalWndProc_(nullptr)
      , lastKnownDevice_(nullptr)
      , lastKnownDDraw_(nullptr)
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
    if (!ppvObj) {
        return false;
    }

    if (riid == GZIID_cIGZImGuiService) {
        *ppvObj = static_cast<cIGZImGuiService*>(this);
        AddRef();
        return true;
    }

    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

void ImGuiService::SetInitSettings(const ImGuiInitSettings& settings) {
    initSettings_ = settings;
}

bool ImGuiService::Init() {
    const auto version = VersionDetection::GetInstance().GetGameVersion();
    if (version != 641) {
        LOG_WARN("ImGuiService: not initializing, game version {} != 641", version);
        return false;
    }

    if (initialized_) {
        return true;
    }

    Logger::Initialize("ImGuiService", "");
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

    initialized_ = false;

    std::vector<ImGuiPanelDesc> panelsToShutdown;
    {
        std::lock_guard panelLock(panelsMutex_);
        panelsToShutdown.reserve(panels_.size());
        for (const auto& panel : panels_) {
            panelsToShutdown.push_back(panel.desc);
        }
        panels_.clear();
    }
    for (const auto& desc : panelsToShutdown) {
        if (desc.on_shutdown) {
            desc.on_shutdown(desc.data);
        }
    }

    {
        std::lock_guard queueLock(renderQueueMutex_);
        for (auto& item : renderQueue_) {
            if (item.cleanup) {
                item.cleanup(item.data);
            }
        }
        renderQueue_.clear();
    }

    {
        std::lock_guard fontLock(fontsMutex_);
        fonts_.clear();
        pendingFontRegistrations_.clear();
        fontAtlasRebuildPending_ = false;
    }

    // Clean up all textures before shutting down ImGui
    {
        std::lock_guard textureLock(texturesMutex_);
        for (auto& texture : textures_ | std::views::values) {
            if (texture.surface) {
                texture.surface->Release();
                texture.surface = nullptr;
            }
            if (texture.d3d11View) {
                texture.d3d11View->Release();
                texture.d3d11View = nullptr;
            }
        }
        textures_.clear();
        pendingTextureReleaseIds_.clear();
    }

    RemoveWndProcHook_();
    if (backend_ == GraphicsBackend::D3D11) {
        SCGLDX11Hook::SetFrameCallback(nullptr);
        SCGLDX11Hook::Shutdown();
        if (d3d11BackendInitialized_) ImGui_ImplDX11_Shutdown();
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        if (d3d11Device_) d3d11Device_->Release();
        d3d11Device_ = nullptr;
        d3d11Context_ = nullptr;
        d3d11SwapChain_ = nullptr;
        d3d11RenderTarget_ = nullptr;
        d3d11BackendInitialized_ = false;
    } else {
        GraphicsInterfaceHook::SetFrameCallback(nullptr);
        GraphicsInterfaceHook::ShutdownImGui();
    }

    imguiInitialized_ = false;
    hookInstalled_ = false;
    deviceLost_ = false;
    lastKnownDevice_ = nullptr;
    lastKnownDDraw_ = nullptr;
    backend_ = GraphicsBackend::None;
    g_renderThreadId.store(0, std::memory_order_release);
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    SetServiceRunning(false);
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
    if (desc.id == 0) {
        LOG_WARN("ImGuiService: rejected panel {} (null id)", desc.id);
        return false;
    }
    if (!desc.on_render) {
        LOG_WARN("ImGuiService: rejected panel {} (null on_render)", desc.id);
        return false;
    }
    if (desc.fontId != 0) {
        std::lock_guard lock(fontsMutex_);
        if (!fonts_.contains(desc.fontId)) {
            LOG_WARN("ImGuiService: rejected panel {} (invalid font id {})", desc.id, desc.fontId);
            return false;
        }
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
    ImGuiPanelDesc desc{};
    {
        std::lock_guard lock(panelsMutex_);
        const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
            return entry.desc.id == panelId;
        });
        if (it == panels_.end()) {
            LOG_WARN("ImGuiService: unregister failed for panel {}", panelId);
            return false;
        }
        desc = it->desc;
        panels_.erase(it);
    }

    if (desc.on_unregister) {
        desc.on_unregister(desc.data);
    }
    LOG_INFO("ImGuiService: unregistered panel {}", panelId);
    return true;
}

bool ImGuiService::SetPanelVisible(const uint32_t panelId, const bool visible) {
    ImGuiPanelDesc desc{};
    {
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
        desc = it->desc;
    }

    if (desc.on_visible_changed) {
        desc.on_visible_changed(desc.data, visible);
    }
    return true;
}

bool ImGuiService::QueueRender(ImGuiRenderCallback callback, void* data, ImGuiRenderCleanup cleanup) {
    if (!callback) {
        return false;
    }

    std::lock_guard lock(renderQueueMutex_);
    renderQueue_.push_back(RenderQueueItem{callback, data, cleanup});
    return true;
}

bool ImGuiService::AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) {
    if (!outD3D || !outDD) {
        return false;
    }

    auto* d3dx = GraphicsInterfaceHook::GetD3DXInterface();
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

bool ImGuiService::AcquireD3D11Interfaces(ID3D11Device** outDevice,
                                           ID3D11DeviceContext** outContext,
                                           IDXGISwapChain** outSwapChain,
                                           ID3D11RenderTargetView** outRenderTarget) {
    if (!outDevice || !outContext || !outSwapChain || !outRenderTarget) return false;
    *outDevice = nullptr;
    *outContext = nullptr;
    *outSwapChain = nullptr;
    *outRenderTarget = nullptr;
    if (!IsRenderThreadCallAllowed_("ImGuiService::AcquireD3D11Interfaces", false) ||
        backend_ != GraphicsBackend::D3D11 || !d3d11Device_ || !d3d11Context_ ||
        !d3d11SwapChain_ || !d3d11RenderTarget_) return false;

    d3d11Device_->AddRef();
    d3d11Context_->AddRef();
    d3d11SwapChain_->AddRef();
    d3d11RenderTarget_->AddRef();
    *outDevice = d3d11Device_;
    *outContext = d3d11Context_;
    *outSwapChain = d3d11SwapChain_;
    *outRenderTarget = d3d11RenderTarget_;
    return true;
}

bool ImGuiService::IsDeviceReady() const {
    if (!imguiInitialized_) {
        return false;
    }
    if (backend_ == GraphicsBackend::D3D11) return d3d11BackendInitialized_ && d3d11Device_;
    auto* d3dx = GraphicsInterfaceHook::GetD3DXInterface();
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

void ImGuiService::RenderFrameD3D11Thunk_(SCGLD3D11FrameContext const* frame) {
    auto* instance = g_instance.load(std::memory_order_acquire);
    if (instance) instance->RenderFrameD3D11_(frame);
}

void ImGuiService::RenderFrameD3D11_(SCGLD3D11FrameContext const* frame) {
    if (!initialized_ || frame == nullptr || frame->structSize < sizeof(*frame) || frame->apiVersion != 1) return;
    if (frame->event == SCGL_D3D11_EVENT_BEFORE_DEVICE_DESTROY) {
        if (backend_ == GraphicsBackend::D3D11 && d3d11BackendInitialized_) {
            OnDeviceLost_();
            ImGui_ImplDX11_Shutdown();
            d3d11BackendInitialized_ = false;
            imguiInitialized_ = false;
        }
        if (d3d11Device_) d3d11Device_->Release();
        d3d11Device_ = nullptr;
        d3d11Context_ = nullptr;
        d3d11SwapChain_ = nullptr;
        d3d11RenderTarget_ = nullptr;
        return;
    }
    if (frame->event != SCGL_D3D11_EVENT_RENDER || !frame->device || !frame->context || !frame->window) return;

    DWORD const threadId = GetCurrentThreadId();
    g_renderThreadId.store(threadId, std::memory_order_release);
    backend_ = GraphicsBackend::D3D11;
    if (!d3d11BackendInitialized_) {
        bool const firstInitialization = ImGui::GetCurrentContext() == nullptr;
        bool initialized = firstInitialization
            ? GraphicsInterfaceHook::InitializeImGuiD3D11(frame->window, frame->device, frame->context, initSettings_)
            : ImGui_ImplDX11_Init(frame->device, frame->context);
        if (!initialized) {
            LOG_ERROR("ImGuiService: failed to initialize the official ImGui DX11 backend");
            return;
        }
        d3d11BackendInitialized_ = true;
        imguiInitialized_ = true;
        if (d3d11Device_ != frame->device) {
            if (d3d11Device_) d3d11Device_->Release();
            d3d11Device_ = frame->device;
            d3d11Device_->AddRef();
        }
        if (firstInitialization && !InstallWndProcHook_(frame->window)) {
            LOG_WARN("ImGuiService: failed to install WndProc hook for DX11");
        }
        if (deviceLost_) OnDeviceRestored_();
        deviceGeneration_.store(frame->deviceGeneration, std::memory_order_release);
        LOG_INFO("ImGuiService: ImGui DX11 backend ready (generation={})", frame->deviceGeneration);
    }

    d3d11Context_ = frame->context;
    d3d11SwapChain_ = frame->swapChain;
    d3d11RenderTarget_ = frame->renderTargetView;
    ProcessPendingTextureReleases_();
    InitializePanels_();
    ProcessPendingFontRegistrations_();
    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    RenderFrameContents_();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    d3d11Context_ = nullptr;
    d3d11SwapChain_ = nullptr;
    d3d11RenderTarget_ = nullptr;
}

void ImGuiService::RenderFrameContents_() {
    std::vector<ImGuiPanelDesc> panelsToUpdate;
    std::vector<ImGuiPanelDesc> panelsToRender;
    {
        std::lock_guard panelLock(panelsMutex_);
        for (auto const& panel : panels_) {
            if (!panel.desc.visible) continue;
            if (panel.desc.on_update) panelsToUpdate.push_back(panel.desc);
            if (panel.desc.on_render) panelsToRender.push_back(panel.desc);
        }
    }
    for (auto const& desc : panelsToUpdate) desc.on_update(desc.data);
    for (auto const& desc : panelsToRender) {
        ImFont* font = desc.fontId == 0 ? nullptr : static_cast<ImFont*>(GetFont(desc.fontId));
        if (font) ImGui::PushFont(font, 0.0f);
        desc.on_render(desc.data);
        if (font) ImGui::PopFont();
    }
    std::vector<RenderQueueItem> renderQueue;
    {
        std::lock_guard queueLock(renderQueueMutex_);
        renderQueue.swap(renderQueue_);
    }
    for (auto& item : renderQueue) {
        item.callback(item.data);
        if (item.cleanup) item.cleanup(item.data);
    }
    ImGui::EndFrame();
    ImGui::Render();
}

void ImGuiService::RenderFrame_(IDirect3DDevice7* device) {
    static auto loggedFirstRender = false;
    const DWORD threadId = GetCurrentThreadId();
    const DWORD prevThreadId = g_renderThreadId.load(std::memory_order_acquire);
    if (prevThreadId == 0) {
        g_renderThreadId.store(threadId, std::memory_order_release);
        LOG_DEBUG("ImGuiService::RenderFrame_: render thread id set to {}", threadId);
    }
    else if (prevThreadId != threadId) {
        LOG_WARN("ImGuiService::RenderFrame_: render thread id changed ({} -> {})",
                 prevThreadId, threadId);
        g_renderThreadId.store(threadId, std::memory_order_release);
    }
    if (!imguiInitialized_) {
        return;
    }

    {
        std::lock_guard lock(panelsMutex_);
        if (!initialized_) {
            return;
        }
    }

    auto* d3dx = GraphicsInterfaceHook::GetD3DXInterface();
    if (!d3dx || device != d3dx->GetD3DDevice()) {
        return;
    }
    auto* dd = d3dx->GetDD();
    if (dd) {
        HRESULT hr = dd->TestCooperativeLevel();
        if (IsDeviceLostResult_(hr)) {
            if (!deviceLost_) {
                OnDeviceLost_();
            }
            return;
        }
    } else {
        return;
    }

    const bool interfacesChanged = lastKnownDevice_ != device || lastKnownDDraw_ != dd;
    const bool hadKnownInterfaces = lastKnownDevice_ != nullptr || lastKnownDDraw_ != nullptr;
    if (interfacesChanged) {
        if (hadKnownInterfaces) {
            LOG_WARN("ImGuiService::RenderFrame_: DX7 interfaces changed (device {} -> {}, dd {} -> {})",
                     static_cast<void*>(lastKnownDevice_),
                     static_cast<void*>(device),
                     static_cast<void*>(lastKnownDDraw_),
                     static_cast<void*>(dd));
            if (!deviceLost_) {
                OnDeviceLost_();
            }
        }

        lastKnownDevice_ = device;
        lastKnownDDraw_ = dd;

        if (!ImGui_ImplDX7_UpdateDevice(device, dd)) {
            LOG_ERROR("ImGuiService::RenderFrame_: failed to update ImGui DX7 backend interfaces");
            return;
        }

        GraphicsInterfaceHook::InstallSceneHooks();
    }

    if (deviceLost_) {
        if (!OnDeviceRestored_()) {
            return;
        }
    }

    if (!ImGui::GetCurrentContext()) {
        return;
    }

    // Delay texture destruction until the next frame so draw commands emitted
    // earlier in the frame never reference a released DDraw surface.
    ProcessPendingTextureReleases_();

    InitializePanels_();
    ProcessPendingFontRegistrations_();

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX7_NewFrame();
    ImGui::NewFrame();

    std::vector<ImGuiPanelDesc> panelsToUpdate;
    std::vector<ImGuiPanelDesc> panelsToRender;
    {
        std::lock_guard panelLock(panelsMutex_);
        panelsToUpdate.reserve(panels_.size());
        panelsToRender.reserve(panels_.size());
        for (const auto& panel : panels_) {
            if (!panel.desc.visible) {
                continue;
            }
            if (panel.desc.on_update) {
                panelsToUpdate.push_back(panel.desc);
            }
            if (panel.desc.on_render) {
                panelsToRender.push_back(panel.desc);
            }
        }
    }

    for (const auto& desc : panelsToUpdate) {
        desc.on_update(desc.data);
    }

    for (const auto& desc : panelsToRender) {
        bool pushedFont = false;
        if (desc.fontId != 0) {
            if (auto* font = static_cast<ImFont*>(GetFont(desc.fontId))) {
                ImGui::PushFont(font, 0.0f);
                pushedFont = true;
            }
        }

        desc.on_render(desc.data);

        if (pushedFont) {
            ImGui::PopFont();
        }
    }

    {
        std::vector<RenderQueueItem> renderQueue;
        {
            std::lock_guard queueLock(renderQueueMutex_);
            renderQueue.swap(renderQueue_);
        }

        for (auto& item : renderQueue) {
            item.callback(item.data);
            if (item.cleanup) {
                item.cleanup(item.data);
            }
        }
    }

    ImGui::EndFrame();
    ImGui::Render();

    const HRESULT preRenderHr = dd->TestCooperativeLevel();
    if (IsDeviceLostResult_(preRenderHr)) {
        if (!deviceLost_) {
            OnDeviceLost_();
        }
        return;
    }

    ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());

    if (!loggedFirstRender) {
        LOG_INFO("ImGuiService: rendered first frame with {} panel(s)", panels_.size());
        loggedFirstRender = true;
    }
}

bool ImGuiService::EnsureInitialized_() {
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

    if (pDriver->GetGZCLSID() == 0xc4554841) {
        backend_ = GraphicsBackend::D3D11;
        SCGLDX11Hook::SetFrameCallback(&ImGuiService::RenderFrameD3D11Thunk_);
        if (!SCGLDX11Hook::CaptureInterface(pDriver)) {
            if (!warnedNoDriver_) LOG_WARN("ImGuiService: SCGL D3D11 frame service is not ready");
            warnedNoDriver_ = true;
            return false;
        }
        warnedNoDriver_ = false;
        return imguiInitialized_;
    }

    if (imguiInitialized_) return GraphicsInterfaceHook::CaptureInterface(pDriver) && IsDeviceReady();

    if (pDriver->GetGZCLSID() != kSCGDriverDirectX) {
        if (!warnedNoDriver_) {
            LOG_WARN("ImGuiService: not a DirectX driver, skipping initialization");
            warnedNoDriver_ = true;
        }
        return false;
    }

    if (!GraphicsInterfaceHook::CaptureInterface(pDriver)) {
        LOG_ERROR("ImGuiService: failed to capture D3DX interface");
        return false;
    }
    auto* d3dx = GraphicsInterfaceHook::GetD3DXInterface();
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

    if (!GraphicsInterfaceHook::InitializeImGui(hwnd, initSettings_)) {
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
    GraphicsInterfaceHook::SetFrameCallback(&ImGuiService::RenderFrameThunk_);
    GraphicsInterfaceHook::InstallSceneHooks();
    LOG_INFO("ImGuiService: ImGui initialized and scene hooks installed");
    return true;
}

void ImGuiService::InitializePanels_() {
    if (!imguiInitialized_) {
        return;
    }

    std::vector<ImGuiPanelDesc> panelsToInit;
    {
        std::lock_guard lock(panelsMutex_);
        panelsToInit.reserve(panels_.size());
        for (auto& panel : panels_) {
            if (!panel.initialized) {
                panel.initialized = true;
                if (panel.desc.on_init) {
                    panelsToInit.push_back(panel.desc);
                }
            }
        }
    }

    for (const auto& desc : panelsToInit) {
        desc.on_init(desc.data);
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
        auto currentProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow_, GWLP_WNDPROC));
        if (currentProc == &ImGuiService::WndProcHook) {
            SetWindowLongPtrW(gameWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWndProc_));
        } else {
            LOG_WARN("ImGuiService: WndProc was re-hooked by another module, skipping restoration to preserve chain");
        }
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
    const DWORD renderThreadId = g_renderThreadId.load(std::memory_order_acquire);
    const bool renderThreadKnown = renderThreadId != 0;
    if (!IsRenderThreadCallAllowed_("ImGuiService::CreateTexture", true) && renderThreadKnown) {
        return ImGuiTextureHandle{0, 0};
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
    tex.width = desc.width;
    tex.height = desc.height;
    tex.creationGeneration = currentGen;
    tex.useSystemMemory = desc.useSystemMemory;
    tex.surface = nullptr;
    tex.needsRecreation = false;
    tex.pendingDestroy = false;

    // Store source pixel data for recreation after device loss
    const size_t dataSize = pixelCount * 4; // RGBA32
    tex.sourceData.resize(dataSize);
    std::memcpy(tex.sourceData.data(), desc.pixels, dataSize);

    {
        std::lock_guard lock(texturesMutex_);
        tex.id = nextTextureId_++;
        if (tex.id == 0) {
            LOG_ERROR("ImGuiService::CreateTexture: texture ID space exhausted");
            return ImGuiTextureHandle{0, 0};
        }
    }

    // Only the render thread may talk to D3D. Before the render thread is known,
    // create a deferred entry and let GetTextureID() realize it later.
    if (!renderThreadKnown) {
        tex.needsRecreation = true;
        LOG_WARN("ImGuiService::CreateTexture: render thread not established yet, deferring surface creation (id={})", tex.id);
    }
    else if (IsDeviceReady() && !deviceLost_) {
        if (!CreateSurfaceForTexture_(tex)) {
            LOG_WARN("ImGuiService::CreateTexture: surface creation failed, will retry later (id={})", tex.id);
            tex.needsRecreation = true;
        }
    }
    else {
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

void* ImGuiService::GetTextureID(const ImGuiTextureHandle handle) {
    if (!IsRenderThreadCallAllowed_("ImGuiService::GetTextureID", false)) {
        return nullptr;
    }

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
    if (tex.pendingDestroy) {
        return nullptr;
    }

    // Recreate surface if needed
    if (tex.needsRecreation || (backend_ == GraphicsBackend::D3D11 ? !tex.d3d11View : !tex.surface)) {
        if (!CreateSurfaceForTexture_(tex)) {
            LOG_WARN("ImGuiService::GetTextureID: failed to recreate surface (id={})", tex.id);
            return nullptr;
        }
    }

    if (backend_ == GraphicsBackend::D3D11) return static_cast<void*>(tex.d3d11View);

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
    if (!IsRenderThreadCallAllowed_("ImGuiService::ReleaseTexture", false)) {
        return;
    }

    std::lock_guard lock(texturesMutex_);

    auto it = textures_.find(handle.id);
    if (it != textures_.end()) {
        ManagedTexture& tex = it->second;
        if (!tex.pendingDestroy) {
            tex.pendingDestroy = true;
            tex.needsRecreation = false;
            pendingTextureReleaseIds_.push_back(handle.id);
        }
    }

    LOG_INFO("ImGuiService::ReleaseTexture: queued texture release (id={})", handle.id);
}

bool ImGuiService::IsTextureValid(const ImGuiTextureHandle handle) const {
    if (!IsRenderThreadCallAllowed_("ImGuiService::IsTextureValid", false)) {
        return false;
    }

    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    if (handle.generation != currentGen) {
        return false;
    }

    if (deviceLost_) {
        return false;
    }

    std::lock_guard lock(texturesMutex_);
    auto it = textures_.find(handle.id);
    return it != textures_.end() && !it->second.pendingDestroy;
}

void ImGuiService::ProcessPendingTextureReleases_() {
    std::vector<uint32_t> pendingIds;
    {
        std::lock_guard lock(texturesMutex_);
        if (pendingTextureReleaseIds_.empty()) {
            return;
        }
        pendingIds.swap(pendingTextureReleaseIds_);
    }

    std::lock_guard lock(texturesMutex_);
    for (const uint32_t textureId : pendingIds) {
        auto it = textures_.find(textureId);
        if (it == textures_.end() || !it->second.pendingDestroy) {
            continue;
        }

        if (it->second.surface) {
            it->second.surface->Release();
            it->second.surface = nullptr;
        }
        if (it->second.d3d11View) {
            it->second.d3d11View->Release();
            it->second.d3d11View = nullptr;
        }
        textures_.erase(it);
        LOG_INFO("ImGuiService::ProcessPendingTextureReleases_: released texture (id={})", textureId);
    }
}

bool ImGuiService::RegisterFont(uint32_t fontId, const char* filePath, float sizePixels) {
    if (!filePath || filePath[0] == '\0' || sizePixels <= 0.0f) {
        LOG_ERROR("ImGuiService::RegisterFont: invalid arguments (fontId={}, size={})", fontId, sizePixels);
        return false;
    }

    if (GetFileAttributesA(filePath) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR("ImGuiService::RegisterFont: file not found '{}'", filePath);
        return false;
    }

    if (!ImGui::GetCurrentContext()) {
        LOG_ERROR("ImGuiService::RegisterFont: ImGui not initialized");
        return false;
    }

    std::lock_guard lock(fontsMutex_);

    // Check if font ID already registered
    if (fonts_.contains(fontId)) {
        LOG_WARN("ImGuiService::RegisterFont: font ID {} already registered", fontId);
        return false;
    }

    pendingFontRegistrations_.push_back(PendingFontRegistration{
        .id = fontId,
        .sizePixels = sizePixels,
        .filePath = filePath,
        .compressedData = {}});
    fonts_[fontId] = {fontId, nullptr};
    fontAtlasRebuildPending_ = true;

    LOG_INFO("ImGuiService::RegisterFont: queued font ID {} from '{}' (size={})", fontId, filePath, sizePixels);
    return true;
}

bool ImGuiService::RegisterFont(uint32_t fontId, const void* compressedFontData, const int compressedFontDataSize,
                                float sizePixels) {
    if (!compressedFontData || compressedFontDataSize <= 0 || sizePixels <= 0.0f) {
        LOG_ERROR("ImGuiService::RegisterFont: invalid arguments (fontId={}, compressedFontDataSize={}, size={})",
                  fontId, compressedFontDataSize, sizePixels);
        return false;
    }

    if (!ImGui::GetCurrentContext()) {
        LOG_ERROR("ImGuiService::RegisterFont: ImGui not initialized");
        return false;
    }

    std::lock_guard lock(fontsMutex_);

    // Check if font ID already registered
    if (fonts_.contains(fontId)) {
        LOG_WARN("ImGuiService::RegisterFont: font ID {} already registered", fontId);
        return false;
    }

    PendingFontRegistration pendingRegistration{
        .id = fontId,
        .sizePixels = sizePixels,
        .filePath = {},
        .compressedData = std::vector<uint8_t>(compressedFontDataSize)};
    std::memcpy(pendingRegistration.compressedData.data(), compressedFontData, static_cast<size_t>(compressedFontDataSize));

    pendingFontRegistrations_.push_back(std::move(pendingRegistration));
    fonts_[fontId] = {fontId, nullptr};
    fontAtlasRebuildPending_ = true;

    LOG_INFO("ImGuiService::RegisterFont: queued font ID {} from compressed data (size={})", fontId, sizePixels);
    return true;
}


bool ImGuiService::UnregisterFont(uint32_t fontId) {
    std::lock_guard lock(fontsMutex_);

    if (!fonts_.contains(fontId)) {
        LOG_WARN("ImGuiService::UnregisterFont: font ID {} not found", fontId);
        return false;
    }

    fonts_.erase(fontId);
    pendingFontRegistrations_.erase(
        std::remove_if(
            pendingFontRegistrations_.begin(),
            pendingFontRegistrations_.end(),
            [fontId](const PendingFontRegistration& pending) {
                return pending.id == fontId;
            }),
        pendingFontRegistrations_.end());
    LOG_INFO("ImGuiService::UnregisterFont: unregistered font ID {}", fontId);
    return true;
}

void* ImGuiService::GetFont(const uint32_t fontId) const {
    if (!ImGui::GetCurrentContext()) {
        return nullptr;
    }

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

void ImGuiService::ProcessPendingFontRegistrations_() {
    if (!imguiInitialized_ || !ImGui::GetCurrentContext()) {
        return;
    }

    std::vector<PendingFontRegistration> pendingRegistrations;
    bool rebuildRequested = false;
    {
        std::lock_guard lock(fontsMutex_);
        pendingRegistrations.swap(pendingFontRegistrations_);
        rebuildRequested = fontAtlasRebuildPending_;
        fontAtlasRebuildPending_ = false;
    }

    if (pendingRegistrations.empty() && !rebuildRequested) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts) {
        std::lock_guard lock(fontsMutex_);
        fontAtlasRebuildPending_ = true;
        pendingFontRegistrations_.insert(
            pendingFontRegistrations_.begin(),
            std::make_move_iterator(pendingRegistrations.begin()),
            std::make_move_iterator(pendingRegistrations.end()));
        return;
    }

    std::vector<std::pair<uint32_t, ImFont*>> appliedFonts;
    std::vector<uint32_t> failedFontIds;
    appliedFonts.reserve(pendingRegistrations.size());
    failedFontIds.reserve(pendingRegistrations.size());

    for (const PendingFontRegistration& pending : pendingRegistrations) {
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 2;
        fontConfig.PixelSnapH = true;
        fontConfig.GlyphExtraAdvanceX = 1.0f;

        ImFont* font = nullptr;
        if (!pending.filePath.empty()) {
            font = io.Fonts->AddFontFromFileTTF(pending.filePath.c_str(), pending.sizePixels, &fontConfig);
            if (!font) {
                LOG_ERROR("ImGuiService::RegisterFont: failed to load font from '{}'", pending.filePath);
            }
        } else {
            font = io.Fonts->AddFontFromMemoryCompressedTTF(
                pending.compressedData.data(),
                static_cast<int>(pending.compressedData.size()),
                pending.sizePixels,
                &fontConfig);
            if (!font) {
                LOG_ERROR(
                    "ImGuiService::RegisterFont: failed to load font from compressed data (fontDataSize={})",
                    pending.compressedData.size());
            }
        }

        if (!font) {
            failedFontIds.push_back(pending.id);
            continue;
        }

        appliedFonts.emplace_back(pending.id, font);
        rebuildRequested = true;
    }

    {
        std::lock_guard lock(fontsMutex_);
        for (const auto& [id, font] : appliedFonts) {
            auto it = fonts_.find(id);
            if (it != fonts_.end()) {
                it->second.font = font;
            }
        }
        for (const uint32_t id : failedFontIds) {
            auto it = fonts_.find(id);
            if (it != fonts_.end() && it->second.font == nullptr) {
                fonts_.erase(it);
            }
        }
    }

    if (rebuildRequested && !RebuildFontAtlas_()) {
        std::lock_guard lock(fontsMutex_);
        fontAtlasRebuildPending_ = true;
        LOG_ERROR("ImGuiService::ProcessPendingFontRegistrations_: failed to rebuild font atlas texture");
        return;
    }

    for (const auto& [id, _] : appliedFonts) {
        LOG_INFO("ImGuiService::RegisterFont: registered font ID {}", id);
    }
}

bool ImGuiService::RebuildFontAtlas_() {
    if (!imguiInitialized_ || !ImGui::GetCurrentContext()) {
        return false;
    }

    if (backend_ == GraphicsBackend::D3D11) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        if (!ImGui_ImplDX11_CreateDeviceObjects()) return false;
    } else {
        ImGui_ImplDX7_InvalidateDeviceObjects();
        if (!ImGui_ImplDX7_CreateDeviceObjects()) return false;
    }

    LOG_INFO("ImGuiService::RebuildFontAtlas_: rebuilt font atlas texture");
    return true;
}

bool ImGuiService::CreateSurfaceForTexture_(ManagedTexture& tex) {
    if (backend_ == GraphicsBackend::D3D11) return CreateD3D11Texture_(tex);
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
    }
    else {
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
        LOG_WARN(
            "ImGuiService::CreateSurfaceForTexture_: video memory exhausted, falling back to system memory (id={})",
            tex.id);
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
    hr = surface->Lock(nullptr, &lockDesc, DDLOCK_WRITEONLY, nullptr);
    if (hr == DDERR_SURFACELOST) {
        LOG_WARN("ImGuiService::CreateSurfaceForTexture_: Surface lost during lock (id={})", tex.id);
        surface->Release();
        tex.needsRecreation = true;
        return false;
    }
    if (FAILED(hr)) {
        LOG_ERROR("ImGuiService::CreateSurfaceForTexture_: Lock failed (hr=0x{:08X}, id={})", hr, tex.id);
        surface->Release();
        return false;
    }

    // Copy pixel data row-by-row (respecting lPitch)
    const auto* srcPixels = reinterpret_cast<const uint8_t*>(tex.sourceData.data());
    auto* dstPixels = static_cast<uint8_t*>(lockDesc.lpSurface);
    const uint32_t srcPitch = tex.width * 4; // RGBA32
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

bool ImGuiService::CreateD3D11Texture_(ManagedTexture& tex) {
    if (!d3d11Device_ || tex.sourceData.empty()) return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = tex.width;
    description.Height = tex.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initialData{ tex.sourceData.data(), tex.width * 4, 0 };
    ID3D11Texture2D* texture = nullptr;
    HRESULT result = d3d11Device_->CreateTexture2D(&description, &initialData, &texture);
    if (FAILED(result)) {
        LOG_ERROR("ImGuiService::CreateD3D11Texture_: CreateTexture2D failed (hr=0x{:08X}, id={})", result, tex.id);
        return false;
    }
    ID3D11ShaderResourceView* view = nullptr;
    result = d3d11Device_->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    if (FAILED(result)) {
        LOG_ERROR("ImGuiService::CreateD3D11Texture_: CreateShaderResourceView failed (hr=0x{:08X}, id={})", result, tex.id);
        return false;
    }
    if (tex.d3d11View) tex.d3d11View->Release();
    tex.d3d11View = view;
    tex.needsRecreation = false;
    tex.creationGeneration = deviceGeneration_.load(std::memory_order_acquire);
    return true;
}

void ImGuiService::OnDeviceLost_() {
    deviceLost_ = true;

    std::vector<ImGuiPanelDesc> panelsToNotify;
    {
        std::lock_guard lock(panelsMutex_);
        panelsToNotify.reserve(panels_.size());
        for (const auto& panel : panels_) {
            if (panel.desc.on_device_lost) {
                panelsToNotify.push_back(panel.desc);
            }
        }
    }

    for (const auto& desc : panelsToNotify) {
        desc.on_device_lost(desc.data);
    }

    if (backend_ == GraphicsBackend::D3D11) ImGui_ImplDX11_InvalidateDeviceObjects();
    else ImGui_ImplDX7_InvalidateDeviceObjects();

    InvalidateAllTextures_();

    LOG_WARN("ImGuiService::OnDeviceLost_: device lost, notified panels and invalidated textures");
}

bool ImGuiService::OnDeviceRestored_() {
    bool const created = backend_ == GraphicsBackend::D3D11
        ? ImGui_ImplDX11_CreateDeviceObjects()
        : ImGui_ImplDX7_CreateDeviceObjects();
    if (!created) {
        LOG_WARN("ImGuiService::OnDeviceRestored_: failed to recreate backend device objects");
        return false;
    }

    // Increment device generation to invalidate old handles
    uint32_t newGen = deviceGeneration_.fetch_add(1, std::memory_order_release) + 1;
    deviceLost_ = false;

    std::vector<ImGuiPanelDesc> panelsToNotify;
    {
        std::lock_guard lock(panelsMutex_);
        panelsToNotify.reserve(panels_.size());
        for (const auto& panel : panels_) {
            if (panel.desc.on_device_restored) {
                panelsToNotify.push_back(panel.desc);
            }
        }
    }

    for (const auto& desc : panelsToNotify) {
        desc.on_device_restored(desc.data);
    }

    LOG_INFO("ImGuiService::OnDeviceRestored_: device restored (new gen={}) and notified panels", newGen);
    return true;
}

void ImGuiService::InvalidateAllTextures_() {
    std::lock_guard lock(texturesMutex_);
    for (auto& tex : textures_ | std::views::values) {
        if (tex.surface) {
            tex.surface->Release();
            tex.surface = nullptr;
        }
        if (tex.d3d11View) {
            tex.d3d11View->Release();
            tex.d3d11View = nullptr;
        }
        tex.needsRecreation = true;
    }
}
