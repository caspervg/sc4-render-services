#include "ImGuiThemes.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "imgui.h"

// The color themes are based on the ones TheAncientOwl shared for Dear ImGui:
// https://github.com/ocornut/imgui/issues/707#issuecomment-4107169777
// MakeControlsVisible adapts each of them, because their frames and borders are hard to see.

namespace
{
    struct ThemeColor
    {
        ImGuiCol id;
        ImVec4 value;
    };

    struct Rounding
    {
        float window, child, frame, popup, scrollbar, grab, tab;
    };

    struct Theme
    {
        std::string_view name;
        void (*base)(ImGuiStyle*);
        std::span<const ThemeColor> colors;
        Rounding rounding;
    };

    const ThemeColor kForestGreen[] = {
        {ImGuiCol_Text, ImVec4(0.85f, 0.90f, 0.85f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.50f, 0.55f, 0.50f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.06f, 0.09f, 0.06f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.08f, 0.11f, 0.08f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.07f, 0.10f, 0.07f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.18f, 0.28f, 0.18f, 0.80f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.12f, 0.18f, 0.12f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.30f, 0.18f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.24f, 0.42f, 0.24f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.09f, 0.14f, 0.09f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.14f, 0.26f, 0.14f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.05f, 0.08f, 0.05f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.09f, 0.14f, 0.09f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.05f, 0.08f, 0.05f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.18f, 0.28f, 0.18f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.25f, 0.38f, 0.25f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.32f, 0.48f, 0.32f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.45f, 0.75f, 0.45f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.35f, 0.55f, 0.35f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.45f, 0.70f, 0.45f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.18f, 0.35f, 0.18f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.45f, 0.25f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.32f, 0.55f, 0.32f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.18f, 0.35f, 0.18f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.45f, 0.25f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.32f, 0.55f, 0.32f, 1.00f)},
        {ImGuiCol_Separator, ImVec4(0.18f, 0.28f, 0.18f, 1.00f)},
        {ImGuiCol_SeparatorHovered, ImVec4(0.25f, 0.45f, 0.25f, 1.00f)},
        {ImGuiCol_SeparatorActive, ImVec4(0.32f, 0.55f, 0.32f, 1.00f)},
        {ImGuiCol_ResizeGrip, ImVec4(0.18f, 0.35f, 0.18f, 0.80f)},
        {ImGuiCol_ResizeGripHovered, ImVec4(0.25f, 0.45f, 0.25f, 1.00f)},
        {ImGuiCol_ResizeGripActive, ImVec4(0.32f, 0.55f, 0.32f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.12f, 0.22f, 0.12f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.25f, 0.45f, 0.25f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.20f, 0.38f, 0.20f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.08f, 0.15f, 0.08f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.12f, 0.22f, 0.12f, 1.00f)},
        {ImGuiCol_PlotLines, ImVec4(0.40f, 0.70f, 0.40f, 1.00f)},
        {ImGuiCol_PlotLinesHovered, ImVec4(0.50f, 0.85f, 0.50f, 1.00f)},
        {ImGuiCol_PlotHistogram, ImVec4(0.40f, 0.70f, 0.40f, 1.00f)},
        {ImGuiCol_PlotHistogramHovered, ImVec4(0.50f, 0.85f, 0.50f, 1.00f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.12f, 0.22f, 0.12f, 1.00f)},
        {ImGuiCol_TableBorderStrong, ImVec4(0.20f, 0.35f, 0.20f, 1.00f)},
        {ImGuiCol_TableBorderLight, ImVec4(0.15f, 0.25f, 0.15f, 1.00f)},
        {ImGuiCol_TableRowBg, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(0.08f, 0.14f, 0.08f, 0.50f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.25f, 0.55f, 0.25f, 0.50f)},
        {ImGuiCol_DragDropTarget, ImVec4(0.60f, 0.90f, 0.60f, 1.00f)},
        {ImGuiCol_NavCursor, ImVec4(0.40f, 0.80f, 0.40f, 1.00f)},
        {ImGuiCol_NavWindowingHighlight, ImVec4(0.85f, 0.90f, 0.85f, 0.70f)},
        {ImGuiCol_NavWindowingDimBg, ImVec4(0.10f, 0.15f, 0.10f, 0.50f)},
        {ImGuiCol_ModalWindowDimBg, ImVec4(0.05f, 0.08f, 0.05f, 0.60f)},
        {ImGuiCol_DockingPreview, ImVec4(0.25f, 0.55f, 0.25f, 0.50f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.06f, 0.09f, 0.06f, 1.00f)},
    };

