// ReSharper disable CppDFAUnreachableCode
// ReSharper disable CppDFAConstantConditions
#include "ImGuiService.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <d3d.h>
#include <exception>
#include <filesystem>
#include <ranges>

#include "cIGZFrameWorkW32.h"
#include "cIGZGraphicSystem2.h"
#include "cISC43DRender.h"
#include "cISC4View3DWin.h"
#include "cRZAutoRefCount.h"
#include "cRZCOMDllDirector.h"
#include "DX7InterfaceHook.h"
#include "GZServPtrs.h"
#include "imgui_impl_gzgdriver.h"
#include "imgui_impl_win32.h"
#include "ImGuiThemes.h"
#include "public/ImGuiServiceIds.h"
#include "SC4UI.h"
#include "utils/Fonts.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

namespace {
    std::atomic<ImGuiService*> g_instance{nullptr};
    std::atomic<DWORD> g_renderThreadId{0};

    // The per-frame present at the end of cGZGraphicSystem::Draw (docs/generic-backend-re.md):
    //   0098e765  8b 10              mov  edx, [eax]
    //   0098e767  8b c8              mov  ecx, eax              <- ecx is the cIGZGDriver
    //   0098e769  ff 92 ac 00 00 00  call [edx+0xac]            <- Flush(), patched to call FlushHook_ + nop
    //   0098e76f  88 5e 26           mov  [esi+0x26], bl
    constexpr uintptr_t kFlushCallGuardAddress = 0x0098E765;
    constexpr uintptr_t kFlushCallSiteAddress = 0x0098E769;
    constexpr uint8_t kFlushCallGuardBytes[] = {
        0x8B, 0x10, 0x8B, 0xC8, 0xFF, 0x92, 0xAC, 0x00, 0x00, 0x00, 0x88, 0x5E, 0x26};
    constexpr uint8_t kFlushCallOriginalBytes[] = {0xFF, 0x92, 0xAC, 0x00, 0x00, 0x00};

    // SC4DrawContext thunks, as DrawService binds them.
    constexpr uintptr_t kGetDrawContextAddress = 0x004E82A0;
    constexpr uintptr_t kSetDefaultRenderStateUnilaterallyAddress = 0x007D5230;

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
      , initialized_(false)
      , imguiInitialized_(false)
      , rendererDriver_(nullptr)
      , identifiedDriver_(nullptr)
      , rendererReady_(false)
      , rendererFailed_(false)
      , flushHookInstalled_(false)
      , inFrame_(false)
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

    // Forget textures without calling the driver, which may already be shut down.
    {
        std::lock_guard textureLock(texturesMutex_);
        textures_.clear();
        pendingTextureReleaseIds_.clear();
    }

    RemoveFlushHook_();
    RemoveWndProcHook_();
    if (rendererReady_) {
        ImGui_ImplGZGDriver_Shutdown();
    }
    DestroyContext_();
    DX7InterfaceHook::ReleaseInterface();

    rendererReady_ = false;
    rendererDriver_ = nullptr;
    imguiInitialized_ = false;
    hookInstalled_ = false;
    deviceLost_ = false;
    g_renderThreadId.store(0, std::memory_order_release);
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    SetServiceRunning(false);
    return true;
}

bool ImGuiService::OnTick(uint32_t) {
    if (!initialized_) {
        return true;
    }

    CaptureD3DX_();
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
    return imguiInitialized_ && rendererReady_ && !deviceLost_;
}

uint32_t ImGuiService::GetDeviceGeneration() const {
    return deviceGeneration_.load(std::memory_order_acquire);
}

void __fastcall ImGuiService::FlushHook_(cIGZGDriver* driver) {
    if (auto* instance = g_instance.load(std::memory_order_acquire)) {
        instance->RenderOverlay_(driver);
    }
    driver->Flush();
}

