// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAConstantFunctionResult
// ReSharper disable CppDFAUnreachableCode
#include "cIGZFrameWork.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC43DRender.h"
#include "cISC4View3DWin.h"
#include "cRZCOMDllDirector.h"
#include "cISTETerrain.h"
#include "cS3DCamera.h"
#include "GZServPtrs.h"
#include "imgui.h"
#include "public/ImGuiTexture.h"
#include "public/cIGZImGuiService.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZS3DCameraService.h"
#include "public/S3DCameraServiceIds.h"
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "SC4UI.h"
#include "utils/Logger.h"
#define WIN32_LEAN_AND_MEAN
#include <ddraw.h>
#include <gdiplus.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>
#include <cstdio>

#include "DX7InterfaceHook.h"
#include "ImGuiService.h"

namespace {
    constexpr auto kWorldProjectionSampleDirectorID = 0xB7E4F2A9;
    constexpr uint32_t kWorldProjectionPanelId = 0x3D9C8B1F;
    constexpr const wchar_t* kBillboardImagePath =
        L"C:\\Users\\caspe\\CLionProjects\\sc4-render-services\\assets\\nam49.jpg";
    bool gGdiplusStarted = false;
    ULONG_PTR gGdiplusToken = 0;
    cIGZS3DCameraService* gCameraService = nullptr;
    S3DCameraHandle gCameraHandle{nullptr, 0, false};

    struct GridConfig {
        bool enabled = true;
        int gridSpacing = 64; // World units between grid lines
        int gridExtent = 512; // How far from center to draw
        float centerX = 512.0f; // Center world position X
        float centerY = 281.0f; // Center world position Y (height)
        float centerZ = 512.0f; // Center world position Z
        ImVec4 gridColor = ImVec4(0.0f, 1.0f, 0.0f, 0.8f); // RGBA
        float lineThickness = 2.0f;
        bool drawCenterMarker = true;
        float markerSize = 10.0f;
        bool conformToTerrain = false;
        bool terrainSnapToGrid = true;
        int terrainSampleStep = 16;
        bool drawText = true;
        bool textBillboard = true;
        float textDepthScale = 0.002f;
        float textOffsetX = 0.0f;
        float textOffsetY = -28.0f;
        bool textLeaderLine = true;
        bool textBackground = true;
        bool textOutline = true;
        bool textShadow = true;
        ImVec4 textColor = ImVec4(1.0f, 0.92f, 0.2f, 1.0f);
        char text[64] = "World label";
        bool drawImage = true;
        bool imageBillboard = true;
        float imageSize = 64.0f;
        float imageOffsetX = 0.0f;
        float imageOffsetY = 0.0f;
        ImGuiTexture imageTexture;
        std::vector<uint8_t> imagePixels;
        uint32_t imageWidth = 0;
        uint32_t imageHeight = 0;
        bool imageLoaded = false;
        cIGZImGuiService* imguiService = nullptr;
    };

    struct DepthDebugState {
        cIGZImGuiService* imguiService = nullptr;
        ImGuiTexture depthTexture;
        ImGuiTexture maskedOverlayTexture;
        std::vector<uint8_t> rgbaPixels;
        std::vector<uint32_t> depthRaw;
        std::vector<float> histogram; // normalized 0..1 values, 256 bins
        uint32_t texWidth = 0;
        uint32_t texHeight = 0;
        float minDepth = 0.0f;
        float maxDepth = 0.0f;
        bool lastCaptureOk = false;
        char status[128] = "Idle";
        bool autoRefresh = false; // safer default
        // Demo overlay settings
        bool showMaskedOverlay = true;
        float overlaySize = 96.0f;
        float overlayBias = 80.0f; // depth units; tune to avoid z-fighting
        ImVec4 overlayColor = ImVec4(0.2f, 0.8f, 1.0f, 1.0f);
    };

    struct WorldProjectionData {
        GridConfig grid;
        DepthDebugState depth;
        cIGZS3DCameraService* cameraService = nullptr;
        S3DCameraHandle cameraHandle{nullptr, 0, false};
    };

    float ClampFloat(float value, float minValue, float maxValue);

    ImU32 DepthColorRamp(float t) {
        // Blue -> Cyan -> Green -> Yellow -> Red
        t = ClampFloat(t, 0.0f, 1.0f);
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (t < 0.25f) {
            const float k = t / 0.25f;
            r = 0.0f;
            g = k;
            b = 1.0f;
        }
        else if (t < 0.5f) {
            const float k = (t - 0.25f) / 0.25f;
            r = 0.0f;
            g = 1.0f;
            b = 1.0f - k;
        }
        else if (t < 0.75f) {
            const float k = (t - 0.5f) / 0.25f;
            r = k;
            g = 1.0f;
            b = 0.0f;
        }
        else {
            const float k = (t - 0.75f) / 0.25f;
            r = 1.0f;
            g = 1.0f - k;
            b = 0.0f;
        }
        return IM_COL32(static_cast<int>(r * 255.0f),
                        static_cast<int>(g * 255.0f),
                        static_cast<int>(b * 255.0f),
                        255);
    }

