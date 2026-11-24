#include "core/ScrollStitch.h"
#include <cstdlib>
#include <iostream>
#include <random>

using ScreenCapture::Core::appendScrollFrame;
using ScreenCapture::Core::findMostSimilarY;
using ScreenCapture::Core::findScrollByBottomStrip;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

// Characterization reference: the previous full-search matching formula. Keep
// this deliberately unoptimized to detect changed offsets, tie-breaking or
// confidence thresholds when optimizing the production matcher.
int referenceTop(const std::vector<std::uint8_t>& old, int height,
                 const std::vector<std::uint8_t>& current, int strip, int width)
{
    double best = std::numeric_limits<double>::max();
    int result = 0;
    for (int y = 0; y <= height - strip; ++y) {
        double error = 0;
        for (int row = 0; row < strip; ++row) {
            for (int x = 0; x < width; ++x) {
                const int difference = old[static_cast<std::size_t>(y + row) * width + x] -
                                       current[static_cast<std::size_t>(row) * width + x];
                error += difference * difference;
            }
        }
        error /= strip;
        if (error < best) { best = error; result = y; }
    }
    return result;
}

int referenceBottom(const std::vector<std::uint8_t>& old,
                    const std::vector<std::uint8_t>& current, int width, int height)
{
    double best = std::numeric_limits<double>::max();
    double zero = best;
    int result = 0;
    for (int offset = 0; offset < height; ++offset) {
        double error = 0;
        const int rows = height - offset;
        for (int row = 0; row < rows; ++row) {
            for (int x = 0; x < width; ++x) {
                const int difference = old[static_cast<std::size_t>(offset + row) * width + x] -
                                       current[static_cast<std::size_t>(row) * width + x];
                error += difference * difference;
            }
        }
        error /= rows;
        if (offset == 0) zero = error;
        if (error < best) { best = error; result = offset; }
    }
    return result <= 0 || best >= zero * 0.9 || best > 40000 ? 0 : result;
}
}

int main()
{
    std::mt19937 random(24112026);
    for (int sample = 0; sample < 600; ++sample) {
        const int width = 1 + static_cast<int>(random() % 50);
        const int height = 2 + static_cast<int>(random() % 90);
        const int strip = 1 + static_cast<int>(random() % static_cast<unsigned>(height));
        std::vector<std::uint8_t> previous(static_cast<std::size_t>(width) * height);
        std::vector<std::uint8_t> current(previous.size());
        for (auto& value : previous) value = static_cast<std::uint8_t>(random() % (sample % 3 == 0 ? 2 : 256));
        for (auto& value : current) value = static_cast<std::uint8_t>(random() % 256);
        if (sample % 2 == 0) {
            const int offset = static_cast<int>(random() % static_cast<unsigned>(height - strip + 1));
            std::copy_n(previous.begin() + static_cast<std::size_t>(offset) * width,
                        static_cast<std::size_t>(strip) * width, current.begin());
        }
        require(findMostSimilarY(previous, height, current, strip, width) ==
                referenceTop(previous, height, current, strip, width), "top-strip matcher changed its result");
        require(findScrollByBottomStrip(previous, current, width, height) ==
                referenceBottom(previous, current, width, height), "bottom-strip matcher changed its result");
    }
    std::vector<std::uint8_t> blank(8 * 20, 255);
    require(findMostSimilarY(blank, 20, blank, 7, 8) == 0, "ties must retain the first offset");
    require(findScrollByBottomStrip(blank, blank, 8, 20) == 0, "blank strips must not imply movement");
    require(findMostSimilarY({}, 10, {}, 5, 2) == 0, "invalid spans must be rejected");

    const std::vector<std::uint8_t> colors{ 255,0,0,0, 0,255,0,255, 0,0,255,128 };
    std::vector<std::uint8_t> gray;
    require(ScreenCapture::Core::toGrayscale(colors, 3, 1, gray), "grayscale conversion failed");
    require(gray == std::vector<std::uint8_t>({29,149,76}), "grayscale weights changed");

    constexpr int width = 7, frameHeight = 23;
    int resultHeight = frameHeight;
    std::vector<std::uint8_t> result(width * frameHeight * 4, 17), expected = result;
    for (int step = 0; step < 100; ++step) {
        const int change = step % 8;
        const int offset = 1 + step % (frameHeight - change);
        std::vector<std::uint8_t> frame(width * frameHeight * 4);
        for (auto& value : frame) value = static_cast<std::uint8_t>(random() % 256);
        const auto stride = static_cast<std::size_t>(width) * 4;
        const auto destination = static_cast<std::size_t>(resultHeight - frameHeight + offset + change) * stride;
        expected.resize(static_cast<std::size_t>(resultHeight + offset) * stride);
        std::copy(frame.begin() + static_cast<std::size_t>(change) * stride, frame.end(), expected.begin() + destination);
        require(appendScrollFrame(result, width, resultHeight, frame, frameHeight, change, offset), "append failed");
        require(result == expected, "stitched pixels differ from previous copy-based algorithm");
    }
    const auto saved = result;
    const auto savedHeight = resultHeight;
    require(!appendScrollFrame(result, width, resultHeight, {}, frameHeight, 0, 1), "short frame was accepted");
    require(result == saved && resultHeight == savedHeight, "invalid input modified the result");
    std::cout << "scroll stitching: 1200 randomized matcher comparisons, 100 append comparisons and edge cases passed\n";
}
