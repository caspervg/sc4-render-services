#pragma once
#include <Windows.h>
#include <atomic>
#include "cIGZGDriver.h"
#include "cISGLDX7D3DX.h"

class DX7InterfaceHook
{
public:
    using FrameCallback = void (*)(IDirect3DDevice7* device);

    static bool CaptureInterface(cIGZGDriver* pDriver);
    static bool InitializeImGui(HWND hwnd);
    static bool InstallSceneHooks();
    static void SetFrameCallback(FrameCallback callback);
    static void ShutdownImGui();
    static cISGLDX7D3DX* GetD3DXInterface();
    static std::atomic<cISGLDX7D3DX*> s_pD3DX;
};