    bool CaptureDepthBuffer(DepthDebugState& state) {
        if (!state.imguiService || !state.imguiService->IsDeviceReady()) {
            sprintf_s(state.status, "ImGui device not ready");
            state.lastCaptureOk = false;
            return false;
        }

        IDirect3DDevice7* device = nullptr;
        IDirectDraw7* dd = nullptr;
        if (!state.imguiService->AcquireD3DInterfaces(&device, &dd) || !device || !dd) {
            sprintf_s(state.status, "D3D/Draw not ready");
            if (device) device->Release();
            if (dd) dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        const HRESULT coop = dd->TestCooperativeLevel();
        if (coop == DDERR_SURFACELOST || coop == DDERR_WRONGMODE || coop == DDERR_EXCLUSIVEMODEALREADYSET) {
            sprintf_s(state.status, "Device not ready (0x%08X)", static_cast<uint32_t>(coop));
            device->Release();
            dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        IDirectDrawSurface7* colorSurface = nullptr;
        HRESULT hr = device->GetRenderTarget(&colorSurface);
        if (FAILED(hr) || !colorSurface) {
            sprintf_s(state.status, "GetRenderTarget failed (0x%08X)", static_cast<uint32_t>(hr));
            if (colorSurface) colorSurface->Release();
            device->Release();
            dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        DDSCAPS2 caps{};
        caps.dwCaps = DDSCAPS_ZBUFFER;
        IDirectDrawSurface7* zBuffer = nullptr;
        hr = colorSurface->GetAttachedSurface(&caps, &zBuffer);
        colorSurface->Release();
        if (FAILED(hr) || !zBuffer) {
            sprintf_s(state.status, "GetAttachedSurface(Z) failed (0x%08X)", static_cast<uint32_t>(hr));
            if (zBuffer) zBuffer->Release();
            device->Release();
            dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof(desc);
        HRESULT hr2 = zBuffer->Lock(nullptr, &desc, DDLOCK_READONLY | DDLOCK_WAIT, nullptr);
        if (FAILED(hr2)) {
            sprintf_s(state.status, "Lock failed (0x%08X)", static_cast<uint32_t>(hr2));
            zBuffer->Release();
            device->Release();
            dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        const uint32_t width = desc.dwWidth;
        const uint32_t height = desc.dwHeight;
        const uint32_t bitDepth = desc.ddpfPixelFormat.dwZBufferBitDepth;
        const uint32_t bytesPerPixel = (bitDepth + 7) / 8;

        if (bytesPerPixel != 2 && bytesPerPixel != 3 && bytesPerPixel != 4) {
            sprintf_s(state.status, "Unsupported Z format: %u bpp", bitDepth);
            zBuffer->Unlock(nullptr);
            zBuffer->Release();
            device->Release();
            dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        uint32_t minVal = (std::numeric_limits<uint32_t>::max)();
        uint32_t maxVal = 0;

        auto* base = static_cast<uint8_t*>(desc.lpSurface);
        state.depthRaw.resize(static_cast<size_t>(width) * height);

        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = base + y * desc.lPitch;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t value = 0;
                switch (bytesPerPixel) {
                case 2: value = reinterpret_cast<const uint16_t*>(row)[x];
                    break;
                case 3: {
                    const uint8_t* p = row + x * 3;
                    value = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                        (static_cast<uint32_t>(p[2]) << 16);
                    break;
                }
                case 4: value = reinterpret_cast<const uint32_t*>(row)[x];
                    break;
                }
                minVal = (std::min)(minVal, value);
                maxVal = (std::max)(maxVal, value);
            }
        }

        if (minVal == (std::numeric_limits<uint32_t>::max)()) {
            sprintf_s(state.status, "No depth data");
            zBuffer->Unlock(nullptr);
            zBuffer->Release();
            device->Release();
            dd->Release();
            state.lastCaptureOk = false;
            return false;
        }

        const float denom = maxVal > minVal ? static_cast<float>(maxVal - minVal) : 1.0f;

        state.rgbaPixels.resize(static_cast<size_t>(width) * height * 4);
        std::vector<uint32_t> histogramCounts(256, 0);
        uint32_t maxCount = 1;

        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = base + y * desc.lPitch;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t value = 0;
                switch (bytesPerPixel) {
                case 2: value = reinterpret_cast<const uint16_t*>(row)[x];
                    break;
                case 3: {
                    const uint8_t* p = row + x * 3;
                    value = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                        (static_cast<uint32_t>(p[2]) << 16);
                    break;
                }
                case 4: value = reinterpret_cast<const uint32_t*>(row)[x];
                    break;
                }

                state.depthRaw[static_cast<size_t>(y) * width + x] = value;

                const float t = ClampFloat(static_cast<float>(value - minVal) / denom, 0.0f, 1.0f);
                const ImU32 color = DepthColorRamp(t);
                const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
                state.rgbaPixels[idx + 0] = static_cast<uint8_t>((color >> IM_COL32_R_SHIFT) & 0xFF);
                state.rgbaPixels[idx + 1] = static_cast<uint8_t>((color >> IM_COL32_G_SHIFT) & 0xFF);
                state.rgbaPixels[idx + 2] = static_cast<uint8_t>((color >> IM_COL32_B_SHIFT) & 0xFF);
                state.rgbaPixels[idx + 3] = 255;

                const uint32_t bin = (std::min)(255u, static_cast<uint32_t>(t * 255.0f + 0.5f));
                maxCount = (std::max)(maxCount, ++histogramCounts[bin]);
            }
        }

        zBuffer->Unlock(nullptr);
        zBuffer->Release();
        device->Release();
        dd->Release();

        state.histogram.resize(256);
        for (size_t i = 0; i < histogramCounts.size(); ++i) {
            state.histogram[i] = static_cast<float>(histogramCounts[i]) / static_cast<float>(maxCount);
        }

        state.texWidth = width;
        state.texHeight = height;
        state.minDepth = static_cast<float>(minVal);
        state.maxDepth = static_cast<float>(maxVal);

        state.depthTexture.Release();
        if (!state.depthTexture.Create(state.imguiService, width, height, state.rgbaPixels.data(), true)) {
            sprintf_s(state.status, "Texture upload failed");
            state.lastCaptureOk = false;
            return false;
        }

        sprintf_s(state.status, "Captured %ux%u, %u bpp", width, height, bitDepth);
        state.lastCaptureOk = true;
        return true;
    }

    void BuildMaskedOverlay(const DepthDebugState& depth, float screenX, float screenY) {
        if (depth.texWidth == 0 || depth.texHeight == 0 || depth.depthRaw.empty() || !depth.imguiService) {
            return;
        }

        const int sizePx = static_cast<int>(depth.overlaySize);
        const int half = sizePx / 2;

        std::vector<uint8_t> maskPixels(static_cast<size_t>(sizePx) * sizePx * 4, 0);
        const float r = depth.overlayColor.x;
        const float g = depth.overlayColor.y;
        const float b = depth.overlayColor.z;
        const uint32_t pitch = depth.texWidth;

        const int baseX = static_cast<int>(screenX) - half;
        const int baseY = static_cast<int>(screenY) - half;
        const uint32_t bias = depth.overlayBias > 0.0f ? static_cast<uint32_t>(depth.overlayBias) : 0u;
        const int centerX = std::clamp(static_cast<int>(screenX), 0, static_cast<int>(depth.texWidth) - 1);
        const int centerY = std::clamp(static_cast<int>(screenY), 0, static_cast<int>(depth.texHeight) - 1);
        const uint32_t overlayDepth = depth.depthRaw[static_cast<size_t>(centerY) * pitch + static_cast<size_t>(
            centerX)];

        for (int y = 0; y < sizePx; ++y) {
            int srcY = baseY + y;
            if (srcY < 0 || srcY >= static_cast<int>(depth.texHeight)) {
                continue;
            }
            for (int x = 0; x < sizePx; ++x) {
                int srcX = baseX + x;
                if (srcX < 0 || srcX >= static_cast<int>(depth.texWidth)) {
                    continue;
                }
                const uint32_t sceneDepth = depth.depthRaw[static_cast<size_t>(srcY) * pitch + static_cast<size_t>(
                    srcX)];
                // Scene pixel is in front if it is closer (smaller depth) than our overlay center minus bias.
                const bool sceneInFront = sceneDepth + bias < overlayDepth;

                const size_t idx = (static_cast<size_t>(y) * sizePx + x) * 4;
                maskPixels[idx + 0] = static_cast<uint8_t>(r * 255.0f);
                maskPixels[idx + 1] = static_cast<uint8_t>(g * 255.0f);
                maskPixels[idx + 2] = static_cast<uint8_t>(b * 255.0f);
                maskPixels[idx + 3] = sceneInFront ? 0 : 255; // hide where building is in front
            }
        }

        auto* service = depth.imguiService;
        ImGuiTexture* tex = const_cast<ImGuiTexture*>(&depth.maskedOverlayTexture);
        tex->Release();
        tex->Create(service, sizePx, sizePx, maskPixels.data(), true);
    }

    bool IsCityView() {
        const cISC4AppPtr app;
        if (!app) {
            return false;
        }
        return app->GetCity() != nullptr;
    }

    float ClampFloat(float value, float minValue, float maxValue) {
        if (value < minValue) {
            return minValue;
        }
        if (value > maxValue) {
            return maxValue;
        }
        return value;
    }

    bool StartGdiplus() {
        if (gGdiplusStarted) {
            return true;
        }
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gGdiplusToken, &input, nullptr) != Gdiplus::Ok) {
            gGdiplusToken = 0;
            return false;
        }
        gGdiplusStarted = true;
        return true;
    }

