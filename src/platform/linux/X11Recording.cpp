// Native polling and borrowed BGRA -> bounded lease copying reuse this project's
// PR #7 at 251bc0071278c6b711c840bf5188d16870bc7349, FrameSource.cpp.
// Copyright (c) 2026 NAMEWTA. MIT; see third_party/pipewire-audio/upstream.json.
#include "platform/linux/X11Recording.h"
#include "platform/linux/X11Capture.h"
#include "platform/linux/LiveRecording.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
using namespace Recording;
using Clock = Core::MediaClock;

bool waylandEnvironment() noexcept
{
    const auto* display = std::getenv("WAYLAND_DISPLAY");
    const auto* session = std::getenv("XDG_SESSION_TYPE");
    return (display && *display) || (session && std::string_view(session) == "wayland");
}

struct Source final {
    std::shared_ptr<Core::FrameQueue> frames{std::make_shared<Core::FrameQueue>(2)};
    std::shared_ptr<std::atomic<Core::LiveSourceActivity>> activity{
        std::make_shared<std::atomic<Core::LiveSourceActivity>>(Core::LiveSourceActivity::Starting)};
    std::stop_source stop;
    std::mutex mutex;
    std::condition_variable_any changed;
    Core::FrameLease first;
    bool ready{};
    Core::FrameSourceTerminal terminal;
    std::jthread worker;

