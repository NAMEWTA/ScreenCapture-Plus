#include "recording/FramePixels.h"
#include "recording/FrameSource.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace core = HuhuCapture::Core;
namespace rec = HuhuCapture::Recording;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
core::FrameMetadata metadata()
{
    core::FrameMetadata result;
    result.sessionId = 17; result.sourceId = 29; result.timing.receivedAt = core::MediaClock::now();
    return result;
}
core::FrameLease pixels(core::FramePool& pool, core::FrameFormat format, core::FrameMetadata info, bool configure = true)
{
    if (configure) require(pool.configure(format) == core::FramePoolStatus::Ready, "Configure the actual bounded frame pool");
    auto writable = pool.tryAcquire(); require(writable.has_value(), "Acquire real owned pixel storage");
    std::fill(writable->plane(0).begin(), writable->plane(0).end(), 0xcd);
    for (int y = 0; y < format.height; ++y) {
        auto row = writable->row(0, static_cast<unsigned>(y));
        for (int x = 0; x < format.width; ++x) {
            const auto value = static_cast<std::uint8_t>(y * format.width + x + 1);
            const bool rgba = format.pixelFormat == core::FramePixelFormat::Rgba8;
            row[x * 4] = rgba ? value + 20 : value;
            row[x * 4 + 1] = value + 10; row[x * 4 + 2] = rgba ? value : value + 20;
            row[x * 4 + 3] = 77;
        }
    }
    return std::move(*writable).publish(info);
}
class OneFrame final : public core::FrameSource {
public:
    explicit OneFrame(core::FrameLease frame) : frame_(std::move(frame)) {}
    core::FrameSourceKind kind() const noexcept override { return core::FrameSourceKind::PipeWire; }
    core::FrameRead read(core::MediaClock::time_point, std::stop_token stop) override
    {
        if (stop.stop_requested()) return {core::FrameReadStatus::Stopped, {}, 0, {}};
        if (!frame_) return {core::FrameReadStatus::Idle, {}, 0, {}};
        return {core::FrameReadStatus::Frame, std::exchange(frame_, {}), 0, {}};
    }
private:
    core::FrameLease frame_;
};
void transforms()
{
    constexpr std::array<std::array<unsigned, 6>, 8> expected{{
        {1, 2, 3, 4, 5, 6}, {3, 6, 2, 5, 1, 4}, {6, 5, 4, 3, 2, 1}, {4, 1, 5, 2, 6, 3},
        {3, 2, 1, 6, 5, 4}, {1, 4, 2, 5, 3, 6}, {4, 5, 6, 1, 2, 3}, {6, 3, 5, 2, 4, 1}}};
    for (unsigned transform = 0; transform < expected.size(); ++transform) {
        for (const bool rgba : {false, true}) for (const bool inverted : {false, true}) {
            auto format = *core::packedFrameFormat(rgba ? core::FramePixelFormat::Rgba8 : core::FramePixelFormat::Bgra8, 3, 2);
            format.bytes = 48; format.planes[0].size = 48; format.planes[0].stride = inverted ? -24 : 24;
            format.planes[0].firstRow = inverted ? 24 : 0;
            core::FramePool pool({2, 1024, 64}); auto info = metadata();
            info.transform = static_cast<core::FrameTransform>(transform);
            auto frame = pixels(pool, format, info); require(bool(frame), "Publish padded positive/negative stride source");
            const auto size = rec::normalizedFrameSize(frame);
            require(size && *size == (transform % 2 ? std::pair{2, 3} : std::pair{3, 2}), "Normalize transform dimensions");
            const auto stride = static_cast<std::size_t>(size->first * 4 + 4);
            std::vector<std::uint8_t> output(stride * size->second, 0xab);
            require(rec::copyFrameBgra(frame, {0, 0, size->first, size->second}, output, stride), "Normalize the actual owned pixels");
            for (int y = 0; y < size->second; ++y) for (int x = 0; x < size->first; ++x) {
                const auto offset = static_cast<std::size_t>(y) * stride + x * 4;
                const auto value = expected[transform][static_cast<std::size_t>(y) * size->first + x];
                require(output[offset] == value && output[offset + 1] == value + 10 && output[offset + 2] == value + 20 && output[offset + 3] == 255,
                    "All eight transforms preserve channel order and opaque alpha");
                require(output[static_cast<std::size_t>(y) * stride + size->first * 4] == 0xab, "Destination padding is untouched");
            }
            require(!rec::copyFrameBgra(frame, {-1, 0, 1, 1}, output, stride), "Negative source selection is rejected");
            require(!rec::copyFrameBgra(frame, {0, 0, size->first + 1, 1}, output, stride), "Selection never extends beyond its authorized pixels");
            require(!rec::copyFrameBgra(frame, {0, 0, size->first, size->second}, std::span(output).first(4), stride), "Incomplete destination allocation is rejected");
        }
    }
    core::FramePool pool({2, 1024, 64}); auto info = metadata();
    info.crop = core::FrameCrop{1, 1, 3, 2}; info.transform = core::FrameTransform::Rotate90;
    auto frame = pixels(pool, *core::packedFrameFormat(core::FramePixelFormat::Bgra8, 4, 3), info);
    std::array<std::uint8_t, 8> selected{};
    require(rec::copyFrameBgra(frame, {0, 1, 2, 1}, selected, 8) && selected[0] == 7 && selected[4] == 11,
        "Crop precedes source transform and the user's normalized selection");
    auto hdr = frame.format(); hdr.color.transfer = core::ColorTransfer::Pq;
    frame.reset(); frame = pixels(pool, hdr, info);
    require(!rec::copyFrameBgra(frame, {0, 1, 2, 1}, selected, 8), "HDR is explicitly unsupported, never tagged as SDR");
}
void authorizationContract()
{
    core::FramePool pool({4, 4096, 64});
    auto format = *core::packedFrameFormat(core::FramePixelFormat::Bgra8, 4, 4);
    auto info = metadata(); auto preview = pixels(pool, format, info);
    const rec::FrameSourceContract contract{preview.format(), preview.metadata(), preview.formatGeneration(), {4, 4}};
    auto source = rec::makeCroppedFrameSource(std::make_unique<OneFrame>(pixels(pool, format, info, false)), {1, 1, 2, 2}, contract, 1024);
    require(bool(source), "Admit a selection from the exact authorized preview");
    auto read = source->read(core::MediaClock::now(), {});
    require(read.status == core::FrameReadStatus::Frame && read.frame && read.frame.row(0, 0)[0] == 6 &&
        read.frame.row(0, 1)[4] == 11 && !read.frame.metadata().crop && read.frame.metadata().transform == core::FrameTransform::None,
        "Selected source owns exactly the requested cropped pixels");
    read.frame.reset(); source.reset();
    for (const unsigned change : {0u, 1u, 2u}) {
        auto changed = info;
        if (change == 0) changed.sourceId++;
        if (change == 1) changed.transform = core::FrameTransform::Rotate180;
        if (change == 2) changed.crop = core::FrameCrop{0, 0, 4, 4};
        source = rec::makeCroppedFrameSource(std::make_unique<OneFrame>(pixels(pool, format, changed, false)), {0, 0, 4, 4}, contract, 1024);
        read = source->read(core::MediaClock::now(), {});
        require(read.status == core::FrameReadStatus::Failed && !read.frame,
            "A changed authorized identity, transform or crop is rejected even with equal dimensions");
    }
    source.reset();
    auto other = format; other.pixelFormat = core::FramePixelFormat::Rgba8;
    require(pool.configure(other) == core::FramePoolStatus::Ready, "Change the real source format generation");
    auto changedGeneration = pixels(pool, format, info);
    require(changedGeneration.formatGeneration() != contract.generation, "The actual pool reports the new generation");
    source = rec::makeCroppedFrameSource(std::make_unique<OneFrame>(std::move(changedGeneration)), {0, 0, 4, 4}, contract, 1024);
    require(source->read(core::MediaClock::now(), {}).status == core::FrameReadStatus::Failed,
        "Returning to old dimensions never reauthorizes a stale source selection");
}
void boundedPolling()
{
    auto request = rec::recordingDefaults(rec::Format::Gif); request.region = {0, 0, 4, 2};
    std::array<std::uint8_t, 32> native{}; native.fill(19); unsigned calls{};
    auto source = rec::makePollingFrameSource(request, rec::CaptureSource::Gdi,
        [&](rec::PixelRegion, bool) { ++calls; return rec::BorrowedBgraFrame{native, 16, 0, {}}; });
    auto first = source->read(core::MediaClock::now(), {}); native.fill(71);
    require(first.frame && first.frame.row(0, 0)[0] == 19, "Borrowed native storage is copied before its next mutation");
    auto second = source->read(core::MediaClock::now(), {});
    require(second.frame && second.frame.row(0, 0)[0] == 71, "The second bounded pool slot receives new pixels");
    require(source->read(core::MediaClock::now(), {}).status == core::FrameReadStatus::Failed && calls == 2,
        "Retaining every pool slot fails before another native capture or allocation");
    first.frame.reset(); second.frame.reset();
    std::stop_source stop; stop.request_stop();
    require(source->read(core::MediaClock::now(), stop.get_token()).status == core::FrameReadStatus::Stopped && calls == 2,
        "Cancellation performs no additional native capture");
}
}

int main()
{
    try {
        transforms(); authorizationContract(); boundedPolling();
        std::cout << "PASS real frame storage: eight transforms, crop/channel/stride bounds, source generations and bounded polling\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