void ImGuiService::RenderOverlay_(cIGZGDriver* driver) {
    if (!initialized_ || !imguiInitialized_ || !ImGui::GetCurrentContext()) {
        return;
    }

    if (driver != rendererDriver_) {
        if (!rendererReady_) {
            rendererReady_ = ImGui_ImplGZGDriver_Init(driver);
            if (!rendererReady_) {
                if (!rendererFailed_) {
                    LOG_ERROR("ImGuiService: ImGui cannot draw with this graphics driver, so ImGui windows will not be shown");
                    rendererFailed_ = true;
                }
                return;
            }
            LOG_INFO("ImGuiService: ImGui is ready and draws through {}", IdentifyDriver_(driver).name);
        }
        else {
            // A new driver object (video mode change): its textures are not the old ones.
            IdentifyDriver_(driver);
            LOG_INFO("ImGuiService: the game switched to a new graphics driver object, recreating ImGui textures");
            OnDeviceLost_();
            OnDeviceRestored_();
        }
        rendererDriver_ = driver;
    }

    // Panel code runs inside, so nothing may unwind into the game's frame.
    try {
        inFrame_ = true;
        ImGui_ImplWin32_NewFrame();
        ImGui_ImplGZGDriver_NewFrame(driver);
        if (BuildFrame_()) {
            ImGui_ImplGZGDriver_RenderDrawData(ImGui::GetDrawData());
        }
        inFrame_ = false;
        ResyncGameStateCache_();
    }
    catch (const std::exception& e) {
        inFrame_ = false;
        static bool s_logged = false;
        if (!s_logged) {
            LOG_ERROR("ImGuiService: exception while drawing ImGui: {}", e.what());
            s_logged = true;
        }
    }

    // How often the game presents decides how often ImGui updates. Report it once.
    static ULONGLONG s_firstFrameTick = 0;
    static uint32_t s_frames = 0;
    if (s_firstFrameTick != ~0ull) {
        const ULONGLONG now = GetTickCount64();
        if (s_firstFrameTick == 0) {
            s_firstFrameTick = now;
        }
        ++s_frames;
        if (now - s_firstFrameTick >= 10000) {
            LOG_INFO("ImGuiService: {} frames presented in the first 10 s", s_frames);
            s_firstFrameTick = ~0ull;
        }
    }
}

void ImGuiService::ResyncGameStateCache_() {
    // SC4DrawContext caches render state above the driver, so after ImGui changed it behind the
    // cache's back, make the cache issue everything again. Only city view has one.
    const auto view3DWin = SC4UI::GetView3DWin();
    if (!view3DWin) {
        return;
    }
    cISC43DRender* renderer = view3DWin->GetRenderer();
    if (!renderer) {
        return;
    }
    const auto getDrawContext = reinterpret_cast<void* (__thiscall*)(void*)>(kGetDrawContextAddress);
    const auto resetState = reinterpret_cast<void (__thiscall*)(void*)>(kSetDefaultRenderStateUnilaterallyAddress);
    if (void* drawContext = getDrawContext(renderer)) {
        resetState(drawContext);
    }
}

bool ImGuiService::InstallFlushHook_() {
    if (flushHookInstalled_) {
        return true;
    }

    auto* guard = reinterpret_cast<uint8_t*>(kFlushCallGuardAddress);
    if (std::memcmp(guard, kFlushCallGuardBytes, sizeof(kFlushCallGuardBytes)) != 0) {
        LOG_ERROR("ImGuiService: cannot attach to the game's frame drawing because the code at 0x{:08X} is not what "
                  "SC4RenderServices expects. Another plugin has probably changed it. ImGui windows will not be shown.",
                  static_cast<uint32_t>(kFlushCallGuardAddress));
        return false;
    }

    // call rel32 to FlushHook_, then a nop over the sixth byte of the original indirect call.
    uint8_t patch[sizeof(kFlushCallOriginalBytes)] = {0xE8, 0, 0, 0, 0, 0x90};
    const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&ImGuiService::FlushHook_) -
                                          (kFlushCallSiteAddress + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));

    auto* site = reinterpret_cast<uint8_t*>(kFlushCallSiteAddress);
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("ImGuiService: Windows did not allow attaching to the game's frame drawing (VirtualProtect failed). "
                  "ImGui windows will not be shown.");
        return false;
    }
    std::memcpy(site, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    VirtualProtect(site, sizeof(patch), oldProtect, &oldProtect);

    flushHookInstalled_ = true;
    LOG_INFO("ImGuiService: attached to the game's frame drawing at 0x{:08X}", static_cast<uint32_t>(kFlushCallSiteAddress));
    return true;
}

void ImGuiService::RemoveFlushHook_() {
    if (!flushHookInstalled_) {
        return;
    }

    auto* site = reinterpret_cast<uint8_t*>(kFlushCallSiteAddress);
    if (site[0] != 0xE8) {
        LOG_WARN("ImGuiService: another plugin changed the game's frame drawing after SC4RenderServices attached to it, "
                 "so it is left as it is");
        flushHookInstalled_ = false;
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, sizeof(kFlushCallOriginalBytes), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("ImGuiService: could not detach from the game's frame drawing (VirtualProtect failed)");
        return;
    }
    std::memcpy(site, kFlushCallOriginalBytes, sizeof(kFlushCallOriginalBytes));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kFlushCallOriginalBytes));
    VirtualProtect(site, sizeof(kFlushCallOriginalBytes), oldProtect, &oldProtect);
    flushHookInstalled_ = false;
}

