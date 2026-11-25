#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace HuhuCapture::Editor {

// The existing native tool preferences and the Qt editor share these exact
// ranges, defaults and RGBA colors. Values are logical pixels; each shape
// captures its source-image pixel size when it is created.
struct ToolSlider {
    const wchar_t* id;
    const wchar_t* key;
    float min, max, def;
};

inline constexpr std::array toolSliders{
    ToolSlider{L"rect", L"width", 1.f, 26.f, 2.f},
    ToolSlider{L"ellipse", L"width", 1.f, 26.f, 2.f},
    ToolSlider{L"arrow", L"width", 1.f, 16.f, 3.f},
    ToolSlider{L"number", L"radius", 6.f, 86.f, 16.f},
    ToolSlider{L"line", L"width", 1.f, 60.f, 12.f},
    ToolSlider{L"text", L"fontSize", 10.f, 60.f, 20.f},
    ToolSlider{L"mosaic", L"width", 18.f, 68.f, 28.f},
    ToolSlider{L"eraser", L"width", 18.f, 68.f, 28.f}
};

inline constexpr const ToolSlider* findToolSlider(std::wstring_view id) noexcept
{
    for (const auto& value : toolSliders)
        if (id == value.id) return &value;
    return nullptr;
}

inline constexpr std::array<std::uint32_t, 9> toolColors{
    0xCF1322FF, 0xD48806FF, 0x389E0DFF, 0x13C2C2FF, 0x0958D9FF,
    0x722ED1FF, 0xEB2F96FF, 0x000000FF, 0xFFFFFFFF
};

} // namespace HuhuCapture::Editor
