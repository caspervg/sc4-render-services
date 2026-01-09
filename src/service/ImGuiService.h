#pragma once

#include <cstdint>
#include <d3d.h>
#include <vector>
#include <Windows.h>
#include <atomic>
#include <mutex>

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
    bool AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) override;
    [[nodiscard]] bool IsDeviceReady() const override;
    [[nodiscard]] uint32_t GetDeviceGeneration() const override;

    ImGuiTextureHandle CreateTexture(const ImGuiTextureDesc& desc) override;
    [[nodiscard]] void* GetTextureID(ImGuiTextureHandle handle) override;
    void ReleaseTexture(ImGuiTextureHandle handle) override;
    [[nodiscard]] bool IsTextureValid(ImGuiTextureHandle handle) const override;

private:
    struct PanelEntry
    {
        ImGuiPanelDesc desc;
        bool initialized;
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

    std::vector<ManagedTexture> textures_;
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