    Source(const BackendRequest& request, PixelRegion region)
    {
        worker = std::jthread([this, request, region](std::stop_token emergency) {
            std::stop_callback terminate(emergency, [this] { stop.request_stop(); });
            run(request, region);
        });
    }
    ~Source() { finish(); }
    void finish() noexcept
    {
        stop.request_stop();
        if (worker.joinable()) worker.join();
    }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    void run(const BackendRequest& request, PixelRegion region) noexcept
    {
        const auto fail = [&](Core::FrameSourceEnd reason, int code, std::string_view detail) noexcept {
            if (terminal.reason != Core::FrameSourceEnd::Stopped) return;
            terminal.reason = reason; terminal.nativeCode = code;
            try { terminal.detail.assign(detail); } catch (...) {}
        };
        try {
            // The connection, SHM segment and borrowed image all live and die
            // here. No Qt connection, native handle or widget crosses threads.
            X11Capture capture(stop.get_token());
            const auto produce = [&] {
                if (stop.stop_requested()) return;
                if (!capture.initialize() || capture.isXWayland()) {
                    if (!stop.stop_requested()) fail(capture.isXWayland() ? Core::FrameSourceEnd::UnsupportedFormat :
                        Core::FrameSourceEnd::PlatformFailure, EIO, capture.error());
                    return;
                }
                X11Desktop baseline;
                if (!capture.desktop(baseline, false)) {
                    if (!stop.stop_requested()) fail(Core::FrameSourceEnd::PlatformFailure, EIO, capture.error());
                    return;
                }
                const auto& bounds = baseline.bounds;
                if (region.x < bounds.x || region.y < bounds.y ||
                    std::int64_t{region.x} + region.width > std::int64_t{bounds.x} + bounds.width ||
                    std::int64_t{region.y} + region.height > std::int64_t{bounds.y} + bounds.height) {
                    fail(Core::FrameSourceEnd::SourceClosed, EINVAL, "The selected recording region is outside the current X11 root"); return;
                }
                const auto format = Core::packedFrameFormat(Core::FramePixelFormat::Bgra8, region.width, region.height,
                    {Core::ColorPrimaries::Bt709, Core::ColorTransfer::Srgb, Core::ColorMatrix::Rgb, Core::ColorRange::Full});
                Core::FramePool pool({4, static_cast<std::size_t>((std::min)(request.maximumFrameBytes * 4,
                    std::uint64_t{512} * 1024 * 1024)), 16384});
                if (!format || pool.configure(*format) != Core::FramePoolStatus::Ready) {
                    fail(Core::FrameSourceEnd::BudgetExceeded, ENOBUFS, "The X11 recording exceeds its four-slot pixel budget"); return;
                }
                const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
                auto next = Clock::now();
                std::uint64_t sequence{};
                std::mutex waitMutex; std::condition_variable_any wait;
                while (!stop.stop_requested()) {
                    X11Desktop current;
                    if (!capture.desktop(current, false)) {
                        if (!stop.stop_requested()) fail(Core::FrameSourceEnd::PlatformFailure, EIO, capture.error());
                        return;
                    }
                    if (current.bounds != baseline.bounds || current.monitors != baseline.monitors) {
                        fail(Core::FrameSourceEnd::SourceClosed, EPIPE, "The actual X11 desktop layout changed during recording"); return;
                    }
                    auto target = pool.tryAcquire();
                    if (!target) { fail(Core::FrameSourceEnd::BudgetExceeded, ENOBUFS, "The X11 recording consumer retained too many native frames"); return; }
                    if (!capture.capture(region.x, region.y, region.width, region.height, request.cursor)) {
                        if (!stop.stop_requested()) fail(Core::FrameSourceEnd::PlatformFailure, EIO, capture.error());
                        return;
                    }
                    const auto pixels = capture.pixels();
                    const auto stride = capture.stride();
                    const auto rowBytes = static_cast<std::size_t>(region.width) * 4;
                    const auto rows = static_cast<std::size_t>(region.height);
                    if (capture.width() != region.width || capture.height() != region.height || stride < rowBytes ||
                        (rows > 1 && stride > ((std::numeric_limits<std::size_t>::max)() - rowBytes) / (rows - 1)) ||
                        pixels.size() < (rows - 1) * stride + rowBytes) {
                        fail(Core::FrameSourceEnd::PlatformFailure, EINVAL, "The actual X11 readback has incomplete pixel rows"); return;
                    }
                    for (std::uint32_t row = 0; row < static_cast<std::uint32_t>(rows); ++row)
                        std::memcpy(target->row(0, row).data(), pixels.data() + row * stride, rowBytes);
                    Core::FrameMetadata metadata;
                    metadata.sessionId = 1; metadata.sourceId = 1;
                    metadata.timing.receivedAt = Clock::now();
                    metadata.timing.sourceSequence = sequence++;
                    auto frame = std::move(*target).publish(metadata);
                    if (!frame) { fail(Core::FrameSourceEnd::PlatformFailure, EINVAL, "The actual X11 frame metadata is invalid"); return; }
                    if (sequence == 1) {
                        { std::lock_guard lock(mutex); first = std::move(frame); ready = true;
                            activity->store(Core::LiveSourceActivity::Streaming, std::memory_order_release); }
                        changed.notify_all();
                    } else if (!frames->push(std::move(frame))) return;
                    next += interval;
                    const auto now = Clock::now();
                    if (next < now) next += interval * ((now - next) / interval + 1);
                    std::unique_lock lock(waitMutex);
                    wait.wait_until(lock, stop.get_token(), next, [] { return false; });
                }
            };
            produce();
        } catch (const std::bad_alloc&) { fail(Core::FrameSourceEnd::BudgetExceeded, ENOMEM, "The X11 recording source exhausted memory"); }
        catch (...) { fail(Core::FrameSourceEnd::PlatformFailure, EIO, "The X11 recording source could not complete capture"); }
        // Native objects have actually gone away before the queue terminates.
        activity->store(Core::LiveSourceActivity::Closed, std::memory_order_release);
        try { frames->close(terminal); }
        catch (...) { frames->close({Core::FrameSourceEnd::BudgetExceeded, ENOMEM, {}}); }
        { std::lock_guard lock(mutex); ready = true; }
        changed.notify_all();
    }
};

