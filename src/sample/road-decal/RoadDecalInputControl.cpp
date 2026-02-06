#include "RoadDecalInputControl.hpp"

#include "utils/Logger.h"

#include <windows.h>

namespace
{
    constexpr uint32_t kControlModifierMask = 0x20000;
}

RoadDecalInputControl::RoadDecalInputControl()
    : cSC4BaseViewInputControl(kRoadDecalControlID)
    , isActive_(false)
    , isDrawing_(false)
    , currentStroke_({})
    , lastSamplePoint_({})
    , styleId_(0)
    , width_(0.5f)
    , onCancel_()
{
}

RoadDecalInputControl::~RoadDecalInputControl()
{
    LOG_INFO("RoadDecalInputControl destroyed");
}

bool RoadDecalInputControl::Init()
{
    if (!cSC4BaseViewInputControl::Init()) {
        return false;
    }
    LOG_INFO("RoadDecalInputControl initialized");
    return true;
}

bool RoadDecalInputControl::Shutdown()
{
    LOG_INFO("RoadDecalInputControl shutting down");
    CancelStroke_();
    return cSC4BaseViewInputControl::Shutdown();
}

void RoadDecalInputControl::Activate()
{
    cSC4BaseViewInputControl::Activate();
    if (!Init()) {
        LOG_WARN("RoadDecalInputControl: Init failed during Activate");
        return;
    }
    isActive_ = true;
    LOG_INFO("RoadDecalInputControl activated");
}

void RoadDecalInputControl::Deactivate()
{
    LOG_INFO("RoadDecalInputControl deactivated");
    isActive_ = false;
    CancelStroke_();
    cSC4BaseViewInputControl::Deactivate();
}

bool RoadDecalInputControl::OnMouseDownL(int32_t x, int32_t z, uint32_t)
{
    if (!isActive_ || !IsOnTop()) {
        return false;
    }

    if (!isDrawing_) {
        return BeginStroke_(x, z);
    }

    return AddSamplePoint_(x, z);
}

bool RoadDecalInputControl::OnMouseMove(int32_t x, int32_t z, uint32_t)
{
    if (!isActive_ || !isDrawing_) {
        return false;
    }

    UpdatePreviewFromScreen_(x, z);
    return true;
}

bool RoadDecalInputControl::OnMouseUpL(int32_t x, int32_t z, uint32_t)
{
    (void)x;
    (void)z;
    return false;
}

bool RoadDecalInputControl::OnMouseDownR(int32_t, int32_t, uint32_t)
{
    if (!isActive_) {
        return false;
    }

    if (isDrawing_) {
        EndStroke_(true);
    } else {
        ClearAllStrokes_();
        RebuildRoadDecalGeometry();
    }

    return true;
}

bool RoadDecalInputControl::OnMouseExit()
{
    ClearPreview_();
    return false;
}

bool RoadDecalInputControl::OnKeyDown(int32_t vkCode, uint32_t modifiers)
{
    if (!isActive_) {
        return false;
    }

    if (vkCode == VK_ESCAPE) {
        CancelStroke_();
        LOG_INFO("RoadDecalInputControl: ESC pressed, canceling");
        if (onCancel_) {
            onCancel_();
        }
        return true;
    }

    if (vkCode == 'Z' && (modifiers & kControlModifierMask) != 0) {
        UndoLastStroke_();
        RebuildRoadDecalGeometry();
        return true;
    }

    if (vkCode == VK_DELETE) {
        CancelStroke_();
        ClearAllStrokes_();
        RebuildRoadDecalGeometry();
        return true;
    }

    if (vkCode == VK_RETURN && isDrawing_) {
        return EndStroke_(true);
    }

    return false;
}

void RoadDecalInputControl::SetStyle(const int styleId)
{
    styleId_ = styleId;
}

void RoadDecalInputControl::SetWidth(float width)
{
    if (width < 0.05f) {
        width = 0.05f;
    }
    width_ = width;
}

void RoadDecalInputControl::SetOnCancel(std::function<void()> onCancel)
{
    onCancel_ = std::move(onCancel);
}

bool RoadDecalInputControl::PickWorld_(int32_t screenX, int32_t screenZ, RoadDecalPoint& outPoint)
{
    if (!view3D) {
        LOG_WARN("RoadDecalInputControl: view3D not available");
        return false;
    }

    float worldCoords[3] = {0.0f, 0.0f, 0.0f};
    if (!view3D->PickTerrain(screenX, screenZ, worldCoords, false)) {
        return false;
    }

    outPoint.x = worldCoords[0];
    outPoint.y = worldCoords[1];
    outPoint.z = worldCoords[2];
    return true;
}

bool RoadDecalInputControl::BeginStroke_(int32_t screenX, int32_t screenZ)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        return false;
    }

    if (!SetCapture()) {
        LOG_WARN("RoadDecalInputControl: failed to SetCapture");
        return false;
    }

    currentStroke_.points.clear();
    currentStroke_.styleId = styleId_;
    currentStroke_.width = width_;
    currentStroke_.points.push_back(p);
    lastSamplePoint_ = p;
    isDrawing_ = true;
    RefreshActiveStroke_();
    ClearPreview_();
    return true;
}

bool RoadDecalInputControl::AddSamplePoint_(int32_t screenX, int32_t screenZ)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        return false;
    }

    const float dx = p.x - lastSamplePoint_.x;
    const float dy = p.y - lastSamplePoint_.y;
    const float dz = p.z - lastSamplePoint_.z;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    constexpr float kMinSampleDist = 0.5f;

    if (dist2 < kMinSampleDist * kMinSampleDist) {
        return false;
    }

    currentStroke_.points.push_back(p);
    lastSamplePoint_ = p;
    RefreshActiveStroke_();
    ClearPreview_();
    return true;
}

bool RoadDecalInputControl::EndStroke_(bool commit)
{
    if (commit && currentStroke_.points.size() >= 2) {
        gRoadDecalStrokes.push_back(currentStroke_);
        RebuildRoadDecalGeometry();
    }

    currentStroke_.points.clear();
    isDrawing_ = false;
    SetRoadDecalActiveStroke(nullptr);
    ClearPreview_();
    ReleaseCapture();
    return true;
}

void RoadDecalInputControl::CancelStroke_()
{
    if (!isDrawing_) {
        return;
    }
    currentStroke_.points.clear();
    isDrawing_ = false;
    SetRoadDecalActiveStroke(nullptr);
    ClearPreview_();
    ReleaseCapture();
}

void RoadDecalInputControl::UpdatePreviewFromScreen_(int32_t screenX, int32_t screenZ)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        ClearPreview_();
        return;
    }

    SetRoadDecalPreviewSegment(true, lastSamplePoint_, p, currentStroke_.styleId, currentStroke_.width);
}

void RoadDecalInputControl::ClearPreview_()
{
    const RoadDecalPoint zero{0.0f, 0.0f, 0.0f};
    SetRoadDecalPreviewSegment(false, zero, zero, 0, 0.0f);
}

void RoadDecalInputControl::RefreshActiveStroke_()
{
    SetRoadDecalActiveStroke(&currentStroke_);
}

void RoadDecalInputControl::UndoLastStroke_()
{
    if (gRoadDecalStrokes.empty()) {
        return;
    }
    gRoadDecalStrokes.pop_back();
}

void RoadDecalInputControl::ClearAllStrokes_()
{
    if (gRoadDecalStrokes.empty()) {
        return;
    }
    gRoadDecalStrokes.clear();
}
