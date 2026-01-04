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
#include "utils/Logger.h"

namespace {
    ImGuiService* g_instance = nullptr;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ImGuiService::ImGuiService()
    : cRZBaseSystemService(kImGuiServiceID, 0),
      gameWindow(nullptr),
      originalWndProc(nullptr),
      initialized(false),
      imguiInitialized(false),
      hookInstalled(false),
      warnedNoDriver(false),
      warnedMissingWindow(false)
{
}

ImGuiService::~ImGuiService()
{
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

uint32_t ImGuiService::AddRef()
{
    return cRZBaseSystemService::AddRef();
}

uint32_t ImGuiService::Release()
{
    return cRZBaseSystemService::Release();
}

bool ImGuiService::QueryInterface(uint32_t riid, void** ppvObj)
{
    if (riid == GZIID_cIGZImGuiService) {
        *ppvObj = static_cast<cIGZImGuiService*>(this);
        AddRef();
        return true;
    }

    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

bool ImGuiService::Init()
{
    if (initialized) {
        return true;
    }

    Logger::Initialize("SC4ImGuiService", "");
    LOG_INFO("ImGuiService: initialized");
    SetServiceRunning(true);
    initialized = true;
    g_instance = this;
    return true;
}

bool ImGuiService::Shutdown()
{
    if (!initialized) {
        return true;
    }

    for (const auto& panel : panels) {
        if (panel.desc.on_shutdown) {
            panel.desc.on_shutdown(panel.desc.data);
        }
    }
    panels.clear();

    RemoveWndProcHook();
    DX7InterfaceHook::SetFrameCallback(nullptr);
    DX7InterfaceHook::ShutdownImGui();

    imguiInitialized = false;
    hookInstalled = false;
    SetServiceRunning(false);
    initialized = false;
    return true;
}

bool ImGuiService::OnTick(uint32_t)
{
    if (!initialized) {
        return true;
    }

    EnsureInitialized();
    return true;
}

bool ImGuiService::OnIdle(uint32_t)
{
    return OnTick(0);
}

uint32_t ImGuiService::GetServiceID() const
{
    return serviceID;
}

uint32_t ImGuiService::GetApiVersion() const
{
    return kImGuiServiceApiVersion;
}

void* ImGuiService::GetContext() const
{
    return ImGui::GetCurrentContext();
}

bool ImGuiService::RegisterPanel(const ImGuiPanelDesc& desc)
{
    if (!desc.render) {
        LOG_WARN("ImGuiService: rejected panel {} (null render)", desc.panel_id);
        return false;
    }

    auto it = std::find_if(panels.begin(), panels.end(), [&](const PanelEntry& entry) {
        return entry.desc.panel_id == desc.panel_id;
    });
    if (it != panels.end()) {
        LOG_WARN("ImGuiService: rejected panel {} (duplicate id)", desc.panel_id);
        return false;
    }

    panels.push_back(PanelEntry{desc});
    SortPanels();
    LOG_INFO("ImGuiService: registered panel {} (order={})", desc.panel_id, desc.order);
    return true;
}

bool ImGuiService::UnregisterPanel(uint32_t panel_id)
{
    auto it = std::find_if(panels.begin(), panels.end(), [&](const PanelEntry& entry) {
        return entry.desc.panel_id == panel_id;
    });
    if (it == panels.end()) {
        LOG_WARN("ImGuiService: unregister failed for panel {}", panel_id);
        return false;
    }

    if (it->desc.on_shutdown) {
        it->desc.on_shutdown(it->desc.data);
    }

    panels.erase(it);
    LOG_INFO("ImGuiService: unregistered panel {}", panel_id);
    return true;
}

bool ImGuiService::SetPanelVisible(uint32_t panel_id, bool visible)
{
    auto it = std::find_if(panels.begin(), panels.end(), [&](const PanelEntry& entry) {
        return entry.desc.panel_id == panel_id;
    });
    if (it == panels.end()) {
        return false;
    }

    it->desc.visible = visible;
    return true;
}

void ImGuiService::RenderFrameThunk(IDirect3DDevice7* device)
{
    if (g_instance) {
        g_instance->RenderFrame(device);
    }
}

void ImGuiService::RenderFrame(IDirect3DDevice7* device) {
    static bool loggedFirstRender = false;
    if (!imguiInitialized || panels.empty()) {
        return;
    }
    if (!DX7InterfaceHook::s_pD3DX || device != DX7InterfaceHook::s_pD3DX->GetD3DDevice()) {
        return;
    }
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    auto* dd = DX7InterfaceHook::s_pD3DX->GetDD();
    if (!dd || dd->TestCooperativeLevel() != S_OK) {
        return;
    }

    ImGui_ImplDX7_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    for (const auto& panel : panels) {
        if (panel.desc.visible && panel.desc.render) {
            panel.desc.render(panel.desc.data);
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
        LOG_INFO("ImGuiService: rendered first frame with {} panel(s)", panels.size());
        loggedFirstRender = true;
    }
}

bool ImGuiService::EnsureInitialized()
{
    if (imguiInitialized) {
        return true;
    }

    cIGZGraphicSystem2Ptr pGS2;
    if (!pGS2) {
        return false;
    }

    cIGZGDriver* pDriver = pGS2->GetGDriver();
    if (!pDriver) {
        if (!warnedNoDriver) {
            LOG_WARN("ImGuiService: graphics driver not available yet");
            warnedNoDriver = true;
        }
        return false;
    }

    if (pDriver->GetGZCLSID() != kSCGDriverDirectX) {
        if (!warnedNoDriver) {
            LOG_WARN("ImGuiService: not a DirectX driver, skipping initialization");
            warnedNoDriver = true;
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
        if (!warnedMissingWindow) {
            LOG_WARN("ImGuiService: game window not ready yet");
            warnedMissingWindow = true;
        }
        return false;
    }

    if (!DX7InterfaceHook::InitializeImGui(hwnd)) {
        LOG_ERROR("ImGuiService: failed to initialize ImGui backends");
        return false;
    }

    imguiInitialized = true;
    warnedNoDriver = false;
    warnedMissingWindow = false;

    if (!InstallWndProcHook(hwnd)) {
        LOG_WARN("ImGuiService: failed to install WndProc hook");
    }
    DX7InterfaceHook::SetFrameCallback(&ImGuiService::RenderFrameThunk);        
    DX7InterfaceHook::InstallSceneHooks();
    LOG_INFO("ImGuiService: ImGui initialized and scene hooks installed");
    return true;
}

void ImGuiService::SortPanels()
{
    std::sort(panels.begin(), panels.end(), [](const PanelEntry& a, const PanelEntry& b) {
        return a.desc.order < b.desc.order;
    });
}

bool ImGuiService::InstallWndProcHook(HWND hwnd)
{
    if (hookInstalled) {
        return true;
    }

    originalWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!originalWndProc) {
        return false;
    }

    gameWindow = hwnd;
    if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ImGuiService::WndProcHook))) {
        originalWndProc = nullptr;
        gameWindow = nullptr;
        return false;
    }

    hookInstalled = true;
    return true;
}

void ImGuiService::RemoveWndProcHook()
{
    if (hookInstalled && gameWindow && originalWndProc) {
        SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWndProc));
    }

    hookInstalled = false;
    originalWndProc = nullptr;
    gameWindow = nullptr;
}

LRESULT CALLBACK ImGuiService::WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
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

    if (!g_instance || !g_instance->originalWndProc) {
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_instance->originalWndProc, hWnd, msg, wParam, lParam);
}
