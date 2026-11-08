// Checks for the color themes: run ImGuiThemesTests.exe, it prints "ok" or aborts.
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "imgui.h"
#include "service/ImGuiThemes.h"

namespace
{
    void Check(const bool condition, const std::string_view theme, const char* what)
    {
        if (!condition) {
            std::fprintf(stderr, "FAILED %.*s: %s\n", static_cast<int>(theme.size()), theme.data(), what);
            std::abort();
        }
    }

    ImVec4 Over(const ImVec4& color, const ImVec4& background)
    {
        const float a = color.w;
        return ImVec4(color.x * a + background.x * (1 - a), color.y * a + background.y * (1 - a),
                      color.z * a + background.z * (1 - a), 1.0f);
    }
}

int main()
{
    constexpr float kMinContrast = 3.0f;

    Check(ImGuiThemeNames().size() == 14, "all", "theme count");
    Check(!IsImGuiTheme("nonsense"), "nonsense", "unknown name is rejected");

    for (const std::string_view name : ImGuiThemeNames()) {
        Check(IsImGuiTheme(name), name, "listed name is known");

        ImGuiStyle style;
        ApplyImGuiTheme(name, style);
        const ImVec4* colors = style.Colors;
        const ImVec4 opaqueWindow(colors[ImGuiCol_WindowBg].x, colors[ImGuiCol_WindowBg].y, colors[ImGuiCol_WindowBg].z, 1.0f);

        Check(style.FrameBorderSize == 1.0f, name, "frames have a border");
        Check(ImGuiThemeContrast(Over(colors[ImGuiCol_Border], opaqueWindow), opaqueWindow) >= kMinContrast, name, "border");
        Check(ImGuiThemeContrast(Over(colors[ImGuiCol_CheckMark], Over(colors[ImGuiCol_CheckboxSelectedBg], opaqueWindow)),
                                 Over(colors[ImGuiCol_CheckboxSelectedBg], opaqueWindow)) >= kMinContrast, name, "check mark");
        Check(ImGuiThemeContrast(Over(colors[ImGuiCol_SliderGrab], Over(colors[ImGuiCol_FrameBg], opaqueWindow)),
                                 Over(colors[ImGuiCol_FrameBg], opaqueWindow)) >= kMinContrast, name, "slider grab");
        Check(ImGuiThemeContrast(Over(colors[ImGuiCol_Text], opaqueWindow), opaqueWindow) >= 4.5f, name, "text");
    }

    ImGuiStyle unknown;
    ApplyImGuiTheme("nonsense", unknown);
    ImGuiStyle dark;
    ApplyImGuiTheme("dark", dark);
    Check(unknown.Colors[ImGuiCol_WindowBg].x == dark.Colors[ImGuiCol_WindowBg].x, "nonsense", "falls back to dark");

    ImGuiStyle sapphire;
    ApplyImGuiTheme("sapphire", sapphire);
    Check(sapphire.WindowRounding == 5.0f && sapphire.FrameRounding == 4.0f && sapphire.ScrollbarRounding == 12.0f,
          "sapphire", "rounding");
    ImGuiStyle cyberpunk;
    ApplyImGuiTheme("cyberpunk", cyberpunk);
    Check(cyberpunk.FrameRounding == 0.0f && cyberpunk.TabRounding == 0.0f, "cyberpunk", "square corners");

    std::puts("ok");
    return 0;
}
