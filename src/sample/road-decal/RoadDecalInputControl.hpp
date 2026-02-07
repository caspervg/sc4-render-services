#pragma once

#include "RoadDecalData.hpp"
#include "cSC4BaseViewInputControl.h"

#include <cstdint>
#include <functional>

class RoadDecalInputControl final : public cSC4BaseViewInputControl
{
public:
    static constexpr uint32_t kRoadDecalControlID = 0x8A3FDECA;

    RoadDecalInputControl();
    ~RoadDecalInputControl() override;

    bool Init() override;
    bool Shutdown() override;

    void Activate() override;
    void Deactivate() override;

    bool OnMouseDownL(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseUpL(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseMove(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseDownR(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseExit() override;
    bool OnKeyDown(int32_t vkCode, uint32_t modifiers) override;

    void SetMarkupType(RoadMarkupType type);
    void SetPlacementMode(PlacementMode mode);
    void SetWidth(float width);
    void SetLength(float length);
    void SetRotation(float radians);
    void SetDashed(bool dashed);
    void SetDashPattern(float dashLength, float gapLength);
    void SetAutoAlign(bool enabled);
    void SetOnCancel(std::function<void()> onCancel);

private:
    bool BeginStroke_(int32_t screenX, int32_t screenZ, uint32_t modifiers);
    bool AddSamplePoint_(int32_t screenX, int32_t screenZ, uint32_t modifiers);
    bool EndStroke_(bool commit);
    void CancelStroke_();
    bool PickWorld_(int32_t screenX, int32_t screenZ, RoadDecalPoint& outPoint);
    void UpdatePreviewFromScreen_(int32_t screenX, int32_t screenZ);
    void ClearPreview_();
    void RefreshActiveStroke_();
    void RequestFullRedraw_();
    void UndoLastStroke_();
    void ClearAllStrokes_();

private:
    bool isActive_;
    bool isDrawing_;

    RoadMarkupStroke currentStroke_;
    RoadDecalPoint lastSamplePoint_;
    RoadMarkupType markupType_;
    PlacementMode placementMode_;
    float width_;
    float length_;
    float rotation_;
    bool dashed_;
    float dashLength_;
    float gapLength_;
    bool autoAlign_;

    std::function<void()> onCancel_;
};