BackendResult record(const BackendRequest& request, FileHandle& writer,
    const BackendEvents& events, std::stop_token stop)
{
    BackendResult result;
    const auto fail = [&](Core::RecordingStage stage, int code, std::string_view detail) {
        result.media.fail(stage, code); result.detail.assign(detail);
    };
    if (stop.stop_requested()) return result;
    const auto region = request.format == Format::Mp4 ? alignedMp4Region(request.region, request.maximumFrameBytes) :
        std::optional<PixelRegion>{request.region};
    const auto layout = region ? Core::pixelLayout(region->width, region->height, 4) : std::nullopt;
    if (!writer || (request.format != Format::Mp4 && request.format != Format::Gif) ||
        request.coordinates != CoordinateSpace::DesktopPixels || request.sourceId || request.generation ||
        !region || region->x < 0 || region->y < 0 || !layout || layout->bytes > request.maximumFrameBytes ||
        !request.maximumFrameBytes || request.maximumFrameBytes > 256ull * 1024 * 1024 ||
        !request.framesPerSecond || request.framesPerSecond > 240 || request.firstFrameTimeout.count() <= 0 ||
        request.firstFrameTimeout > std::chrono::minutes(1)) {
        fail(Core::RecordingStage::Prepare, EINVAL, "The native X11 recording request has an invalid desktop coordinate domain or resource limits"); return result;
    }
    if (waylandEnvironment()) {
        fail(Core::RecordingStage::Prepare, ENOTSUP, "Wayland recording requires its authorized compositor source"); return result;
    }
    Source source(request, *region);
    std::stop_callback cancel(stop, [&source] { source.stop.request_stop(); source.changed.notify_all(); });
    bool ready{};
    Core::FrameLease first;
    {
        std::unique_lock lock(source.mutex);
        source.changed.wait_until(lock, stop, Clock::now() + request.firstFrameTimeout, [&source] { return source.ready; });
        ready = source.ready;
        first = std::move(source.first);
    }
    if (stop.stop_requested() || !ready || !first) {
        const bool timeout = !ready && !stop.stop_requested();
        source.finish();
        result.sourceTerminal = source.terminal;
        if (timeout) fail(Core::RecordingStage::Capture, ETIMEDOUT, "The native X11 source did not supply its first recording pixels before the deadline");
        else if (!stop.stop_requested()) fail(Core::RecordingStage::Capture,
            source.terminal.nativeCode ? static_cast<int>(source.terminal.nativeCode) : EIO, source.terminal.detail);
        return result;
    }
    Core::LiveSourceConsumer consumer{1, 1, {0, 0, static_cast<std::uint32_t>(region->width), static_cast<std::uint32_t>(region->height)},
        source.frames, source.activity, source.stop, std::move(first), Clock::now()};
    // This mapping is established by the actual readback of exactly region.
    // There is no DPI inference or unrelated screenshot/Portal rectangle.
    auto local = request;
    local.region = {0, 0, region->width, region->height};
    local.coordinates = CoordinateSpace::AuthorizedSourcePixels; local.sourceId = local.generation = 1;
    const auto desktop = [region = *region](PreparedRecording value) {
        value.region.x += region.x; value.region.y += region.y;
        value.source = CaptureSource::X11; value.coordinates = CoordinateSpace::DesktopPixels;
        value.sourceId = value.generation = 0; return value;
    };
    BackendEvents mapped{[&](const auto& value) { if (events.prepared) events.prepared(desktop(value)); }, events.finalizing};
    auto consume = makeLiveRecordingBackend(std::move(consumer));
    result = consume(local, std::move(writer), mapped, stop);
    source.finish();
    if (result.prepared) result.prepared = desktop(*result.prepared);
    return result;
}
}

Recording::Backend makeX11RecordingBackend()
{
    return [](const BackendRequest& request, FileHandle writer, const BackendEvents& events, std::stop_token stop) {
        BackendResult result;
        try { result = record(request, writer, events, stop); }
        catch (const std::bad_alloc&) { result.media.fail(Core::RecordingStage::Prepare, ENOMEM); }
        catch (...) { result.media.fail(Core::RecordingStage::Capture, EIO); }
        result.stopRequested = stop.stop_requested();
        std::error_code error;
        if (!writer.close(error)) result.media.finish(false, error.value());
        return result;
    };
}

} // namespace HuhuCapture::Platform::Linux
