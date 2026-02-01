#pragma once

#include <atomic>
#include <d3d.h>
#include <imgui.h>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Windows.h>

#include "cRZBaseSystemService.h"
#include "public/cIGZImGuiService.h"

// Forward declaration
struct IDirectDrawSurface7;

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class ImGuiService final : public cRZBaseSystemService, public cIGZImGuiService
{
public:
    ImGuiService();
    ~ImGuiService();

    uint32_t AddRef() override;
    uint32_t Release() override;

    bool QueryInterface(uint32_t riid, void** ppvObj) override;

    bool Init() override;
    bool Shutdown() override;
    bool OnTick(uint32_t unknown1) override;
    bool OnIdle(uint32_t unknown1) override;

    [[nodiscard]] uint32_t GetServiceID() const override;
    [[nodiscard]] uint32_t GetApiVersion() const override;
    [[nodiscard]] void* GetContext() const override;
    bool RegisterPanel(const ImGuiPanelDesc& desc) override;
    bool UnregisterPanel(uint32_t panelId) override;
    bool SetPanelVisible(uint32_t panelId, bool visible) override;
    bool QueueRender(ImGuiRenderCallback callback, void* data, ImGuiRenderCleanup cleanup = nullptr) override;
    bool AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) override;
    [[nodiscard]] bool IsDeviceReady() const override;
    [[nodiscard]] uint32_t GetDeviceGeneration() const override;

    ImGuiTextureHandle CreateTexture(const ImGuiTextureDesc& desc) override;
    [[nodiscard]] void* GetTextureID(ImGuiTextureHandle handle) override;
    void ReleaseTexture(ImGuiTextureHandle handle) override;
    [[nodiscard]] bool IsTextureValid(ImGuiTextureHandle handle) const override;

    bool RegisterFont(uint32_t fontId, const char* filePath, float sizePixels) override;
    bool RegisterFont(uint32_t fontId, const void* compressedFontData, int compressedFontDataSize, float sizePixels) override;
    bool UnregisterFont(uint32_t fontId) override;
    [[nodiscard]] void* GetFont(uint32_t fontId) const override;

    template <typename Fn>
    bool QueueRenderLambda(Fn&& fn) {
        using FnType = std::decay_t<Fn>;
        auto* heapFn = new (std::nothrow) FnType(std::forward<Fn>(fn));
        if (!heapFn) {
            return false;
        }
        return QueueRender(&ImGuiService::InvokeLambda_<FnType>, heapFn, &ImGuiService::DeleteLambda_<FnType>);
    }

private:
    struct PanelEntry
    {
        ImGuiPanelDesc desc;
        bool initialized;
    };

    struct ManagedFont
    {
        uint32_t id;
        ImFont* font;  // Pointer to ImFont* managed by ImGui
    };

    struct ManagedTexture
    {
        uint32_t id;
        uint32_t width;
        uint32_t height;
        uint32_t creationGeneration;
        std::vector<uint8_t> sourceData;       // RGBA32 pixel data for recreation
        IDirectDrawSurface7* surface;          // Can be nullptr if device lost
        bool needsRecreation;
        bool useSystemMemory;

        ManagedTexture()
            : id(0)
            , width(0)
            , height(0)
            , creationGeneration(0)
            , surface(nullptr)
            , needsRecreation(false)
            , useSystemMemory(false) {}
    };

    struct RenderQueueItem
    {
        ImGuiRenderCallback callback;
        void* data;
        ImGuiRenderCleanup cleanup;
    };

    template <typename FnType>
    static void InvokeLambda_(void* data) {
        (*static_cast<FnType*>(data))();
    }

    template <typename FnType>
    static void DeleteLambda_(void* data) {
        delete static_cast<FnType*>(data);
    }

    static void RenderFrameThunk_(IDirect3DDevice7* device);
    void RenderFrame_(IDirect3DDevice7* device);
    bool EnsureInitialized_();
    void InitializePanels_();
    void SortPanels_();
    bool InstallWndProcHook_(HWND hwnd);
    void RemoveWndProcHook_();
    static LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Texture management helpers
    bool CreateSurfaceForTexture_(ManagedTexture& tex);
    void OnDeviceLost_();
    void OnDeviceRestored_();
    void InvalidateAllTextures_();

private:
    std::vector<PanelEntry> panels_;
    mutable std::mutex panelsMutex_;

    std::vector<RenderQueueItem> renderQueue_;
    mutable std::mutex renderQueueMutex_;

    std::unordered_map<uint32_t, ManagedFont> fonts_;  // Key: font ID
    mutable std::mutex fontsMutex_;

    std::unordered_map<uint32_t, ManagedTexture> textures_;  // Key: texture ID
    mutable std::mutex texturesMutex_;

    HWND gameWindow_;
    WNDPROC originalWndProc_;
    bool initialized_;
    bool imguiInitialized_;
    bool hookInstalled_;
    bool warnedNoDriver_;
    bool warnedMissingWindow_;
    bool deviceLost_;
    std::atomic<uint32_t> deviceGeneration_;
    uint32_t nextTextureId_;
};
