#include "recording/GifRecording.h"
#include "gifski.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace HuhuCapture::Recording {
namespace {
struct Output {
    int descriptor;
    std::atomic<int> error{};
    std::uint64_t bytes{};
    static int write(std::size_t count, const std::uint8_t* pixels, void* context) noexcept
    {
        auto& output = *static_cast<Output*>(context);
        constexpr std::uint64_t maximumBytes = 32ull * 1024 * 1024 * 1024;
        if (count > maximumBytes - output.bytes || (count && !pixels)) {
            int empty{}; output.error.compare_exchange_strong(empty, EFBIG); return GIFSKI_WRITE_ZERO;
        }
        while (count) {
            const auto chunk = (std::min)(count, static_cast<std::size_t>(1024 * 1024));
#ifdef _WIN32
            const auto written = _write(output.descriptor, pixels, static_cast<unsigned>(chunk));
#else
            const auto written = ::write(output.descriptor, pixels, chunk);
#endif
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                int empty{}; output.error.compare_exchange_strong(empty, written == 0 ? EIO : errno);
                return GIFSKI_WRITE_ZERO;
            }
            pixels += written; count -= static_cast<std::size_t>(written);
            output.bytes += static_cast<std::uint64_t>(written);
        }
        return GIFSKI_OK;
    }
};

bool convertRgb(const Core::FrameLease& frame, PixelRegion region, std::vector<std::uint8_t>& rgb)
{
    using namespace Core;
    const auto& format = frame.format();
    if (format.width != region.width || format.height != region.height ||
        (format.pixelFormat != FramePixelFormat::Bgra8 && format.pixelFormat != FramePixelFormat::Rgba8) ||
        frame.metadata().crop || frame.metadata().transform != FrameTransform::None || frame.metadata().timing.discontinuity ||
        format.color.transfer == ColorTransfer::Pq || format.color.transfer == ColorTransfer::Hlg ||
        format.alpha != FrameAlpha::Opaque) return false;
    const bool bgra = format.pixelFormat == FramePixelFormat::Bgra8;
    for (int y = 0; y < region.height; ++y) {
        const auto source = frame.row(0, static_cast<std::uint32_t>(y));
        if (source.size() < static_cast<std::size_t>(region.width) * 4) return false;
        auto* target = rgb.data() + static_cast<std::size_t>(y) * region.width * 3;
        for (int x = 0; x < region.width; ++x) {
            target[x * 3] = source[x * 4 + (bgra ? 2 : 0)];
            target[x * 3 + 1] = source[x * 4 + 1];
            target[x * 3 + 2] = source[x * 4 + (bgra ? 0 : 2)];
        }
    }
    return true;
}
}