bool ImGuiService::BuildFrame_() {
    static auto loggedFirstFrame = false;
    const DWORD threadId = GetCurrentThreadId();
    const DWORD prevThreadId = g_renderThreadId.load(std::memory_order_acquire);
    if (prevThreadId == 0) {
        g_renderThreadId.store(threadId, std::memory_order_release);
        LOG_DEBUG("ImGuiService::BuildFrame_: render thread id set to {}", threadId);
    }
    else if (prevThreadId != threadId) {
        LOG_WARN("ImGuiService::BuildFrame_: render thread id changed ({} -> {})",
                 prevThreadId, threadId);
        g_renderThreadId.store(threadId, std::memory_order_release);
    }
    if (!imguiInitialized_ || !ImGui::GetCurrentContext()) {
        return false;
    }

    {
        std::lock_guard lock(panelsMutex_);
        if (!initialized_) {
            return false;
        }
    }

    // Delay texture destruction until the next frame so draw commands emitted
    // earlier in the frame never reference a released driver texture.
    ProcessPendingTextureReleases_();

    InitializePanels_();
    ProcessPendingFontRegistrations_();

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

    ImGui::Render();

    if (!loggedFirstFrame) {
        LOG_INFO("ImGuiService: built first frame with {} panel(s)", panelsToRender.size());
        loggedFirstFrame = true;
    }
    return true;
}

