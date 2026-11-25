#include "recording/RecordingBackend.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace recording = HuhuCapture::Recording;
namespace {
void require(bool condition, const char* detail)
{ if (!condition) throw std::runtime_error(detail); }
}

int main()
{
    try {
        const auto positive = recording::alignedMp4Region({3, 5, 103, 99});
        require(positive && *positive == recording::PixelRegion{4, 6, 100, 98},
            "Odd origins align inward without growing the selected area");
        const auto negative = recording::alignedMp4Region({-1919, -1079, 3840, 2160});
        require(negative && *negative == recording::PixelRegion{-1916, -1078, 3836, 2158},
            "Negative monitor origins use ceiling alignment, not truncation toward zero");
        require(!recording::alignedMp4Region({3, 1, 4, 2}), "A sub-alignment selection never expands outside itself");
        require(!recording::alignedMp4Region({0, 0, 0, 20}), "Empty region is refused");
        require(!recording::alignedMp4Region({0, 0, 16388, 8}), "Oversized frame width is refused");
        require(!recording::alignedMp4Region({0, 0, 1920, 1080}, 1920ull * 1080 * 4 - 1),
            "Frame byte budget is enforced before native allocation");
        require(!recording::alignedMp4Region({(std::numeric_limits<std::int32_t>::max)() - 4, 0, 16, 4}),
            "Right edge overflow is refused before converting to Win32 coordinates");
        const auto lowest = (std::numeric_limits<std::int32_t>::min)();
        const auto low = recording::alignedMp4Region({lowest, lowest, 16, 8});
        require(low && low->x == lowest && low->y == lowest, "Lowest negative coordinates do not overflow alignment");
        for (std::int32_t x = -31; x != 32; ++x) for (std::int32_t y = -31; y != 32; ++y) {
            const recording::PixelRegion original{x, y, 135, 93};
            const auto aligned = recording::alignedMp4Region(original);
            require(aligned && aligned->x % 4 == 0 && aligned->y % 2 == 0 &&
                aligned->width % 4 == 0 && aligned->height % 2 == 0 &&
                aligned->x >= x && aligned->y >= y &&
                aligned->x + aligned->width <= x + original.width &&
                aligned->y + aligned->height <= y + original.height,
                "Alignment preserves all selected signed-coordinate boundaries");
        }
        std::cout << "PASS MP4 physical-coordinate alignment, overflow and allocation budgets\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
