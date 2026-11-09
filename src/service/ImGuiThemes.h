#pragma once

#include <string_view>
#include <vector>

struct ImGuiStyle;
struct ImVec4;

// Lower case names, as used for the Theme setting.
[[nodiscard]] const std::vector<std::string_view>& ImGuiThemeNames();
[[nodiscard]] bool IsImGuiTheme(std::string_view name);

// Sets the colors and rounding of the theme and makes its controls easy to see. An unknown name gives dark.
void ApplyImGuiTheme(std::string_view name, ImGuiStyle& style);

// WCAG contrast ratio of two opaque colors, from 1 to 21.
[[nodiscard]] float ImGuiThemeContrast(const ImVec4& a, const ImVec4& b);
