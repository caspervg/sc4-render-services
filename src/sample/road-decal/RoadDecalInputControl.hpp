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

    void SetStyle(int styleId);
    void SetWidth(float width);
    void SetOnCancel(std::function<void()> onCancel);

private:
    bool BeginStroke_(int32_t screenX, int32_t screenZ);
    bool AddSamplePoint_(int32_t screenX, int32_t screenZ);
    bool EndStroke_(bool commit);
    void CancelStroke_();
    bool PickWorld_(int32_t screenX, int32_t screenZ, RoadDecalPoint& outPoint);
    void UpdatePreviewFromScreen_(int32_t screenX, int32_t screenZ);
    void ClearPreview_();
    void RefreshActiveStroke_();
    void UndoLastStroke_();
    void ClearAllStrokes_();

private:
    bool isActive_;
    bool isDrawing_;

    RoadDecalStroke currentStroke_;
    RoadDecalPoint lastSamplePoint_;
    int styleId_;
    float width_;

    std::function<void()> onCancel_;
};
