#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

class SC4DrawContext;
class cIGZGDriver;

// Fields of SC4 1.1.641's SC4DrawContext and the draw-context functions the decal renderer
// calls (docs/world-draw-design.md).
namespace TerrainDecal::DrawContextAccess
{
    constexpr std::ptrdiff_t kDriverOffset = 0x30;
    constexpr std::ptrdiff_t kRendererOffset = 0x38;
    constexpr std::ptrdiff_t kRenderStateOffset = 0x80;

    // cIGZGDriver texture wrap values, as SC4DrawContext::SetTexWrapModes takes them.
    constexpr int kWrapClamp = 2;
    constexpr int kWrapRepeat = 3;

    using SetTexWrapModesFn = void(__thiscall*)(SC4DrawContext*, int wrapU, int wrapV, int stage);

    template <typename T>
    [[nodiscard]] T ReadField(const void* const base, const std::ptrdiff_t offset) noexcept
    {
        T value{};
        std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(T));
        return value;
    }

    [[nodiscard]] inline cIGZGDriver* Driver(SC4DrawContext* const drawContext) noexcept
    {
        return drawContext ? ReadField<cIGZGDriver*>(drawContext, kDriverOffset) : nullptr;
    }
}
