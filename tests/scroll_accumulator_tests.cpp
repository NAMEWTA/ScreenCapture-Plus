#include "capture/ScrollAccumulator.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using HuhuCapture::Capture::ScrollAccumulator;
using HuhuCapture::Capture::ScrollAppend;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::uint8_t> page(int width, int height)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
        bytes[offset] = static_cast<std::uint8_t>((y * 17 + x * 23) % 251);
        bytes[offset + 1] = static_cast<std::uint8_t>((y * 13 + x * 3) % 253);
        bytes[offset + 2] = static_cast<std::uint8_t>((y * 7 + x * 31) % 249);
        bytes[offset + 3] = 255;
    }
    return bytes;
}
}
int main()
{
    try {
        constexpr int width = 28, height = 140;
        const auto source = page(width, 500);
        const auto frame = [&](int top) { return std::span(source).subspan(static_cast<std::size_t>(top) * width * 4,
            static_cast<std::size_t>(height) * width * 4); };
        ScrollAccumulator result;
        require(result.start(frame(0), width, height, {}), "Start with a complete original frame");
        require(result.append(frame(20)) == ScrollAppend::Appended && result.height() == 160, "Append actual matching 20-row tail");
        require(result.append(frame(40)) == ScrollAppend::Appended && result.height() == 180, "Continue from the previous source frame");
        require(std::equal(result.pixels().begin(), result.pixels().end(), source.begin()), "The complete stitched image is the exact source page");
        const auto saved = std::vector(result.pixels().begin(), result.pixels().end());
        for (int i = 0; i < 8; ++i) require(result.append(frame(40)) == ScrollAppend::RetryScroll, "Preserve eight automatic no-motion retries");
        require(result.append(frame(40)) == ScrollAppend::ReachedBottom, "Stop after the existing no-motion limit");
        require(std::equal(result.pixels().begin(), result.pixels().end(), saved.begin()), "Bottom detection retains the last complete image");
        require(result.append(frame(40).first(8)) == ScrollAppend::Failed, "Reject a changed/incomplete frame");
        require(std::equal(result.pixels().begin(), result.pixels().end(), saved.begin()), "Malformed input cannot damage prior pixels");
        ScrollAccumulator manual;
        require(manual.start(frame(0), width, height, {}), "Start manual stream");
        for (int i = 0; i < 30; ++i) require(manual.append(frame(0), false) == ScrollAppend::RetryScroll, "Manual waiting cannot infer reached bottom");
        require(manual.append(frame(20), false) == ScrollAppend::Appended, "Manual scrolling uses the same exact matching rules");
        ScrollAccumulator bounded;
        // Two BGRA frames and two grayscale frames precede any appended rows.
        constexpr std::size_t baseWorkingBytes = width * height * (4u + 4u + 1u + 1u);
        constexpr std::size_t workingLimit = baseWorkingBytes + width * 4u * 10u;
        ScrollAccumulator tooSmall;
        require(!tooSmall.start(frame(0), width, height, {baseWorkingBytes - 1, 36000}),
            "Reject a budget one byte below the complete initial working set");
        require(bounded.start(frame(0), width, height, {workingLimit, 36000}) &&
            bounded.allocatedBytes() == baseWorkingBytes,
            "Start with all accumulation and matching allocations charged to the budget");
        require(bounded.append(frame(20)) == ScrollAppend::Limit && bounded.height() == height, "Stop before an over-budget append");
        require(std::equal(bounded.pixels().begin(), bounded.pixels().end(), source.begin()), "Budget limit preserves original complete frame");
        ScrollAccumulator heightBound;
        require(heightBound.start(frame(0), width, height, {512u * 1024u * 1024u, 150}), "Start height-limited sequence");
        require(heightBound.append(frame(20)) == ScrollAppend::Limit && heightBound.height() == 160,
            "Retain the legacy complete append crossing the height stop threshold");
        const auto* ownedPixels = heightBound.pixels().data();
        const auto ownedHeight = heightBound.height();
        auto taken = heightBound.takePixels();
        require(taken.data() == ownedPixels && heightBound.pixels().empty() &&
            taken.size() == static_cast<std::size_t>(width) * ownedHeight * 4,
            "Transfer the complete accumulated allocation without copying its pixels");
        std::cout << "scroll accumulator contracts passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
