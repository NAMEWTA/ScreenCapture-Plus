#include "core/ScrollStitch.h"
#include "core/ImagePreview.h"
#include <cstdlib>
#include <iostream>
#include <random>

using HuhuCapture::Core::appendScrollFrame;
using HuhuCapture::Core::findMostSimilarY;
using HuhuCapture::Core::findScrollByBottomStrip;

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
    require(HuhuCapture::Core::toGrayscale(colors, 3, 1, gray), "grayscale conversion failed");
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
    // Compare the real clipped sampler with the tail of the original complete
    // nearest-neighbour thumbnail, including non-integer zoom and BGRA alpha.
    for (int sample = 0; sample < 60; ++sample) {
        const int sourceWidth = 1 + static_cast<int>(random() % 73);
        const int sourceHeight = 1 + static_cast<int>(random() % 180);
        const float targetWidth = 32.5f + static_cast<float>(random() % 160);
        const int visibleHeight = 1 + static_cast<int>(random() % 200);
        std::vector<std::uint8_t> source(static_cast<std::size_t>(sourceWidth) * sourceHeight * 4);
        for (auto& value : source) value = static_cast<std::uint8_t>(random() % 256);
        const float scale = targetWidth / sourceWidth;
        const int fullWidth = static_cast<int>(sourceWidth * scale);
        const int fullHeight = static_cast<int>(sourceHeight * scale);
        std::vector<std::uint8_t> complete(static_cast<std::size_t>(fullWidth) * fullHeight * 4);
        for (int y = 0; y < fullHeight; ++y) {
            const int sourceY = std::min(static_cast<int>(static_cast<float>(y) / scale), sourceHeight - 1);
            for (int x = 0; x < fullWidth; ++x) {
                const int sourceX = std::min(static_cast<int>(static_cast<float>(x) / scale), sourceWidth - 1);
                std::copy_n(source.data() + (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4,
                            4, complete.data() + (static_cast<std::size_t>(y) * fullWidth + x) * 4);
            }
        }
        std::vector<std::uint8_t> clipped;
        const auto size = HuhuCapture::Core::makeImagePreview(source, sourceWidth, sourceHeight,
            targetWidth, visibleHeight, 16384, clipped);
        if (fullHeight == 0) {
            require(!size, "zero-height preview must not allocate");
            continue;
        }
        require(size && size->width == fullWidth && size->height == std::min(fullHeight, visibleHeight),
                "clipped preview dimensions changed");
        require(clipped == std::vector<std::uint8_t>(complete.end() - clipped.size(), complete.end()),
                "visible thumbnail pixels differ from the original full preview");
    }
    // The complete thumbnail would be 128 x 4,608,000 (>2 GiB). Only its visible
    // 1,080 rows should be rasterized, retaining the original bottom alignment.
    std::vector<std::uint8_t> narrow(36000 * 4);
    for (int row = 0; row < 36000; ++row) {
        narrow[row * 4] = static_cast<std::uint8_t>(row % 251);
        narrow[row * 4 + 1] = static_cast<std::uint8_t>((row / 251) % 251);
        narrow[row * 4 + 3] = 255;
    }
    std::vector<std::uint8_t> preview;
    const auto narrowSize = HuhuCapture::Core::makeImagePreview(narrow, 1, 36000, 128.0f, 1080, 16384, preview);
    require(narrowSize && narrowSize->width == 128 && narrowSize->height == 1080,
            "narrow selection did not limit preview to visible rows");
    require(preview.size() == 128u * 1080u * 4u, "narrow selection allocated a complete thumbnail");
    for (int row = 0; row < 1080; ++row) {
        const int sourceRow = (4608000 - 1080 + row) / 128;
        require(std::equal(preview.begin() + static_cast<std::size_t>(row) * 128 * 4,
                           preview.begin() + static_cast<std::size_t>(row) * 128 * 4 + 4,
                           narrow.begin() + sourceRow * 4), "narrow preview sampled a different row");
    }
    const auto previousPreview = preview;
    require(!HuhuCapture::Core::makeImagePreview({}, 1, 36000, 128.0f, 1080, 16384, preview),
            "short preview source was accepted");
    require(!HuhuCapture::Core::makeImagePreview(narrow, 1, 36000, 128.0f, 0, 16384, preview),
            "invisible preview was allocated");
    require(!HuhuCapture::Core::makeImagePreview(narrow, 1, 36000,
                (std::numeric_limits<float>::max)(), 1080, 16384, preview),
            "overflowing preview dimensions were accepted");
    require(preview == previousPreview, "invalid preview input modified the previous buffer");
    std::cout << "scroll stitching: 1200 matcher, 100 append, 60 preview comparisons and edge cases passed\n";
}
