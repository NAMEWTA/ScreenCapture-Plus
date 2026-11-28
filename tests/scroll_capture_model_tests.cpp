#include "capture/ScrollCaptureModel.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture::Capture;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

// A generated BGRA page: fixed header followed by deterministic row-addressed
// content. Viewports at different scroll positions are samples of that one
// page; expected output is the actual contiguous page, not a second stitcher.
std::vector<std::uint8_t> page(int width, int height, int header)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
        const auto row = static_cast<unsigned>(y - header);
        pixels[offset] = y < header ? 240 : static_cast<std::uint8_t>((row * 71 + x * 19) % 251);
        pixels[offset + 1] = y < header ? 242 : static_cast<std::uint8_t>((row * 43 + x * 53) % 251);
        pixels[offset + 2] = y < header ? 246 : static_cast<std::uint8_t>((row * 37 + x * 31) % 251);
        pixels[offset + 3] = 255;
    }
    return pixels;
}
std::vector<std::uint8_t> viewport(const std::vector<std::uint8_t>& content,
    int width, int height, int header, int position)
{
    const auto stride = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> frame(stride * height);
    std::copy_n(content.begin(), stride * header, frame.begin());
    std::copy_n(content.begin() + stride * (header + position), stride * (height - header),
        frame.begin() + stride * header);
    return frame;
}
void actualPagePixels()
{
    constexpr int width = 37, height = 180, header = 7;
    const auto content = page(width, 1000, header);
    ScrollCaptureModel model;
    require(model.begin(viewport(content, width, height, header, 0), width, height) == ScrollStartError::None,
        "the first complete page viewport initializes the owned result");
    const std::uint8_t* reserved = nullptr;
    for (int i = 1; i <= 6; ++i) {
        const auto update = model.accept(viewport(content, width, height, header, i * 12));
        require(update.appended && update.next == ScrollNextStep::Scroll && model.height() == height + i * 12,
            "accepted page scrolling appends the exact physical row count");
        require(model.pixels() == std::vector<std::uint8_t>(content.begin(),
            content.begin() + static_cast<std::size_t>(width) * model.height() * 4),
            "the fixed header and all original page pixels survive consecutive scrolls");
        if (reserved) require(model.pixels().data() == reserved,
            "appends within allocated capacity do not recopy the accumulated page");
        reserved = model.pixels().data();
    }
    const auto saved = model.pixels();
    model.stop();
    require(model.stopReason() == ScrollStopReason::Requested && model.pixels() == saved &&
        model.allocatedBytes() == model.pixels().capacity(),
        "stop releases matching frames while retaining the complete long image");
    require(model.begin({}, width, height) == ScrollStartError::InvalidFrame && model.pixels() == saved,
        "a rejected new capture cannot replace the last complete output");
    std::cout << "PASS contiguous page pixels, fixed header, incremental capacity and stopped-output ownership\n";
}
void stationaryAndCaptureFailure()
{
    const auto first = page(11, 160, 0);
    ScrollCaptureModel model;
    require(model.begin(first, 11, 160) == ScrollStartError::None, "begin stationary page");
    for (int i = 0; i < 8; ++i)
        require(model.accept(first).next == ScrollNextStep::Scroll && model.running(),
            "the first eight stationary samples retain the original retry behavior");
    const auto end = model.accept(first);
    require(end.next == ScrollNextStep::Stopped && end.reason == ScrollStopReason::ReachedBottom && model.pixels() == first,
        "the ninth stationary sample stops and preserves the original image");
    require(model.begin(first, 11, 160) == ScrollStartError::None, "restart with a complete source");
    for (int i = 0; i < 8; ++i)
        require(model.captureFailed().next == ScrollNextStep::Scroll, "capture failure keeps its eight real retries");
    require(model.captureFailed().reason == ScrollStopReason::CaptureFailed && model.pixels() == first,
        "capture failure is distinguished from page bottom without losing output");
    std::cout << "PASS ninth-sample stopping and real capture-failure distinction\n";
}
void changedButUnmatched()
{
    std::vector<std::uint8_t> first(8 * 140 * 4, 50), changed(first.size(), 60);
    for (std::size_t i = 3; i < first.size(); i += 4) first[i] = changed[i] = 255;
    ScrollCaptureModel model;
    require(model.begin(first, 8, 140) == ScrollStartError::None, "begin a changing uniform page");
    require(model.accept(changed).next == ScrollNextStep::Settle &&
        model.accept(changed).next == ScrollNextStep::Settle &&
        model.accept(changed).next == ScrollNextStep::Scroll,
        "a changing frame with no matched movement gets exactly two settle rechecks before another wheel");
    require(model.pixels() == first && model.height() == 140,
        "an unmatched animation does not add fabricated page rows");
    std::cout << "PASS animation settle rechecks without fabricated scroll offsets\n";
}
void heightAndMemoryLimits()
{
    constexpr int width = 7, height = 180;
    const auto content = page(width, 400, 0);
    const auto first = viewport(content, width, height, 0, 0);
    ScrollCaptureModel heightLimited({200, 1024 * 1024});
    require(heightLimited.begin(first, width, height) == ScrollStartError::None, "begin height-bounded capture");
    require(heightLimited.accept(viewport(content, width, height, 0, 12)).appended && heightLimited.running(),
        "a complete frame below the original stopping threshold is retained");
    const auto end = heightLimited.accept(viewport(content, width, height, 0, 24));
    require(end.appended && end.next == ScrollNextStep::Stopped && end.reason == ScrollStopReason::HeightLimit &&
        heightLimited.height() == 204 && heightLimited.pixels() ==
            std::vector<std::uint8_t>(content.begin(), content.begin() + width * 204 * 4),
        "the original greater-than height rule retains the complete last append rather than cutting pixels");

    constexpr std::size_t workingBytes = width * height * 10 + width * 12 * 4;
    ScrollCaptureModel bounded({36000, workingBytes});
    require(bounded.begin(first, width, height) == ScrollStartError::None, "begin memory-bounded capture");
    require(bounded.accept(viewport(content, width, height, 0, 12)).appended && bounded.allocatedBytes() <= workingBytes,
        "vector capacity growth respects the entire accumulation and matcher budget");
    const auto saved = bounded.pixels();
    const auto limited = bounded.accept(viewport(content, width, height, 0, 24));
    require(limited.next == ScrollNextStep::Stopped && limited.reason == ScrollStopReason::MemoryLimit &&
        !limited.appended && bounded.height() == 192 && bounded.pixels() == saved,
        "memory refusal retains the last complete result and its exact dimensions");
    std::cout << "PASS complete-frame height threshold and bounded memory with retained last result\n";
}
void progressResetsRetries()
{
    const auto content = page(11, 400, 0);
    const auto first = viewport(content, 11, 180, 0, 0);
    ScrollCaptureModel model;
    require(model.begin(first, 11, 180) == ScrollStartError::None, "begin progress recovery");
    for (int i = 0; i < 8; ++i) require(model.accept(first).next == ScrollNextStep::Scroll, "wait for actual scrolling");
    const auto moved = viewport(content, 11, 180, 0, 13);
    require(model.accept(moved).appended, "actual progress remains acceptable after eight stationary samples");
    for (int i = 0; i < 8; ++i) require(model.accept(moved).next == ScrollNextStep::Scroll, "progress resets the entire retry allowance");
    require(model.accept(moved).reason == ScrollStopReason::ReachedBottom, "the restored ninth-sample threshold remains exact");
    std::cout << "PASS real progress resets stationary retry state\n";
}
void manualReadingAndRetainedOutput()
{
    const auto content = page(31, 400, 5);
    const auto first = viewport(content, 31, 180, 5, 0);
    ScrollCaptureModel model;
    require(model.begin(first, 31, 180) == ScrollStartError::None, "begin a manually scrolled real page");
    for (int i = 0; i < 30; ++i)
        require(model.accept(first, false).next == ScrollNextStep::Scroll && model.running(),
            "a user reading a manual source cannot establish automatic reached-bottom");
    require(model.accept(viewport(content, 31, 180, 5, 17), false).appended && model.height() == 197,
        "manual scrolling retains the same exact matched page rows");
    const auto* backing = model.pixels().data();
    const auto bytes = model.pixels().size();
    model.stop();
    require(model.pixels().data() == backing && model.pixels().size() == bytes && model.hasImage() && !model.running() &&
        std::equal(model.pixels().begin(), model.pixels().end(), content.begin()),
        "the completed manual image retains the same backing allocation and fixed header for output");
    std::cout << "PASS manual reading intervals and exact retained output allocation\n";
}
}
int main()
{
    static_assert(ScrollCaptureRules::comparisonRows == 100 && ScrollCaptureRules::firstScrollDelayMs == 88 &&
        ScrollCaptureRules::scrollSettleMs == 250 && ScrollCaptureRules::settleRecheckMs == 250 &&
        ScrollCaptureRules::retryDelayMs == 500);
    try { actualPagePixels(); stationaryAndCaptureFailure(); changedButUnmatched(); heightAndMemoryLimits(); progressResetsRetries(); manualReadingAndRetainedOutput(); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
