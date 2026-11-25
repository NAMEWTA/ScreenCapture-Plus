#include "core/MosaicPixels.h"
#include "editor/ToolSettings.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
void check(bool value, int line)
{
    if (!value) {
        std::cerr << "FAIL mosaic contract at line " << line << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}
#define CHECK(...) check((__VA_ARGS__), __LINE__)

int main()
{
    using HuhuCapture::Core::mosaicPixels;
    // Two complete blocks and one short edge, padded scanlines, independent
    // alpha and channel means. Guard bytes must never become part of a block.
    constexpr int width = 5, height = 3, stride = 24;
    std::vector<std::uint8_t> image(stride * height, 0xE7);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 4; ++c)
                image[y * stride + x * 4 + c] = static_cast<std::uint8_t>(x + y * 10 + c * 40);
    const auto original = image;
    CHECK(mosaicPixels(image, stride, width, height, 2));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int blockX = x / 2 * 2, blockY = y / 2 * 2;
            const int xEnd = (std::min)(blockX + 2, width), yEnd = (std::min)(blockY + 2, height);
            for (int c = 0; c < 4; ++c) {
                unsigned sum = 0;
                for (int yy = blockY; yy < yEnd; ++yy)
                    for (int xx = blockX; xx < xEnd; ++xx) sum += original[yy * stride + xx * 4 + c];
                CHECK(image[y * stride + x * 4 + c] == sum / ((xEnd - blockX) * (yEnd - blockY)));
            }
        }
        for (int x = width * 4; x < stride; ++x) CHECK(image[y * stride + x] == 0xE7);
    }
    const auto valid = image;
    CHECK(!mosaicPixels(image, 19, width, height, 2) && image == valid);
    CHECK(!mosaicPixels(std::span(image).first(10), stride, width, height, 2) && image == valid);
    CHECK(!mosaicPixels(image, stride, width, height, 0) && image == valid);
    CHECK(!mosaicPixels(image, (std::numeric_limits<std::size_t>::max)(), width, height, 2) && image == valid);
    CHECK(mosaicPixels(image, stride, width, height, (std::numeric_limits<int>::max)()));
    CHECK(HuhuCapture::Editor::findToolSlider(L"number")->max == 86.f);
    CHECK(HuhuCapture::Editor::findToolSlider(L"annotation") == nullptr);
    std::cout << "PASS shared block averages, padding, partial blocks and checked layout\n";
}