bool ImGuiService::EnsureInitialized_() {
    if (imguiInitialized_) {
        return true;
    }

    cIGZGraphicSystem2Ptr pGS2;
    if (!pGS2) {
        return false;
    }

    if (!pGS2->GetGDriver()) {
        if (!warnedNoDriver_) {
            LOG_WARN("ImGuiService: graphics driver not available yet");
            warnedNoDriver_ = true;
        }
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

    if (!CreateContext_(hwnd)) {
        LOG_ERROR("ImGuiService: failed to create the ImGui context");
        return false;
    }

    imguiInitialized_ = true;
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    warnedNoDriver_ = false;
    warnedMissingWindow_ = false;

    if (!InstallWndProcHook_(hwnd)) {
        LOG_WARN("ImGuiService: failed to install WndProc hook");
    }
    if (!InstallFlushHook_()) {
        LOG_ERROR("ImGuiService: ImGui is set up, but it could not attach to the game's frame drawing, so ImGui "
                  "windows will not be shown");
        return true;
    }
    LOG_INFO("ImGuiService: ImGui context created");
    return true;
}

bool ImGuiService::CreateContext_(const HWND hwnd) {
    if (ImGui::GetCurrentContext()) {
        DestroyContext_();
        LOG_WARN("ImGuiService::CreateContext_: destroyed stale ImGui state before reinitializing");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiInitSettings& settings = initSettings_;

    if (settings.keyboardNav) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    }
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // ImGui keeps the pointer, so the path must outlive the context. Without a path it
    // would default to imgui.ini in the game's working directory, so disable it instead.
    static std::string s_layoutFile;
    s_layoutFile = settings.layoutFile;
    io.IniFilename = s_layoutFile.empty() ? nullptr : s_layoutFile.c_str();

    // Apply theme
    ApplyImGuiTheme(settings.theme, ImGui::GetStyle());

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
                LOG_INFO("ImGuiService::CreateContext_: loaded custom font from {}", fontPath.string());
            } else {
                LOG_WARN("ImGuiService::CreateContext_: failed to load font from {}, falling back to built-in", fontPath.string());
            }
        } else {
            LOG_WARN("ImGuiService::CreateContext_: font file not found: {}, falling back to built-in", fontPath.string());
        }
    }

    if (!font) {
        font = io.Fonts->AddFontFromMemoryCompressedTTF(
            ProggyVector_compressed_data, ProggyVector_compressed_size, scaledFontSize, &fontConfig);
        if (font) {
            LOG_INFO("ImGuiService::CreateContext_: loaded built-in font (size={})", scaledFontSize);
        } else {
            LOG_WARN("ImGuiService::CreateContext_: failed to load built-in font, will use the ImGui default");
        }
    }

    if (font) {
        io.FontDefault = font;
    }

    if (!ImGui_ImplWin32_Init(hwnd)) {
        LOG_ERROR("ImGuiService::CreateContext_: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }

    return true;
}

void ImGuiService::DestroyContext_() {
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    if (ImGui::GetIO().BackendPlatformUserData) {
        ImGui_ImplWin32_Shutdown();
    }
    ImGui::DestroyContext();
}

void ImGuiService::CaptureD3DX_() {
    // DX7 consumers (terrain decals, AcquireD3DInterfaces) read the native driver's D3DX
    // object. ImGui itself does not need it.
    // Only the built-in driver has that object. SCD3D11 reports the same class ID, so the class
    // ID alone would read an unrelated field of its driver.
    cIGZGraphicSystem2Ptr pGS2;
    cIGZGDriver* const pDriver = pGS2 ? pGS2->GetGDriver() : nullptr;
    if (pDriver && IdentifyDriver_(pDriver).kind == SC4GraphicsDriverKind::BuiltInDirectX) {
        DX7InterfaceHook::CaptureInterface(pDriver);
    }
}

const IdentifiedGraphicsDriver& ImGuiService::IdentifyDriver_(cIGZGDriver* driver) const {
    if (driver != identifiedDriver_) {
        identifiedDriver_ = driver;
        driverInfo_ = IdentifyGraphicsDriver(driver);
        if (driver) {
            LOG_INFO("ImGuiService: the game is drawing with {} (class ID 0x{:08X}, {})", driverInfo_.name,
                     driverInfo_.classId, driverInfo_.modulePath.empty() ? "module unknown" : driverInfo_.modulePath);
        }
    }
    return driverInfo_;
}

bool ImGuiService::GetActiveDriverInfo(SC4GraphicsDriverInfo& out) const {
    cIGZGraphicSystem2Ptr pGS2;
    cIGZGDriver* const pDriver = pGS2 ? pGS2->GetGDriver() : nullptr;
    if (!pDriver) {
        return false;
    }
    const IdentifiedGraphicsDriver& info = IdentifyDriver_(pDriver);
    out.kind = info.kind;
    out.classId = info.classId;
    out.name = info.name;
    out.modulePath = info.modulePath.c_str();
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

    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);

    // Create managed texture entry. desc.useSystemMemory was a DirectDraw placement hint
    // and has no meaning for driver textures.
    ManagedTexture tex;
    tex.width = desc.width;
    tex.height = desc.height;
    tex.creationGeneration = currentGen;

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

    // The driver texture is made on first use by GetTextureID(), inside the ImGui frame: only
    // there are driver calls safe from disturbing the game's own texture bindings.
    tex.needsRecreation = true;

    const uint32_t textureId = tex.id;

    {
        std::lock_guard lock(texturesMutex_);
        textures_.emplace(textureId, std::move(tex));
    }

    LOG_DEBUG("ImGuiService::CreateTexture: created texture id={} ({}x{}, gen={})",
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

    // A driver may drop textures when the device is lost. Recreate them from the kept pixels.
    if (tex.driverTexture != 0 && inFrame_ && !ImGui_ImplGZGDriver_IsTextureAlive(tex.driverTexture)) {
        tex.driverTexture = 0;
        tex.needsRecreation = true;
    }

    // Recreate the driver texture if needed
    if (tex.needsRecreation || tex.driverTexture == 0) {
        if (!CreateDriverTexture_(tex)) {
            return nullptr;
        }
    }

    return reinterpret_cast<void*>(static_cast<uintptr_t>(tex.driverTexture));
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

    LOG_DEBUG("ImGuiService::ReleaseTexture: queued texture release (id={})", handle.id);
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

        ReleaseDriverTexture_(it->second);
        textures_.erase(it);
        LOG_DEBUG("ImGuiService::ProcessPendingTextureReleases_: released texture (id={})", textureId);
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

    // No explicit atlas rebuild: with ImGuiBackendFlags_RendererHasTextures the renderer
    // backend uploads atlas changes through ImTextureData requests on the next frame.
    for (const auto& [id, _] : appliedFonts) {
        LOG_INFO("ImGuiService::RegisterFont: registered font ID {}", id);
    }
}

bool ImGuiService::CreateDriverTexture_(ManagedTexture& tex) {
    if (!inFrame_ || !rendererReady_) {
        return false;
    }

    const uint32_t texture = ImGui_ImplGZGDriver_CreateTexture(
        tex.sourceData.data(), static_cast<int>(tex.width), static_cast<int>(tex.height));
    if (texture == 0) {
        LOG_WARN("ImGuiService::CreateDriverTexture_: driver texture creation failed (id={}, {}x{})",
                 tex.id, tex.width, tex.height);
        return false;
    }

    tex.driverTexture = texture;
    tex.needsRecreation = false;
    tex.creationGeneration = deviceGeneration_.load(std::memory_order_acquire);
    return true;
}

void ImGuiService::ReleaseDriverTexture_(ManagedTexture& tex) {
    // Outside the frame the texture is only forgotten: the driver may be gone or replaced.
    if (tex.driverTexture != 0 && inFrame_ && rendererReady_) {
        ImGui_ImplGZGDriver_DestroyTexture(tex.driverTexture);
    }
    tex.driverTexture = 0;
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

    InvalidateAllTextures_();

    LOG_WARN("ImGuiService::OnDeviceLost_: device lost, notified panels and invalidated textures");
}

void ImGuiService::OnDeviceRestored_() {
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
}

void ImGuiService::InvalidateAllTextures_() {
    std::lock_guard lock(texturesMutex_);
    for (auto& tex : textures_ | std::views::values) {
        ReleaseDriverTexture_(tex);
        tex.needsRecreation = true;
    }
}
