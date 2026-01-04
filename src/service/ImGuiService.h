#pragma once

#include <cstdint>
#include <d3d.h>
#include <vector>

#include <Windows.h>

#include "cIGZImGuiService.h"
#include "cRZBaseSystemService.h"

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
    bool UnregisterPanel(uint32_t panel_id) override;
    bool SetPanelVisible(uint32_t panel_id, bool visible) override;

private:
    struct PanelEntry
    {
        ImGuiPanelDesc desc;
    };

    static void RenderFrameThunk(IDirect3DDevice7* device);
    void RenderFrame(IDirect3DDevice7* device);
    bool EnsureInitialized();
    void SortPanels();
    bool InstallWndProcHook(HWND hwnd);
    void RemoveWndProcHook();
    static LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::vector<PanelEntry> panels;

    HWND gameWindow;
    WNDPROC originalWndProc;
    bool initialized;
    bool imguiInitialized;
    bool hookInstalled;
    bool warnedNoDriver;
    bool warnedMissingWindow;
};
