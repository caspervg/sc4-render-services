#pragma once
#include "cIGZUnknown.h"

struct IDirect3D7;
struct IDirect3DDevice7;
struct IDirectDraw7;
struct IDirectDrawSurface7;

static constexpr auto kSCGDriverDirectX = 0xBADB6906;

class cISGLDX7D3DX : public cIGZUnknown
{
public:
    virtual IDirectDraw7* GetDD() = 0;
    virtual IDirect3D7* GetD3D() = 0;
    virtual IDirect3DDevice7* GetD3DDevice() = 0;
    virtual IDirectDrawSurface7* GetPrimary() = 0;
    virtual IDirectDrawSurface7* GetZBuffer() = 0;
    virtual int32_t ReinitDevice() = 0; // Returns a HRESULT
    virtual void ShutdownDevice() = 0;
    virtual void UpdateFrame(uint32_t primarySurfaceFlipFlags) = 0;
    virtual void RestoreSurfaces() = 0;    
};