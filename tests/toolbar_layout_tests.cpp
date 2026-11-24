#include "ui/toolbars/ToolbarMetrics.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace ScreenCapture::UI;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "toolbar_layout_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

void editorWrapsWithoutShrinkingTargets()
{
    const std::vector<float> widths{
        44, 44, 44, 44, 44, 44, 44, 44, 44, 1, 44, 44, 1, 44, 44, 44
    };
    const auto wide = measureToolbar(widths);
    CHECK(wide.rows == 1);
    CHECK(wide.width == 620.f);
    CHECK(wide.height == 46.f);
    const auto narrow = measureToolbar(widths, 320.f);
    CHECK(narrow.rows == 2);
    CHECK(narrow.width <= 320.f);
    CHECK(narrow.height == 90.f);
    const auto smaller = measureToolbar(widths, 240.f);
    CHECK(smaller.rows == 3);
    CHECK(smaller.width <= 240.f);
    CHECK(smaller.height == 134.f);
}

void mixedControlsAndPaletteKeepRowHeight()
{
    const std::array<float, 8> video{ 60, 60, 1, 44, 44, 1, 44, 44 };
    CHECK(measureToolbar(video).width == 300.f);
    CHECK(measureToolbar(video, 200.f).rows == 2);
    const std::array<float, 12> textPalette{ 44, 44, 106, 44, 44, 44, 44, 44, 44, 44, 44, 44 };
    const auto palette = measureToolbar(textPalette, 320.f, 3.f);
    CHECK(palette.rows == 2);
    CHECK(palette.width <= 320.f);
    CHECK(palette.height == 93.f);
    const std::array<float, 2> eraser{ 44, 106 };
    CHECK(measureToolbar(eraser, 320.f, 3.f).width == 152.f);
    CHECK(measureToolbar(eraser, 320.f, 3.f).height == 49.f);
}

void exactBoundaryAndVeryNarrowAreas()
{
    const std::array<float, 3> buttons{ 44, 44, 44 };
    CHECK(measureToolbar(buttons, 134.f).rows == 1);
    CHECK(measureToolbar(buttons, 133.f).rows == 2);
    const auto minimal = measureToolbar(buttons, 1.f);
    CHECK(minimal.rows == 3);
    CHECK(minimal.width == 46.f);
    CHECK(measureToolbar(std::span<const float>{}).rows == 0);
}

void fractionalDpiDoesNotClipOrChangeWrapping()
{
    const std::array<float, 5> widths{ 136, 1, 44, 44, 44 };
    for (const auto dpi : { 1.f, 1.25f, 1.5f, 1.75f, 2.f, 2.5f, 3.f }) {
        const auto extent = measureToolbar(widths, 640.f / dpi);
        const auto windowWidth = toolbarPixelAligned(extent.width, dpi) * dpi;
        const auto windowHeight = toolbarPixelAligned(extent.height, dpi) * dpi;
        CHECK(windowWidth >= extent.width * dpi);
        CHECK(windowHeight >= extent.height * dpi);
        CHECK(windowWidth <= 640.f);
        CHECK(std::abs(windowWidth - std::round(windowWidth)) < .001f);
        // Re-measuring at the actual native width must retain the row count.
        CHECK(measureToolbar(widths, windowWidth / dpi).rows == extent.rows);
    }
}

} // namespace

int main()
{
    editorWrapsWithoutShrinkingTargets();
    mixedControlsAndPaletteKeepRowHeight();
    exactBoundaryAndVeryNarrowAreas();
    fractionalDpiDoesNotClipOrChangeWrapping();
    std::cout << "toolbar_layout_tests: 4 scenarios passed\n";
}
