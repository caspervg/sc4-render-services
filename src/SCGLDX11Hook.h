#pragma once

#include <atomic>

#include "SCGLD3D11Service.h"
#include "cIGZGDriver.h"

class SCGLDX11Hook
{
public:
    using FrameCallback = void (*)(SCGLD3D11FrameContext const* frame);

    static bool CaptureInterface(cIGZGDriver* driver);
    static void SetFrameCallback(FrameCallback callback);
    static void Shutdown();
    static bool IsCaptured();
};
