#include "core/PixelBuffer.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main()
{
    using ScreenCapture::Core::pixelLayout;
    using ScreenCapture::Core::bgraToRgb;
    using ScreenCapture::Core::makeOpaque;
    const auto frame = pixelLayout(1920, 1080, 4);
    require(frame && frame->stride == 7680 && frame->bytes == 8294400,
            "1080p BGRA layout");
    require(!pixelLayout(0, 10, 4) && !pixelLayout(-1, 10, 4) &&
            !pixelLayout(10, 0, 4) && !pixelLayout(10, -1, 4) &&
            !pixelLayout(10, 10, 0), "invalid dimensions rejected");
    require(!pixelLayout(2, 1, (std::numeric_limits<std::size_t>::max)()),
            "stride overflow rejected");
    require(!pixelLayout(1, 3, (std::numeric_limits<std::size_t>::max)() / 2),
            "total allocation overflow rejected");

    // Odd width and multiple rows catch accidental RGB row padding and inversion.
    const std::array<std::uint8_t, 24> bgra{
        0, 0, 255, 0, 0, 255, 0, 17, 255, 0, 0, 255,
        3, 2, 1, 0, 30, 20, 10, 128, 0, 0, 0, 0
    };
    const std::array<std::uint8_t, 18> expected{
        255, 0, 0, 0, 255, 0, 0, 0, 255,
        1, 2, 3, 10, 20, 30, 0, 0, 0
    };
    std::array<std::uint8_t, 20> rgb{};
    rgb.fill(0xCD);
    require(bgraToRgb(bgra, rgb, 3, 2), "conversion succeeds");
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(rgb[i] == expected[i], "RGB order and top-down rows preserved");
    require(rgb[18] == 0xCD && rgb[19] == 0xCD, "destination guard untouched");
    rgb.fill(0xCD);
    require(!bgraToRgb(std::span(bgra).first(23), rgb, 3, 2), "short source rejected");
    require(!bgraToRgb(bgra, std::span(rgb).first(17), 3, 2), "short target rejected");
    for (const auto b : rgb) require(b == 0xCD, "invalid conversion writes nothing");
    auto opaque = bgra;
    require(makeOpaque(opaque), "screen alpha normalization succeeds");
    for (std::size_t i = 0; i < opaque.size(); ++i)
        require(opaque[i] == (i % 4 == 3 ? 255 : bgra[i]), "alpha changes without changing BGR");
    auto partial = bgra;
    require(!makeOpaque(std::span(partial).first(23)), "incomplete pixel rejected");
    require(partial == bgra, "invalid alpha normalization writes nothing");
    std::cout << "Pixel buffer regression checks passed\n";
}
