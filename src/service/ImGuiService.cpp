// ReSharper disable CppDFAUnreachableCode
// ReSharper disable CppDFAConstantConditions
#include "ImGuiService.h"

#include <algorithm>

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
    ImGuiService* g_instance = nullptr;
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
    , deviceGeneration_(0) {}

ImGuiService::~ImGuiService() {
    if (g_instance == this) {
        g_instance = nullptr;
    }
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
    g_instance = this;
    return true;
}

bool ImGuiService::Shutdown() {
    if (!initialized_) {
        return true;
    }

    for (const auto& panel : panels_) {
        if (panel.desc.on_shutdown) {
            panel.desc.on_shutdown(panel.desc.data);
        }
    }
    panels_.clear();

    RemoveWndProcHook_();
    DX7InterfaceHook::SetFrameCallback(nullptr);
    DX7InterfaceHook::ShutdownImGui();

    imguiInitialized_ = false;
    hookInstalled_ = false;
    deviceGeneration_ += 1;
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

    const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
        return entry.desc.id == desc.id;
    });
    if (it != panels_.end()) {
        LOG_WARN("ImGuiService: rejected panel {} (duplicate id)", desc.id);
        return false;
    }

    panels_.push_back(PanelEntry{desc, false});
    if (imguiInitialized_) {
        InitializePanels_();
    }
    SortPanels_();
    LOG_INFO("ImGuiService: registered panel {} (order={})", desc.id, desc.order);
    return true;
}

bool ImGuiService::UnregisterPanel(uint32_t panelId) {
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
    if (!outD3D || !outDD || !DX7InterfaceHook::s_pD3DX) {
        return false;
    }

    auto* d3d = DX7InterfaceHook::s_pD3DX->GetD3DDevice();
    auto* dd = DX7InterfaceHook::s_pD3DX->GetDD();
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
    return imguiInitialized_ && DX7InterfaceHook::s_pD3DX && DX7InterfaceHook::s_pD3DX->GetD3DDevice() &&
        DX7InterfaceHook::s_pD3DX->GetDD();
}

uint32_t ImGuiService::GetDeviceGeneration() const {
    return deviceGeneration_;
}

void ImGuiService::RenderFrameThunk_(IDirect3DDevice7* device) {
    if (g_instance) {
        g_instance->RenderFrame_(device);
    }
}

void ImGuiService::RenderFrame_(IDirect3DDevice7* device) {
    static auto loggedFirstRender = false;
    if (!imguiInitialized_ || panels_.empty()) {
        return;
    }
    if (!DX7InterfaceHook::s_pD3DX || device != DX7InterfaceHook::s_pD3DX->GetD3DDevice()) {
        return;
    }
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    InitializePanels_();

    auto* dd = DX7InterfaceHook::s_pD3DX->GetDD();
    if (!dd || dd->TestCooperativeLevel() != S_OK) {
        return;
    }

    ImGui_ImplDX7_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    for (auto& panel : panels_) {
        if (panel.desc.visible && panel.desc.on_update) {
            panel.desc.on_update(panel.desc.data);
        }
    }

    for (auto& panel : panels_) {
        if (panel.desc.visible && panel.desc.on_render) {
            panel.desc.on_render(panel.desc.data);
        }
    }

    // Reset texture coordinate generation/transform state that can be left
    // dirty by the game and cause garbled font sampling by the ImGui backend.
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);

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
    deviceGeneration_ += 1;
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

    if (!g_instance || !g_instance->originalWndProc_) {
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_instance->originalWndProc_, hWnd, msg, wParam, lParam);
}