BackendResult runGifRecording(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> source, const BackendEvents& events, std::stop_token stop)
{
    using Core::RecordingStage;
    BackendResult result;
    gifski* encoder{};
    Output output{writer.get()};
    const auto fail = [&](RecordingStage stage, std::int64_t code, const char* detail) {
        if (!result.media.firstError) result.detail = detail;
        result.media.fail(stage, code);
    };
    try {
        const auto capture = [&] {
            const auto layout = Core::pixelLayout(request.region.width, request.region.height, 4);
            if (request.format != Format::Gif || !writer || !source || !layout ||
                layout->bytes > request.maximumFrameBytes || request.region.width > 16384 ||
                request.region.height > 16384 || !request.framesPerSecond || request.framesPerSecond > 16 ||
                request.maximumDuration.count() <= 0 || request.maximumDuration > std::chrono::minutes(6) ||
                request.firstFrameTimeout.count() <= 0 || request.firstFrameTimeout > std::chrono::minutes(1) ||
                request.systemAudio || request.microphone) {
                fail(RecordingStage::Prepare, EINVAL, "Invalid GIF recording parameters"); return;
            }
            if (stop.stop_requested()) return;
            std::error_code identityError;
            const auto identity = Platform::readFileDescriptorSnapshot(writer.get(), identityError);
            if (!identity || identity->bytes) {
                fail(RecordingStage::Prepare, EINVAL, "GIF output is not an owned empty ordinary file"); return;
            }
            const auto rgbLayout = Core::pixelLayout(request.region.width, request.region.height, 3);
            if (!rgbLayout) { fail(RecordingStage::Prepare, EOVERFLOW, "Invalid GIF pixel geometry"); return; }
            std::vector<std::uint8_t> rgb(rgbLayout->bytes);
            Core::FrameLease latest;
            const auto firstDeadline = Core::MediaClock::now() + request.firstFrameTimeout;
            const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
            Core::MediaClock::time_point nextFrame = Core::MediaClock::now();
            std::mutex mutex;
            std::condition_variable_any wait;
            while (!stop.stop_requested()) {
                const auto now = Core::MediaClock::now();
                if (result.prepared && now - result.prepared->started >= request.maximumDuration) {
                    result.durationLimitReached = true; break;
                }
                auto received = source->read(result.prepared ? nextFrame : firstDeadline, stop);
                if (stop.stop_requested()) break;
                if (received.status == FrameReadStatus::Stopped) {
                    fail(RecordingStage::Capture, EPIPE, "The recording source stopped unexpectedly"); break;
                }
                if (received.status == FrameReadStatus::Failed) {
                    if (!result.media.firstError) result.detail = std::move(received.detail);
                    result.media.fail(RecordingStage::Capture, received.error ? received.error : EIO); break;
                }
                if (received.status == FrameReadStatus::Frame) {
                    if (!received.frame) { fail(RecordingStage::Capture, EINVAL, "The frame source returned no pixels"); break; }
                    latest = std::move(received.frame);
                }
                if (!latest) {
                    if (Core::MediaClock::now() >= firstDeadline) {
                        fail(RecordingStage::Capture, ETIMEDOUT, "Timed out waiting for the first recording frame"); break;
                    }
                    std::unique_lock lock(mutex);
                    wait.wait_until(lock, stop, (std::min)(firstDeadline, Core::MediaClock::now() +
                        std::chrono::milliseconds(5)), [] { return false; });
                    continue;
                }
                if (!convertRgb(latest, request.region, rgb)) {
                    fail(RecordingStage::Capture, EINVAL, "The GIF source geometry or color format changed"); break;
                }
                if (!encoder) {
                    // Preserve the shipping quality=80, fast=true and infinite
                    // loop settings. One aggregate Rust library exports gifski.
                    const GifskiSettings settings{static_cast<std::uint32_t>(request.region.width),
                        static_cast<std::uint32_t>(request.region.height), 80, true, 0};
                    encoder = gifski_new(&settings);
                    if (!encoder) { fail(RecordingStage::Prepare, GIFSKI_INVALID_INPUT, "Cannot create the GIF encoder"); break; }
                    const auto status = gifski_set_write_callback(encoder, &Output::write, &output);
                    if (status != GIFSKI_OK) { fail(RecordingStage::Prepare, status, "Cannot attach the GIF output file"); break; }
                    result.prepared = PreparedRecording{request.region, Codec::Gif, source->kind(),
                        Core::MediaClock::now(), false, false};
                    nextFrame = result.prepared->started;
                    if (events.prepared) events.prepared(*result.prepared);
                }
                if (stop.stop_requested()) break;
                const auto index = result.media.framesWritten;
                if (index > (std::numeric_limits<std::uint32_t>::max)()) {
                    fail(RecordingStage::Encode, EOVERFLOW, "GIF frame counter overflow"); break;
                }
                // Real monotonic elapsed time preserves slow-frame/backpressure
                // duration; index/fps would speed the result up under load.
                const double timestamp = index ? std::chrono::duration<double>(
                    Core::MediaClock::now() - result.prepared->started).count() : 0.0;
                const auto status = gifski_add_frame_rgb(encoder, static_cast<std::uint32_t>(index),
                    static_cast<std::uint32_t>(request.region.width), static_cast<std::uint32_t>(rgbLayout->stride),
                    static_cast<std::uint32_t>(request.region.height), rgb.data(), timestamp);
                if (status != GIFSKI_OK) { fail(RecordingStage::Encode, status, "The GIF encoder rejected a frame"); break; }
                ++result.media.framesWritten;
                nextFrame += interval;
                const auto after = Core::MediaClock::now();
                if (nextFrame < after) nextFrame += interval * ((after - nextFrame) / interval + 1);
                const auto stopAt = result.prepared->started + request.maximumDuration;
                std::unique_lock lock(mutex);
                wait.wait_until(lock, stop, (std::min)(nextFrame, stopAt), [] { return false; });
            }
        };
        capture();
    } catch (const std::bad_alloc&) { fail(RecordingStage::Encode, ENOMEM, "The GIF recording exceeded available memory"); }
    catch (...) { fail(RecordingStage::Encode, EIO, "The GIF recording worker failed"); }

    // A source may own a PipeWire producer. Join/close it before the encoder's
    // final output, never on the GUI thread and never while holding a PW buffer.
    if (source) source->stopAndWait();
    source.reset();
    if (result.prepared) result.elapsed = std::chrono::duration_cast<Core::MediaTime>(
        Core::MediaClock::now() - result.prepared->started);
    if (encoder) {
        if (events.finalizing) {
            try { events.finalizing(); }
            catch (...) { fail(RecordingStage::Finalize, EIO, "The finalization callback failed"); }
        }
        const auto status = gifski_finish(encoder);
        // Cancellation before any submitted frame does not produce media; an
        // upstream no-frame error is expected, but actual write failures remain.
        if (result.media.framesWritten || !stop.stop_requested()) result.media.finish(status == GIFSKI_OK, status);
        if (const auto error = output.error.load()) result.media.finish(false, error);
    }
    std::error_code closeError;
    if (!writer.close(closeError)) result.media.finish(false, closeError.value());
    result.stopRequested = stop.stop_requested();
    return result;
}

} // namespace HuhuCapture::Recording