    const ThemeColor kAmethyst[] = {
        {ImGuiCol_Text, ImVec4(0.92f, 0.90f, 0.95f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.55f, 0.50f, 0.60f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.09f, 0.07f, 0.12f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.11f, 0.09f, 0.14f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.09f, 0.07f, 0.12f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.25f, 0.20f, 0.35f, 0.80f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.15f, 0.12f, 0.22f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.20f, 0.38f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.25f, 0.55f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.12f, 0.09f, 0.18f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.20f, 0.14f, 0.32f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.07f, 0.05f, 0.10f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.12f, 0.09f, 0.18f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.07f, 0.05f, 0.10f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.25f, 0.20f, 0.35f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.35f, 0.30f, 0.50f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.45f, 0.40f, 0.65f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.65f, 0.45f, 0.95f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.50f, 0.35f, 0.75f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.65f, 0.45f, 0.95f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.25f, 0.20f, 0.40f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.28f, 0.62f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.50f, 0.35f, 0.80f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.25f, 0.20f, 0.40f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.38f, 0.28f, 0.62f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.50f, 0.35f, 0.80f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.15f, 0.12f, 0.25f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.38f, 0.28f, 0.62f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.28f, 0.20f, 0.45f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.10f, 0.08f, 0.15f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.15f, 0.12f, 0.25f, 1.00f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.18f, 0.15f, 0.28f, 1.00f)},
        {ImGuiCol_TableBorderStrong, ImVec4(0.25f, 0.20f, 0.40f, 1.00f)},
        {ImGuiCol_TableBorderLight, ImVec4(0.20f, 0.15f, 0.30f, 1.00f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(1.00f, 1.00f, 1.00f, 0.04f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.50f, 0.35f, 0.80f, 0.35f)},
        {ImGuiCol_DragDropTarget, ImVec4(0.80f, 0.65f, 1.00f, 0.95f)},
        {ImGuiCol_NavCursor, ImVec4(0.60f, 0.45f, 0.90f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.50f, 0.35f, 0.80f, 0.50f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.09f, 0.07f, 0.12f, 1.00f)},
    };

    const ThemeColor kSapphire[] = {
        {ImGuiCol_Text, ImVec4(0.90f, 0.93f, 0.97f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.40f, 0.50f, 0.65f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.07f, 0.09f, 0.12f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.09f, 0.12f, 0.16f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.07f, 0.09f, 0.12f, 0.95f)},
        {ImGuiCol_Border, ImVec4(0.15f, 0.25f, 0.35f, 0.70f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.12f, 0.18f, 0.26f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.28f, 0.40f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.38f, 0.55f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.09f, 0.12f, 0.18f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.14f, 0.22f, 0.35f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.05f, 0.08f, 0.12f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.12f, 0.16f, 0.22f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.06f, 0.08f, 0.11f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.20f, 0.32f, 0.48f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.28f, 0.42f, 0.60f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.35f, 0.50f, 0.75f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.40f, 0.70f, 1.00f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.30f, 0.55f, 0.85f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.45f, 0.75f, 1.00f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.18f, 0.35f, 0.55f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.48f, 0.75f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.35f, 0.60f, 0.90f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.18f, 0.35f, 0.55f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.48f, 0.75f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.35f, 0.60f, 0.90f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.12f, 0.20f, 0.32f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.25f, 0.45f, 0.70f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.18f, 0.35f, 0.55f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.08f, 0.12f, 0.18f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.12f, 0.20f, 0.32f, 1.00f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.15f, 0.25f, 0.40f, 1.00f)},
        {ImGuiCol_TableBorderStrong, ImVec4(0.20f, 0.35f, 0.55f, 1.00f)},
        {ImGuiCol_TableBorderLight, ImVec4(0.15f, 0.25f, 0.40f, 1.00f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(1.00f, 1.00f, 1.00f, 0.05f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.30f, 0.55f, 0.85f, 0.40f)},
        {ImGuiCol_DragDropTarget, ImVec4(0.50f, 0.80f, 1.00f, 0.90f)},
        {ImGuiCol_NavCursor, ImVec4(0.40f, 0.70f, 1.00f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.25f, 0.50f, 0.80f, 0.50f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.07f, 0.09f, 0.12f, 1.00f)},
    };

    const ThemeColor kAmberYellow[] = {
        {ImGuiCol_Text, ImVec4(1.00f, 0.95f, 0.80f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.50f, 0.45f, 0.30f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.06f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.08f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.06f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.30f, 0.25f, 0.10f, 0.80f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.15f, 0.14f, 0.10f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.22f, 0.12f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.30f, 0.15f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.12f, 0.11f, 0.08f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.20f, 0.18f, 0.10f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.05f, 0.05f, 0.04f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.12f, 0.11f, 0.08f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.05f, 0.05f, 0.04f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.35f, 0.30f, 0.10f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.45f, 0.40f, 0.15f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.55f, 0.50f, 0.20f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.95f, 0.80f, 0.10f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.70f, 0.60f, 0.10f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.95f, 0.80f, 0.10f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.30f, 0.25f, 0.05f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.38f, 0.10f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.60f, 0.50f, 0.15f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.30f, 0.25f, 0.05f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.45f, 0.38f, 0.10f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.60f, 0.50f, 0.15f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.15f, 0.14f, 0.10f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.45f, 0.38f, 0.10f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.35f, 0.30f, 0.10f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.08f, 0.08f, 0.07f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.15f, 0.14f, 0.10f, 1.00f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.18f, 0.16f, 0.10f, 1.00f)},
        {ImGuiCol_TableBorderStrong, ImVec4(0.35f, 0.30f, 0.15f, 1.00f)},
        {ImGuiCol_TableBorderLight, ImVec4(0.25f, 0.20f, 0.10f, 1.00f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(1.00f, 1.00f, 1.00f, 0.03f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.95f, 0.80f, 0.10f, 0.25f)},
        {ImGuiCol_DragDropTarget, ImVec4(1.00f, 0.85f, 0.00f, 0.90f)},
        {ImGuiCol_NavCursor, ImVec4(0.95f, 0.80f, 0.10f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.95f, 0.80f, 0.10f, 0.40f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.07f, 0.07f, 0.06f, 1.00f)},
    };

    const ThemeColor kDracula[] = {
        {ImGuiCol_Text, ImVec4(0.97f, 0.97f, 0.95f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.38f, 0.45f, 0.64f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.16f, 0.16f, 0.21f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.16f, 0.16f, 0.21f, 0.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.16f, 0.16f, 0.21f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.38f, 0.45f, 0.64f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.48f, 0.55f, 0.74f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.13f, 0.14f, 0.18f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.16f, 0.16f, 0.21f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.13f, 0.14f, 0.18f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.13f, 0.14f, 0.18f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.16f, 0.16f, 0.21f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.38f, 0.45f, 0.64f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.48f, 0.55f, 0.74f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.31f, 0.98f, 0.48f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.74f, 0.58f, 0.98f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.84f, 0.68f, 1.00f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.47f, 0.78f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.80f, 0.37f, 0.62f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.38f, 0.45f, 0.64f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.48f, 0.55f, 0.74f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.16f, 0.16f, 0.21f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.13f, 0.14f, 0.18f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.16f, 0.16f, 0.21f, 1.00f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_TableBorderStrong, ImVec4(0.38f, 0.45f, 0.64f, 1.00f)},
        {ImGuiCol_TableBorderLight, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_PlotLines, ImVec4(0.55f, 0.91f, 0.99f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.27f, 0.28f, 0.35f, 1.00f)},
        {ImGuiCol_NavCursor, ImVec4(0.74f, 0.58f, 0.98f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.74f, 0.58f, 0.98f, 0.50f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.16f, 0.16f, 0.21f, 1.00f)},
    };

    const ThemeColor kCatppuccinMocha[] = {
        {ImGuiCol_Text, ImVec4(0.80f, 0.84f, 0.96f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.42f, 0.45f, 0.55f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.18f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.15f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.11f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.19f, 0.20f, 0.27f, 1.00f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.19f, 0.20f, 0.27f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.26f, 0.35f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.31f, 0.32f, 0.42f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.09f, 0.09f, 0.15f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.12f, 0.12f, 0.18f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.07f, 0.07f, 0.11f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.09f, 0.09f, 0.15f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.09f, 0.09f, 0.15f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.31f, 0.32f, 0.42f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.37f, 0.38f, 0.51f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.42f, 0.45f, 0.55f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.71f, 0.75f, 1.00f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.45f, 0.78f, 0.93f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.45f, 0.78f, 0.93f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.19f, 0.20f, 0.27f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.65f, 0.97f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.70f, 0.55f, 0.87f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.19f, 0.20f, 0.27f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.26f, 0.35f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.31f, 0.32f, 0.42f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.12f, 0.12f, 0.18f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.31f, 0.32f, 0.42f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.19f, 0.20f, 0.27f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.09f, 0.09f, 0.15f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.12f, 0.12f, 0.18f, 1.00f)},
        {ImGuiCol_PlotLines, ImVec4(0.94f, 0.72f, 0.42f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.31f, 0.32f, 0.42f, 1.00f)},
        {ImGuiCol_NavCursor, ImVec4(0.71f, 0.75f, 1.00f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.71f, 0.75f, 1.00f, 0.50f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.12f, 0.12f, 0.18f, 1.00f)},
    };

    const ThemeColor kGruvboxHard[] = {
        {ImGuiCol_Text, ImVec4(0.92f, 0.86f, 0.70f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.57f, 0.51f, 0.45f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.11f, 0.13f, 0.13f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.11f, 0.13f, 0.13f, 0.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.11f, 0.13f, 0.13f, 0.95f)},
        {ImGuiCol_Border, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.24f, 0.22f, 0.21f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.40f, 0.36f, 0.33f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.11f, 0.13f, 0.13f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.11f, 0.13f, 0.13f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.11f, 0.13f, 0.13f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.15f, 0.14f, 0.13f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.11f, 0.13f, 0.13f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.40f, 0.36f, 0.33f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.57f, 0.51f, 0.45f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.72f, 0.73f, 0.15f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.51f, 0.65f, 0.60f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.55f, 0.73f, 0.67f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.29f, 0.20f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.80f, 0.20f, 0.15f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.24f, 0.22f, 0.21f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.40f, 0.36f, 0.33f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.24f, 0.22f, 0.21f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_PlotLines, ImVec4(0.98f, 0.74f, 0.18f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.31f, 0.29f, 0.27f, 1.00f)},
        {ImGuiCol_NavCursor, ImVec4(0.98f, 0.29f, 0.20f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.72f, 0.73f, 0.15f, 0.50f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.11f, 0.13f, 0.13f, 1.00f)},
    };

    const ThemeColor kCrimsonVesuvius[] = {
        {ImGuiCol_Text, ImVec4(1.00f, 0.90f, 0.90f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.50f, 0.40f, 0.40f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.08f, 0.07f, 0.07f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.10f, 0.09f, 0.09f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.08f, 0.07f, 0.07f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.25f, 0.15f, 0.15f, 0.80f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.15f, 0.10f, 0.10f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.15f, 0.15f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.20f, 0.20f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.12f, 0.08f, 0.08f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.25f, 0.10f, 0.10f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.05f, 0.05f, 0.05f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.12f, 0.08f, 0.08f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.05f, 0.05f, 0.05f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.25f, 0.12f, 0.12f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.35f, 0.15f, 0.15f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.45f, 0.20f, 0.20f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.85f, 0.15f, 0.15f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.60f, 0.12f, 0.12f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.85f, 0.15f, 0.15f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.30f, 0.12f, 0.12f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.18f, 0.18f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.70f, 0.25f, 0.25f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.30f, 0.12f, 0.12f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.50f, 0.18f, 0.18f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.70f, 0.25f, 0.25f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.15f, 0.10f, 0.10f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.50f, 0.18f, 0.18f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.35f, 0.12f, 0.12f, 1.00f)},
        {ImGuiCol_PlotLines, ImVec4(0.85f, 0.20f, 0.20f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.85f, 0.15f, 0.15f, 0.35f)},
        {ImGuiCol_NavCursor, ImVec4(0.85f, 0.15f, 0.15f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.85f, 0.15f, 0.15f, 0.40f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.08f, 0.07f, 0.07f, 1.00f)},
    };

    const ThemeColor kRoseQuartz[] = {
        {ImGuiCol_Text, ImVec4(0.95f, 0.90f, 0.95f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.55f, 0.45f, 0.55f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.12f, 0.10f, 0.12f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.14f, 0.12f, 0.14f, 1.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.10f, 0.08f, 0.10f, 0.96f)},
        {ImGuiCol_Border, ImVec4(0.35f, 0.25f, 0.35f, 0.50f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_FrameBg, ImVec4(0.20f, 0.15f, 0.20f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.30f, 0.22f, 0.30f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.40f, 0.28f, 0.40f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.15f, 0.10f, 0.15f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.25f, 0.15f, 0.25f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.08f, 0.06f, 0.08f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.15f, 0.10f, 0.15f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.08f, 0.06f, 0.08f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.40f, 0.25f, 0.40f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.55f, 0.35f, 0.55f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.70f, 0.45f, 0.70f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.95f, 0.60f, 0.75f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.85f, 0.50f, 0.65f, 1.00f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.95f, 0.60f, 0.75f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.45f, 0.25f, 0.35f, 1.00f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.35f, 0.50f, 1.00f)},
        {ImGuiCol_ButtonActive, ImVec4(0.85f, 0.45f, 0.65f, 1.00f)},
        {ImGuiCol_Header, ImVec4(0.45f, 0.25f, 0.35f, 1.00f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.55f, 0.30f, 0.45f, 1.00f)},
        {ImGuiCol_HeaderActive, ImVec4(0.65f, 0.35f, 0.55f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.20f, 0.15f, 0.20f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(0.65f, 0.35f, 0.50f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(0.45f, 0.25f, 0.35f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.95f, 0.60f, 0.75f, 0.35f)},
        {ImGuiCol_NavCursor, ImVec4(0.95f, 0.60f, 0.75f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.95f, 0.60f, 0.75f, 0.40f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.12f, 0.10f, 0.12f, 1.00f)},
    };

    const ThemeColor kCyberpunk[] = {
        {ImGuiCol_Text, ImVec4(0.00f, 1.00f, 0.62f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.20f, 0.40f, 0.35f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.04f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.02f, 0.02f, 0.04f, 0.00f)},
        {ImGuiCol_PopupBg, ImVec4(0.02f, 0.02f, 0.04f, 0.98f)},
        {ImGuiCol_Border, ImVec4(1.00f, 0.00f, 0.25f, 0.60f)},
        {ImGuiCol_BorderShadow, ImVec4(1.00f, 0.00f, 0.25f, 0.20f)},
        {ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.10f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(1.00f, 0.00f, 0.25f, 0.20f)},
        {ImGuiCol_FrameBgActive, ImVec4(1.00f, 0.00f, 0.25f, 0.40f)},
        {ImGuiCol_TitleBg, ImVec4(0.02f, 0.02f, 0.04f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.05f, 0.05f, 0.10f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.02f, 0.02f, 0.04f, 1.00f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.05f, 0.05f, 0.10f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.02f, 0.02f, 0.04f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(1.00f, 0.93f, 0.04f, 0.60f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(1.00f, 0.93f, 0.04f, 0.80f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(1.00f, 0.93f, 0.04f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(1.00f, 0.93f, 0.04f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(1.00f, 0.00f, 0.25f, 0.80f)},
        {ImGuiCol_SliderGrabActive, ImVec4(1.00f, 0.00f, 0.25f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.00f, 1.00f, 0.62f, 0.20f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.00f, 1.00f, 0.62f, 0.50f)},
        {ImGuiCol_ButtonActive, ImVec4(0.00f, 1.00f, 0.62f, 1.00f)},
        {ImGuiCol_Header, ImVec4(1.00f, 0.00f, 0.25f, 0.30f)},
        {ImGuiCol_HeaderHovered, ImVec4(1.00f, 0.00f, 0.25f, 0.50f)},
        {ImGuiCol_HeaderActive, ImVec4(1.00f, 0.00f, 0.25f, 1.00f)},
        {ImGuiCol_Tab, ImVec4(0.05f, 0.05f, 0.10f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(1.00f, 0.00f, 0.25f, 0.80f)},
        {ImGuiCol_TabSelected, ImVec4(0.80f, 0.00f, 0.20f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(1.00f, 0.93f, 0.04f, 0.30f)},
        {ImGuiCol_NavCursor, ImVec4(1.00f, 0.00f, 0.25f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.00f, 1.00f, 0.62f, 0.40f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.02f, 0.02f, 0.04f, 1.00f)},
    };

    const ThemeColor kPaperAndInk[] = {
        {ImGuiCol_Text, ImVec4(0.12f, 0.12f, 0.12f, 1.00f)},
        {ImGuiCol_TextDisabled, ImVec4(0.55f, 0.55f, 0.55f, 1.00f)},
        {ImGuiCol_WindowBg, ImVec4(0.96f, 0.96f, 0.94f, 1.00f)},
        {ImGuiCol_ChildBg, ImVec4(0.00f, 0.00f, 0.00f, 0.03f)},
        {ImGuiCol_PopupBg, ImVec4(1.00f, 1.00f, 1.00f, 1.00f)},
        {ImGuiCol_Border, ImVec4(0.75f, 0.75f, 0.72f, 1.00f)},
        {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
        {ImGuiCol_Separator, ImVec4(0.80f, 0.80f, 0.78f, 1.00f)},
        {ImGuiCol_SeparatorHovered, ImVec4(0.17f, 0.34f, 0.59f, 0.78f)},
        {ImGuiCol_SeparatorActive, ImVec4(0.17f, 0.34f, 0.59f, 1.00f)},
        {ImGuiCol_FrameBg, ImVec4(1.00f, 1.00f, 1.00f, 1.00f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.90f, 0.92f, 0.95f, 1.00f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.85f, 0.88f, 0.92f, 1.00f)},
        {ImGuiCol_TitleBg, ImVec4(0.92f, 0.92f, 0.90f, 1.00f)},
        {ImGuiCol_TitleBgActive, ImVec4(0.88f, 0.88f, 0.86f, 1.00f)},
        {ImGuiCol_TitleBgCollapsed, ImVec4(0.92f, 0.92f, 0.90f, 0.75f)},
        {ImGuiCol_MenuBarBg, ImVec4(0.92f, 0.92f, 0.90f, 1.00f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.96f, 0.96f, 0.94f, 1.00f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.80f, 0.80f, 0.78f, 1.00f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.70f, 0.70f, 0.68f, 1.00f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.60f, 0.60f, 0.58f, 1.00f)},
        {ImGuiCol_CheckMark, ImVec4(0.17f, 0.34f, 0.59f, 1.00f)},
        {ImGuiCol_SliderGrab, ImVec4(0.17f, 0.34f, 0.59f, 0.70f)},
        {ImGuiCol_SliderGrabActive, ImVec4(0.17f, 0.34f, 0.59f, 1.00f)},
        {ImGuiCol_Button, ImVec4(0.17f, 0.34f, 0.59f, 0.08f)},
        {ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.34f, 0.59f, 0.20f)},
        {ImGuiCol_ButtonActive, ImVec4(0.17f, 0.34f, 0.59f, 0.35f)},
        {ImGuiCol_Header, ImVec4(0.17f, 0.34f, 0.59f, 0.12f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.17f, 0.34f, 0.59f, 0.25f)},
        {ImGuiCol_HeaderActive, ImVec4(0.17f, 0.34f, 0.59f, 0.40f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.90f, 0.90f, 0.88f, 1.00f)},
        {ImGuiCol_TableBorderStrong, ImVec4(0.75f, 0.75f, 0.72f, 1.00f)},
        {ImGuiCol_TableBorderLight, ImVec4(0.85f, 0.85f, 0.82f, 1.00f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(0.00f, 0.00f, 0.00f, 0.03f)},
        {ImGuiCol_Tab, ImVec4(0.92f, 0.92f, 0.90f, 1.00f)},
        {ImGuiCol_TabHovered, ImVec4(1.00f, 1.00f, 1.00f, 1.00f)},
        {ImGuiCol_TabSelected, ImVec4(1.00f, 1.00f, 1.00f, 1.00f)},
        {ImGuiCol_TabDimmed, ImVec4(0.92f, 0.92f, 0.90f, 1.00f)},
        {ImGuiCol_TabDimmedSelected, ImVec4(0.96f, 0.96f, 0.94f, 1.00f)},
        {ImGuiCol_PlotLines, ImVec4(0.17f, 0.34f, 0.59f, 1.00f)},
        {ImGuiCol_PlotHistogram, ImVec4(0.17f, 0.34f, 0.59f, 1.00f)},
        {ImGuiCol_TextSelectedBg, ImVec4(0.17f, 0.34f, 0.59f, 0.25f)},
        {ImGuiCol_DragDropTarget, ImVec4(0.17f, 0.34f, 0.59f, 0.90f)},
        {ImGuiCol_NavCursor, ImVec4(0.17f, 0.34f, 0.59f, 1.00f)},
        {ImGuiCol_DockingPreview, ImVec4(0.17f, 0.34f, 0.59f, 0.40f)},
        {ImGuiCol_DockingEmptyBg, ImVec4(0.96f, 0.96f, 0.94f, 1.00f)},
    };

    const Theme kThemes[] = {
        {"dark", ImGui::StyleColorsDark, {}, {4, 3, 3, 3, 9, 3, 3}},
        {"light", ImGui::StyleColorsLight, {}, {4, 3, 3, 3, 9, 3, 3}},
        {"classic", ImGui::StyleColorsClassic, {}, {4, 3, 3, 3, 9, 3, 3}},
        {"forestgreen", ImGui::StyleColorsDark, kForestGreen, {6, 4, 4, 4, 4, 4, 4}},
        {"amethyst", ImGui::StyleColorsDark, kAmethyst, {4, 3, 3, 3, 9, 3, 3}},
        {"sapphire", ImGui::StyleColorsDark, kSapphire, {5, 4, 4, 4, 12, 3, 4}},
        {"amberyellow", ImGui::StyleColorsDark, kAmberYellow, {2, 2, 2, 2, 2, 2, 2}},
        {"dracula", ImGui::StyleColorsDark, kDracula, {6, 4, 4, 4, 12, 4, 4}},
        {"catppuccinmocha", ImGui::StyleColorsDark, kCatppuccinMocha, {8, 5, 5, 5, 12, 5, 5}},
        {"gruvboxhard", ImGui::StyleColorsDark, kGruvboxHard, {2, 2, 2, 2, 2, 2, 2}},
        {"crimsonvesuvius", ImGui::StyleColorsDark, kCrimsonVesuvius, {3, 2, 2, 2, 12, 2, 3}},
        {"rosequartz", ImGui::StyleColorsDark, kRoseQuartz, {10, 6, 6, 6, 12, 6, 6}},
        {"cyberpunk", ImGui::StyleColorsDark, kCyberpunk, {0, 0, 0, 0, 0, 0, 0}},
        {"paperandink", ImGui::StyleColorsDark, kPaperAndInk, {2, 2, 2, 2, 12, 2, 2}},
    };

    constexpr float kMinControlContrast = 3.0f;

    ImVec4 Mix(const ImVec4& from, const ImVec4& to, const float amount)
    {
        return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
                      from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount);
    }

    ImVec4 Opaque(const ImVec4& color)
    {
        return ImVec4(color.x, color.y, color.z, 1.0f);
    }

    // The color as seen on an opaque background.
    ImVec4 Over(const ImVec4& color, const ImVec4& background)
    {
        return Opaque(Mix(background, color, color.w));
    }

    // Moves the color toward the text color until it stands out from the background.
    ImVec4 StandOut(const ImVec4& color, const ImVec4& background, const ImVec4& text)
    {
        constexpr int kSteps = 50;
        const ImVec4 start = Over(color, background);
        for (int step = 0; step < kSteps; ++step) {
            const ImVec4 mixed = Mix(start, text, static_cast<float>(step) / kSteps);
            if (ImGuiThemeContrast(mixed, background) >= kMinControlContrast) {
                return mixed;
            }
        }
        return text;
    }

    void MakeControlsVisible(ImGuiStyle& style)
    {
        ImVec4* colors = style.Colors;
        const ImVec4 window = Opaque(colors[ImGuiCol_WindowBg]);
        const ImVec4 text = Opaque(colors[ImGuiCol_Text]);

        style.FrameBorderSize = 1.0f;
        colors[ImGuiCol_Border] = StandOut(colors[ImGuiCol_Border], window, text);
        colors[ImGuiCol_Separator] = ImVec4(colors[ImGuiCol_Border].x, colors[ImGuiCol_Border].y,
                                            colors[ImGuiCol_Border].z, 0.5f);

        colors[ImGuiCol_CheckboxSelectedBg] = Mix(colors[ImGuiCol_FrameBg], colors[ImGuiCol_FrameBgHovered], 0.65f);
        colors[ImGuiCol_CheckMark] = StandOut(colors[ImGuiCol_CheckMark], Over(colors[ImGuiCol_CheckboxSelectedBg], window), text);
        colors[ImGuiCol_SliderGrab] = StandOut(colors[ImGuiCol_SliderGrab], Over(colors[ImGuiCol_FrameBg], window), text);
    }

    const Theme* FindTheme(const std::string_view name)
    {
        const auto theme = std::ranges::find(kThemes, name, &Theme::name);
        return theme != std::end(kThemes) ? &*theme : nullptr;
    }

    float Luminance(const ImVec4& color)
    {
        const auto channel = [](const float value) {
            return value <= 0.03928f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        };
        return 0.2126f * channel(color.x) + 0.7152f * channel(color.y) + 0.0722f * channel(color.z);
    }
}

const std::vector<std::string_view>& ImGuiThemeNames()
{
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> result;
        for (const Theme& theme : kThemes) {
            result.push_back(theme.name);
        }
        return result;
    }();
    return names;
}

bool IsImGuiTheme(const std::string_view name)
{
    return FindTheme(name) != nullptr;
}

void ApplyImGuiTheme(const std::string_view name, ImGuiStyle& style)
{
    const Theme* theme = FindTheme(name);
    if (!theme) {
        theme = &kThemes[0];
    }

    theme->base(&style);
    for (const ThemeColor& color : theme->colors) {
        style.Colors[color.id] = color.value;
    }

    style.WindowRounding = theme->rounding.window;
    style.ChildRounding = theme->rounding.child;
    style.FrameRounding = theme->rounding.frame;
    style.PopupRounding = theme->rounding.popup;
    style.ScrollbarRounding = theme->rounding.scrollbar;
    style.GrabRounding = theme->rounding.grab;
    style.TabRounding = theme->rounding.tab;

    MakeControlsVisible(style);
}

float ImGuiThemeContrast(const ImVec4& a, const ImVec4& b)
{
    const auto [darker, lighter] = std::minmax(Luminance(a), Luminance(b));
    return (lighter + 0.05f) / (darker + 0.05f);
}