    void StopGdiplus() {
        if (!gGdiplusStarted) {
            return;
        }
        Gdiplus::GdiplusShutdown(gGdiplusToken);
        gGdiplusToken = 0;
        gGdiplusStarted = false;
    }

    bool LoadJpegToBgra(const wchar_t* path, std::vector<uint8_t>& outPixels,
                        uint32_t& outWidth, uint32_t& outHeight) {
        if (!gGdiplusStarted) {
            return false;
        }

        Gdiplus::Bitmap bitmap(path);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }

        outWidth = bitmap.GetWidth();
        outHeight = bitmap.GetHeight();
        if (outWidth == 0 || outHeight == 0) {
            return false;
        }

        Gdiplus::Rect rect(0, 0, static_cast<INT>(outWidth), static_cast<INT>(outHeight));
        Gdiplus::BitmapData data{};
        if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
            return false;
        }

        outPixels.resize(static_cast<size_t>(outWidth) * outHeight * 4);
        for (uint32_t y = 0; y < outHeight; ++y) {
            const uint8_t* srcRow = static_cast<const uint8_t*>(data.Scan0) + y * data.Stride;
            uint8_t* dstRow = outPixels.data() + (static_cast<size_t>(y) * outWidth * 4);
            for (uint32_t x = 0; x < outWidth; ++x) {
                const uint8_t b = srcRow[x * 4 + 0];
                const uint8_t g = srcRow[x * 4 + 1];
                const uint8_t r = srcRow[x * 4 + 2];
                const uint8_t a = srcRow[x * 4 + 3];
                dstRow[x * 4 + 0] = b;
                dstRow[x * 4 + 1] = g;
                dstRow[x * 4 + 2] = r;
                dstRow[x * 4 + 3] = a;
            }
        }

        bitmap.UnlockBits(&data);
        return true;
    }

    void DrawWorldGrid(cS3DCamera* camera, cISTETerrain* terrain, const GridConfig& config) {
        if (!camera || !config.enabled) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(config.gridColor);
        const int stepValue = config.terrainSampleStep > 0 ? config.terrainSampleStep : 16;
        const float sampleStep = static_cast<float>(stepValue);

        // Draw grid lines parallel to X axis (running along X direction)
        for (float z = -config.gridExtent; z <= config.gridExtent; z += config.gridSpacing) {
            float worldZ = config.centerZ + z;

            // Start and end points along X axis
            float startX = config.centerX - config.gridExtent;
            float endX = config.centerX + config.gridExtent;

            if (!config.conformToTerrain || !terrain) {
                float screenX1, screenY1, screenX2, screenY2;
                bool visible1 = gCameraService->WorldToScreen(gCameraHandle, startX, config.centerY, worldZ, screenX1,
                                                              screenY1);
                bool visible2 = gCameraService->WorldToScreen(gCameraHandle, endX, config.centerY, worldZ, screenX2,
                                                              screenY2);

                if (visible1 && visible2) {
                    drawList->AddLine(
                        ImVec2(screenX1, screenY1),
                        ImVec2(screenX2, screenY2),
                        color,
                        config.lineThickness
                    );
                }
                continue;
            }

            bool hasPrev = false;
            ImVec2 prevPos{};
            for (float x = startX; x <= endX; x += sampleStep) {
                if (!terrain->LocationIsInBounds(x, worldZ)) {
                    hasPrev = false;
                    continue;
                }

                const float y = config.terrainSnapToGrid
                                    ? terrain->GetAltitudeAtNearestGrid(x, worldZ)
                                    : terrain->GetAltitude(x, worldZ);
                float sx, sy;
                if (!gCameraService->WorldToScreen(gCameraHandle, x, y, worldZ, sx, sy)) {
                    hasPrev = false;
                    continue;
                }

                const ImVec2 curPos(sx, sy);
                if (hasPrev) {
                    drawList->AddLine(prevPos, curPos, color, config.lineThickness);
                }
                prevPos = curPos;
                hasPrev = true;
            }
        }

        // Draw grid lines parallel to Z axis (running along Z direction)
        for (float x = -config.gridExtent; x <= config.gridExtent; x += config.gridSpacing) {
            float worldX = config.centerX + x;

            // Start and end points along Z axis
            float startZ = config.centerZ - config.gridExtent;
            float endZ = config.centerZ + config.gridExtent;

            if (!config.conformToTerrain || !terrain) {
                float screenX1, screenY1, screenX2, screenY2;
                bool visible1 = gCameraService->WorldToScreen(gCameraHandle, worldX, config.centerY, startZ, screenX1,
                                                              screenY1);
                bool visible2 = gCameraService->WorldToScreen(gCameraHandle, worldX, config.centerY, endZ, screenX2,
                                                              screenY2);

                if (visible1 && visible2) {
                    drawList->AddLine(
                        ImVec2(screenX1, screenY1),
                        ImVec2(screenX2, screenY2),
                        color,
                        config.lineThickness
                    );
                }
                continue;
            }

            bool hasPrev = false;
            ImVec2 prevPos{};
            for (float z = startZ; z <= endZ; z += sampleStep) {
                if (!terrain->LocationIsInBounds(worldX, z)) {
                    hasPrev = false;
                    continue;
                }

                const float y = config.terrainSnapToGrid
                                    ? terrain->GetAltitudeAtNearestGrid(worldX, z)
                                    : terrain->GetAltitude(worldX, z);
                float sx, sy;
                if (!gCameraService->WorldToScreen(gCameraHandle, worldX, y, z, sx, sy)) {
                    hasPrev = false;
                    continue;
                }

                const ImVec2 curPos(sx, sy);
                if (hasPrev) {
                    drawList->AddLine(prevPos, curPos, color, config.lineThickness);
                }
                prevPos = curPos;
                hasPrev = true;
            }
        }

        // Draw center marker
        if (config.drawCenterMarker) {
            float screenX, screenY;
            if (gCameraService->WorldToScreen(gCameraHandle, config.centerX, config.centerY, config.centerZ, screenX,
                                              screenY)) {
                // Draw crosshair at center
                ImU32 markerColor = IM_COL32(255, 0, 0, 255); // Red marker
                drawList->AddCircleFilled(
                    ImVec2(screenX, screenY),
                    config.markerSize * 0.5f,
                    markerColor
                );

                // Draw crosshair lines
                drawList->AddLine(
                    ImVec2(screenX - config.markerSize, screenY),
                    ImVec2(screenX + config.markerSize, screenY),
                    markerColor,
                    config.lineThickness
                );
                drawList->AddLine(
                    ImVec2(screenX, screenY - config.markerSize),
                    ImVec2(screenX, screenY + config.markerSize),
                    markerColor,
                    config.lineThickness
                );
            }
        }
    }

    void DrawWorldText(cS3DCamera* camera, const GridConfig& config) {
        if (!camera || !config.drawText) {
            return;
        }

        float screenX = 0.0f;
        float screenY = 0.0f;
        float depth = 0.0f;
        if (!gCameraService->WorldToScreen(gCameraHandle, config.centerX, config.centerY, config.centerZ, screenX,
                                           screenY, &depth)) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        float alpha = config.textColor.w;
        if (!config.textBillboard) {
            const float fade = 1.0f / (1.0f + depth * config.textDepthScale);
            alpha *= ClampFloat(fade, 0.2f, 1.0f);
        }

        ImVec4 textColorF = config.textColor;
        textColorF.w = ClampFloat(alpha, 0.0f, 1.0f);
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(textColorF);
        const ImU32 outlineColor = IM_COL32(0, 0, 0, 210);
        const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
        const ImU32 leaderColor = IM_COL32(0, 0, 0, 180);

        const char* label = config.text;
        const ImVec2 labelSize = ImGui::CalcTextSize(label);

        const ImVec2 anchor(screenX, screenY);
        ImVec2 textPos = ImVec2(
            screenX + config.textOffsetX - labelSize.x * 0.5f,
            screenY + config.textOffsetY - labelSize.y
        );

        if (config.textLeaderLine && (config.textOffsetX != 0.0f || config.textOffsetY != 0.0f)) {
            drawList->AddLine(anchor,
                              ImVec2(textPos.x + labelSize.x * 0.5f, textPos.y + labelSize.y),
                              leaderColor,
                              1.5f);
        }

        if (config.textBackground) {
            const ImVec2 pad(4.0f, 2.0f);
            drawList->AddRectFilled(
                ImVec2(textPos.x - pad.x, textPos.y - pad.y),
                ImVec2(textPos.x + labelSize.x + pad.x, textPos.y + labelSize.y + pad.y),
                IM_COL32(0, 0, 0, 140),
                4.0f
            );
        }

        if (config.textShadow) {
            drawList->AddText(ImVec2(textPos.x + 2.0f, textPos.y + 2.0f), shadowColor, label);
        }
        if (config.textOutline) {
            drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y), outlineColor, label);
            drawList->AddText(ImVec2(textPos.x - 1.0f, textPos.y), outlineColor, label);
            drawList->AddText(ImVec2(textPos.x, textPos.y + 1.0f), outlineColor, label);
            drawList->AddText(ImVec2(textPos.x, textPos.y - 1.0f), outlineColor, label);
        }

        drawList->AddText(textPos, textColor, label);
    }

    void DrawWorldImage(cS3DCamera* camera, cISTETerrain* terrain, GridConfig& config) {
        if (!camera || !config.drawImage || !config.imguiService) {
            return;
        }

        float screenX = 0.0f;
        float screenY = 0.0f;
        float depth = 0.0f;
        if (!gCameraService->WorldToScreen(gCameraHandle, config.centerX, config.centerY, config.centerZ, screenX,
                                           screenY, &depth)) {
            return;
        }

        if (!config.imageLoaded) {
            config.imageLoaded = LoadJpegToBgra(kBillboardImagePath, config.imagePixels,
                                                config.imageWidth, config.imageHeight);
            if (config.imageLoaded) {
                config.imageTexture.Create(config.imguiService, config.imageWidth,
                                           config.imageHeight, config.imagePixels.data());
            }
        }

        void* texId = config.imageTexture.GetID();
        if (!texId && config.imageLoaded) {
            config.imageTexture.Create(config.imguiService, config.imageWidth,
                                       config.imageHeight, config.imagePixels.data());
            texId = config.imageTexture.GetID();
        }

        if (!texId) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        if (config.imageBillboard) {
            const float size = config.imageSize;
            const ImVec2 center(screenX + config.imageOffsetX, screenY + config.imageOffsetY);
            const ImVec2 half(size * 0.5f, size * 0.5f);
            const ImVec2 minPos(center.x - half.x, center.y - half.y);
            const ImVec2 maxPos(center.x + half.x, center.y + half.y);
            drawList->AddImage(texId, minPos, maxPos);
            return;
        }

        const float worldX = config.centerX + config.imageOffsetX;
        const float worldZ = config.centerZ + config.imageOffsetY;
        const float halfSize = config.imageSize * 0.5f;

        auto sampleHeight = [&](float x, float z) -> float {
            if (!config.conformToTerrain || !terrain || !terrain->LocationIsInBounds(x, z)) {
                return config.centerY;
            }
            return config.terrainSnapToGrid
                       ? terrain->GetAltitudeAtNearestGrid(x, z)
                       : terrain->GetAltitude(x, z);
        };

        const float y1 = sampleHeight(worldX - halfSize, worldZ - halfSize);
        const float y2 = sampleHeight(worldX + halfSize, worldZ - halfSize);
        const float y3 = sampleHeight(worldX + halfSize, worldZ + halfSize);
        const float y4 = sampleHeight(worldX - halfSize, worldZ + halfSize);

        float p1x, p1y, p2x, p2y, p3x, p3y, p4x, p4y;
        if (!gCameraService->WorldToScreen(gCameraHandle, worldX - halfSize, y1, worldZ - halfSize, p1x, p1y) ||
            !gCameraService->WorldToScreen(gCameraHandle, worldX + halfSize, y2, worldZ - halfSize, p2x, p2y) ||
            !gCameraService->WorldToScreen(gCameraHandle, worldX + halfSize, y3, worldZ + halfSize, p3x, p3y) ||
            !gCameraService->WorldToScreen(gCameraHandle, worldX - halfSize, y4, worldZ + halfSize, p4x, p4y)) {
            return;
        }

        drawList->AddImageQuad(
            texId,
            ImVec2(p1x, p1y),
            ImVec2(p2x, p2y),
            ImVec2(p3x, p3y),
            ImVec2(p4x, p4y)
        );
    }

    void RenderWorldProjectionPanel(void* userData) {
        if (!IsCityView()) {
            return;
        }

        auto* data = static_cast<WorldProjectionData*>(userData);
        if (!data) {
            return;
        }
        auto& config = data->grid;
        auto& depth = data->depth;

        float overlayScreenX = 0.0f, overlayScreenY = 0.0f;
        bool overlayHasPos = false;

        // Refresh camera handle from service each frame (handles device/lifetime changes).
        if (data->cameraService) {
            data->cameraHandle = data->cameraService->WrapActiveRendererCamera();
            gCameraService = data->cameraService;
            gCameraHandle = data->cameraHandle;
        }
        else {
            gCameraService = nullptr;
            gCameraHandle = {nullptr, 0, false};
        }

        cS3DCamera* camera = static_cast<cS3DCamera*>(data->cameraHandle.ptr);
        cISC4AppPtr app;
        cISTETerrain* terrain = nullptr;
        if (app) {
            cISC4City* city = app->GetCity();
            if (city) {
                terrain = city->GetTerrain();
            }
        }

        if (camera) {
            DrawWorldGrid(camera, terrain, config);
            DrawWorldText(camera, config);
            DrawWorldImage(camera, terrain, config);
            overlayHasPos = gCameraService->WorldToScreen(gCameraHandle, config.centerX, config.centerY, config.centerZ,
                                                          overlayScreenX, overlayScreenY, nullptr);
        }

        if (depth.autoRefresh) {
            CaptureDepthBuffer(depth);
        }

        if (depth.showMaskedOverlay && depth.lastCaptureOk && overlayHasPos) {
            BuildMaskedOverlay(depth, overlayScreenX, overlayScreenY);
            if (void* maskTex = depth.maskedOverlayTexture.GetID()) {
                const float half = depth.overlaySize * 0.5f;
                ImGui::GetBackgroundDrawList()->AddImage(
                    maskTex,
                    ImVec2(overlayScreenX - half, overlayScreenY - half),
                    ImVec2(overlayScreenX + half, overlayScreenY + half));
            }
        }

        // Control panel
        ImGui::Begin("World space", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Separator();

        ImGui::Checkbox("Enable grid", &config.enabled);

        if (config.enabled) {
            ImGui::Spacing();
            ImGui::Text("Grid Configuration");
            ImGui::Separator();

            ImGui::SliderInt("Spacing", &config.gridSpacing, 8, 256);
            ImGui::SliderInt("Extent", &config.gridExtent, 64, 2048);
            ImGui::SliderFloat("Line thickness", &config.lineThickness, 1.0f, 5.0f, "%.1f");

            ImGui::Spacing();
            ImGui::Text("Grid center");
            ImGui::Separator();

            ImGui::DragFloat("Center X", &config.centerX, 1.0f, 0.0f, 4096.0f, "%.1f");
            ImGui::DragFloat("Center Y (Height)", &config.centerY, 0.5f, -100.0f, 500.0f, "%.1f");
            ImGui::DragFloat("Center Z", &config.centerZ, 1.0f, 0.0f, 4096.0f, "%.1f");

            ImGui::Spacing();
            ImGui::Text("Appearance");
            ImGui::Separator();

            ImGui::ColorEdit4("Color", reinterpret_cast<float*>(&config.gridColor));

            ImGui::Spacing();
            ImGui::Checkbox("Draw marker", &config.drawCenterMarker);
            if (config.drawCenterMarker) {
                ImGui::SliderFloat("Marker size", &config.markerSize, 5.0f, 30.0f, "%.1f");
            }

            ImGui::Spacing();
            ImGui::Text("Terrain conform");
            ImGui::Separator();
            ImGui::Checkbox("Conform to terrain", &config.conformToTerrain);
            if (config.conformToTerrain) {
                ImGui::Checkbox("Snap to grid", &config.terrainSnapToGrid);
                ImGui::SliderInt("Sample step (m)", &config.terrainSampleStep, 4, 64);
            }

            ImGui::Spacing();
            ImGui::Text("World text");
            ImGui::Separator();

            ImGui::Checkbox("Draw text", &config.drawText);
            if (config.drawText) {
                ImGui::InputText("Text", config.text, IM_ARRAYSIZE(config.text));
                ImGui::Checkbox("Billboard", &config.textBillboard);
                if (!config.textBillboard) {
                    ImGui::SliderFloat("Depth scale", &config.textDepthScale, 0.0005f, 0.01f, "%.4f");
                }
                ImGui::DragFloat2("Text offset", &config.textOffsetX, 1.0f, -200.0f, 200.0f, "%.1f");
                ImGui::ColorEdit4("Text color", reinterpret_cast<float*>(&config.textColor));
                ImGui::Checkbox("Leader line", &config.textLeaderLine);
                ImGui::Checkbox("Background plate", &config.textBackground);
                ImGui::Checkbox("Outline", &config.textOutline);
                ImGui::Checkbox("Shadow", &config.textShadow);
            }

            ImGui::Spacing();
            ImGui::Text("Billboard image");
            ImGui::Separator();

            ImGui::Checkbox("Draw image", &config.drawImage);
            if (config.drawImage) {
                ImGui::Text("Image: nam49.jpg");
                ImGui::Text("Mode");
                if (ImGui::RadioButton("Billboard (pixels)", config.imageBillboard)) {
                    config.imageBillboard = true;
                }
                if (ImGui::RadioButton("Planar (world units)", !config.imageBillboard)) {
                    config.imageBillboard = false;
                }
                ImGui::SliderFloat("Image size", &config.imageSize, 16.0f, 256.0f, "%.1f");
                if (config.imageBillboard) {
                    ImGui::DragFloat2("Image offset (px)", &config.imageOffsetX, 1.0f, -200.0f, 200.0f, "%.1f");
                }
                else {
                    ImGui::DragFloat2("Image offset (world X/Z)", &config.imageOffsetX, 1.0f, -512.0f, 512.0f, "%.1f");
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Depth buffer");
        ImGui::Checkbox("Auto refresh each frame", &depth.autoRefresh);
        ImGui::SameLine();
        if (ImGui::Button("Capture now")) {
            CaptureDepthBuffer(depth);
        }
        ImGui::TextColored(depth.lastCaptureOk ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "%s", depth.status);
        ImGui::Text("Min depth: %.0f   Max depth: %.0f", depth.minDepth, depth.maxDepth);

        if (!depth.histogram.empty()) {
            ImGui::PlotHistogram("Depth histogram", depth.histogram.data(),
                                 static_cast<int>(depth.histogram.size()), 0,
                                 nullptr, 0.0f, 1.0f, ImVec2(0, 80));
        }

        if (void* texId = depth.depthTexture.GetID()) {
            const float aspect = depth.texHeight > 0
                                     ? static_cast<float>(depth.texHeight) / static_cast<float>(depth.texWidth)
                                     : 1.0f;
            const float displayWidth = 512.0f;
            ImGui::Image(texId, ImVec2(displayWidth, displayWidth * aspect));
        }
        else {
            ImGui::TextUnformatted("Depth texture not available yet.");
        }

        ImGui::Spacing();
        ImGui::Text("Masked overlay PoC");
        ImGui::Checkbox("Show masked overlay in scene", &depth.showMaskedOverlay);
        ImGui::SliderFloat("Overlay size (px)", &depth.overlaySize, 32.0f, 256.0f, "%.0f");
        ImGui::SliderFloat("Depth bias", &depth.overlayBias, 0.0f, 400.0f, "%.0f");
        ImGui::ColorEdit4("Overlay color", reinterpret_cast<float*>(&depth.overlayColor));
        if (void* masked = depth.maskedOverlayTexture.GetID()) {
            ImGui::TextUnformatted("Preview:");
            ImGui::Image(masked, ImVec2(depth.overlaySize, depth.overlaySize));
        }
        else {
            ImGui::TextUnformatted("Preview: (capture first)");
        }

        ImGui::End();
    }

    void ShutdownWorldProjection(void* userData) {
        auto* data = static_cast<WorldProjectionData*>(userData);
        if (data) {
            data->grid.imageTexture.Release();
            data->depth.depthTexture.Release();
            data->depth.maskedOverlayTexture.Release();
            if (data->cameraService) {
                data->cameraService->Release();
                data->cameraService = nullptr;
            }
            gCameraService = nullptr;
            gCameraHandle = {nullptr, 0, false};
        }
        delete data;
    }
}

class WorldProjectionSampleDirector final : public cRZCOMDllDirector {
public:
    WorldProjectionSampleDirector()
        : service(nullptr),
          panelRegistered(false) {}

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kWorldProjectionSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4WorldProjectionSample", "");
        LOG_INFO("WorldProjectionSample: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("WorldProjectionSample: framework hook added");
        }
        else {
            LOG_WARN("WorldProjectionSample: mpFrameWork not available on start");
        }

        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("WorldProjectionSample: PostAppInit");

        if (!mpFrameWork || panelRegistered) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&service))) {
            LOG_WARN("WorldProjectionSample: ImGui service not available");
            return true;
        }

        // Camera service (optional; 641-only)
        cIGZS3DCameraService* cameraService = nullptr;
        if (!mpFrameWork->GetSystemService(kS3DCameraServiceID, GZIID_cIGZS3DCameraService,
                                           reinterpret_cast<void**>(&cameraService))) {
            LOG_WARN("WorldProjectionSample: Camera service not available");
        }

        LOG_INFO("WorldProjectionSample: obtained ImGui service");

        auto* data = new WorldProjectionData();
        data->grid.imguiService = service;
        data->depth.imguiService = service;
        data->cameraService = cameraService;
        ImGuiPanelDesc desc{};
        desc.id = kWorldProjectionPanelId;
        desc.order = 200; // Render after other panels
        desc.visible = true;
        desc.on_render = &RenderWorldProjectionPanel;
        desc.on_shutdown = &ShutdownWorldProjection;
        desc.data = data;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("WorldProjectionSample: failed to register panel");
            ShutdownWorldProjection(data);
            service->Release();
            service = nullptr;
            return true;
        }

        LOG_INFO("WorldProjectionSample: registered panel {}", kWorldProjectionPanelId);
        panelRegistered = true;
        if (!StartGdiplus()) {
            LOG_WARN("WorldProjectionSample: failed to start GDI+");
        }
        return true;
    }

    bool PostAppShutdown() override {
        if (service) {
            service->UnregisterPanel(kWorldProjectionPanelId);
            service->Release();
            service = nullptr;
        }
        StopGdiplus();
        panelRegistered = false;
        return true;
    }

private:
    cIGZImGuiService* service;
    bool panelRegistered;
};

static WorldProjectionSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
