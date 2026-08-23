#include "SCGLDX11Hook.h"

#include "utils/Logger.h"

namespace
{
    constexpr uint32_t kSCGLDriverClsid = 0xc4554841;
    std::atomic<SCGLDX11Hook::FrameCallback> g_frameCallback{};
    SCGLUnregisterD3D11FrameCallbackFn g_unregister{};
    bool g_registered{};

    void __stdcall ForwardFrame(SCGLD3D11FrameContext const* frame, void*) {
        auto const callback = g_frameCallback.load(std::memory_order_acquire);
        if (callback != nullptr) callback(frame);
    }
}

bool SCGLDX11Hook::CaptureInterface(cIGZGDriver* driver) {
    if (driver == nullptr || driver->GetGZCLSID() != kSCGLDriverClsid) return false;
    if (g_registered) return true;

    HMODULE const module = GetModuleHandleW(L"SCGL.dll");
    if (module == nullptr) {
        LOG_ERROR("SCGLDX11Hook: SCGL.dll is not loaded");
        return false;
    }
    auto const registerCallback = reinterpret_cast<SCGLRegisterD3D11FrameCallbackFn>(
        GetProcAddress(module, "SCGLRegisterD3D11FrameCallback"));
    g_unregister = reinterpret_cast<SCGLUnregisterD3D11FrameCallbackFn>(
        GetProcAddress(module, "SCGLUnregisterD3D11FrameCallback"));
    if (registerCallback == nullptr || g_unregister == nullptr || !registerCallback(ForwardFrame, nullptr)) {
        LOG_ERROR("SCGLDX11Hook: SCGL D3D11 service API is unavailable or already owned");
        g_unregister = nullptr;
        return false;
    }
    g_registered = true;
    LOG_INFO("SCGLDX11Hook: registered the SCGL D3D11 frame callback");
    return true;
}

void SCGLDX11Hook::SetFrameCallback(FrameCallback callback) {
    g_frameCallback.store(callback, std::memory_order_release);
}

void SCGLDX11Hook::Shutdown() {
    g_frameCallback.store(nullptr, std::memory_order_release);
    if (g_registered && g_unregister != nullptr) g_unregister(ForwardFrame, nullptr);
    g_unregister = nullptr;
    g_registered = false;
}

bool SCGLDX11Hook::IsCaptured() {
    return g_registered;
}
