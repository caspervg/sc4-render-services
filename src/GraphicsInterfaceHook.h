#pragma once
#include <atomic>
#include <string>
#include <Windows.h>
#include <d3d11.h>
#include "cIGZGDriver.h"
#include "cISGLDX7D3DX.h"

struct ImGuiInitSettings
{
    float fontSize = 13.0f;
    std::string fontFile;           // Empty = use built-in ProggyVector
    int fontOversample = 2;
    std::string theme = "dark";     // dark, light, classic
    bool keyboardNav = true;
    float uiScale = 1.0f;
};

class GraphicsInterfaceHook
{
public:
    using FrameCallback = void (*)(IDirect3DDevice7* device);

    static bool CaptureInterface(cIGZGDriver* pDriver);
    static bool InitializeImGui(HWND hwnd, const ImGuiInitSettings& settings = {});
    static bool InitializeImGuiD3D11(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
                                     const ImGuiInitSettings& settings = {});
    static bool InstallSceneHooks();
    static void SetFrameCallback(FrameCallback callback);
    static void ShutdownImGui();
    static cISGLDX7D3DX* GetD3DXInterface();
    static std::atomic<cISGLDX7D3DX*> s_pD3DX;
};
