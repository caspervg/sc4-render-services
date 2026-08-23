#pragma once

// Shared ABI contract exported by SCGL.dll. Native objects are borrowed and valid only
// for the duration of the callback.
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>

enum SCGLD3D11Event : uint32_t
{
    SCGL_D3D11_EVENT_RENDER = 1,
    SCGL_D3D11_EVENT_BEFORE_DEVICE_DESTROY = 2,
};

struct SCGLD3D11FrameContext
{
    uint32_t structSize;
    uint32_t apiVersion;
    SCGLD3D11Event event;
    uint32_t deviceGeneration;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGISwapChain* swapChain;
    ID3D11RenderTargetView* renderTargetView;
    HWND window;
};

using SCGLD3D11FrameCallback = void(__stdcall*)(SCGLD3D11FrameContext const*, void*);
using SCGLRegisterD3D11FrameCallbackFn = BOOL(__stdcall*)(SCGLD3D11FrameCallback, void*);
using SCGLUnregisterD3D11FrameCallbackFn = BOOL(__stdcall*)(SCGLD3D11FrameCallback, void*);
