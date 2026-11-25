#include "platform/windows/NativeGifRecording.h"
#include "core/GifLimits.h"
#include "platform/windows/GdiCapture.h"
#include "services/media/GifEncoder.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>

namespace HuhuCapture::Platform::Windows {
namespace {
using namespace Recording;
using Core::RecordingStage;

class PhysicalPixelScope final {
public:
    PhysicalPixelScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixelScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
    explicit operator bool() const noexcept { return previous_ != nullptr; }
private:
    DPI_AWARENESS_CONTEXT previous_;
};

BackendResult capture(const BackendRequest& request, FileHandle& writer,
    const BackendEvents& events, std::stop_token stop)
{
    BackendResult result;
    const auto fail = [&](HRESULT error) {
        result.media.fail(RecordingStage::Prepare, error); return result;
    };
    const auto& region = request.region;
    const auto layout = Core::pixelLayout(region.width, region.height, 4);
    if (request.format != Format::Gif || !layout || layout->bytes > request.maximumFrameBytes ||
        region.width > 65535 || region.height > 65535 || !request.framesPerSecond || request.framesPerSecond > 50 ||
        request.systemAudio || request.microphone || request.maximumDuration.count() <= 0 ||
        request.maximumDuration > std::chrono::minutes(6) || request.firstFrameTimeout.count() <= 0 ||
        request.firstFrameTimeout > std::chrono::minutes(1)) return fail(E_INVALIDARG);
    const auto maximumFrames = (request.maximumDuration.count() * request.framesPerSecond + 999) / 1000 + 1;
    if (maximumFrames > Core::maximumValidatedGifFrames) return fail(E_INVALIDARG);
    // The fixed upstream enforces a two-centisecond minimum frame delay. Its
    // default 16 FPS is retained; requesting >50 FPS cannot truthfully represent
    // that rate in this encoder and is rejected instead of silently slowed.
    if (stop.stop_requested()) { result.stopRequested = true; return result; }
    PhysicalPixelScope pixels;
    if (!pixels) return fail(HRESULT_FROM_WIN32(GetLastError()));
    const auto x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0 || region.x < x || region.y < y ||
        std::int64_t{region.x} + region.width > std::int64_t{x} + width ||
        std::int64_t{region.y} + region.height > std::int64_t{y} + height) return fail(E_INVALIDARG);

    Media::GifEncoder encoder({region.width, region.height, request.maximumFrameBytes}, std::move(writer));
    if (!encoder.ready()) { result.media = encoder.finish(); return result; }
    GdiCapture source;
    const auto firstDeadline = Core::MediaClock::now() + request.firstFrameTimeout;
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
    std::mutex waitMutex;
    std::condition_variable_any wake;
    try {
        while (!stop.stop_requested()) {
            const auto frameStarted = Core::MediaClock::now();
            if (result.prepared && frameStarted - result.prepared->started >= request.maximumDuration) {
                result.durationLimitReached = true; break;
            }
            if (!source.capture(region.x, region.y, region.width, region.height, request.cursor)) {
                result.media.fail(RecordingStage::Capture, HRESULT_FROM_WIN32(ERROR_GEN_FAILURE)); break;
            }
            if (!result.prepared && Core::MediaClock::now() >= firstDeadline) {
                result.media.fail(RecordingStage::Capture, HRESULT_FROM_WIN32(ERROR_TIMEOUT)); break;
            }
            if (stop.stop_requested()) break;
            // Preserve the existing constant-rate GIF media timeline. Pacing
            // uses a steady clock and stop-aware wait; Qt timers do not supply
            // input PTS and no frames are silently assigned duplicate PTS.
            const auto index = encoder.result().framesWritten;
            const Core::MediaTime time(static_cast<std::int64_t>(index * 10'000'000 / request.framesPerSecond));
            if (!encoder.addBgra(source.pixels(), source.stride(), time)) break;
            if (!result.prepared) {
                result.prepared = PreparedRecording{region, Codec::Gif, CaptureSource::Gdi, frameStarted, false, false};
                if (events.prepared) events.prepared(*result.prepared);
            }
            const auto next = (std::min)(frameStarted + interval,
                result.prepared->started + request.maximumDuration);
            std::unique_lock lock(waitMutex);
            wake.wait_until(lock, stop, next, [] { return false; });
        }
    } catch (const std::bad_alloc&) { result.media.fail(RecordingStage::Encode, E_OUTOFMEMORY); }
    catch (...) { result.media.fail(RecordingStage::Encode, E_FAIL); }
    result.stopRequested = stop.stop_requested();
    if (result.prepared) {
        result.elapsed = std::chrono::duration_cast<Core::MediaTime>(Core::MediaClock::now() - result.prepared->started);
        try { if (events.finalizing) events.finalizing(); }
        catch (...) { result.media.fail(RecordingStage::Finalize, E_FAIL); }
    }
    // Source/notification failure observed before finish remains primary. The
    // shared encoder still drains, checks its real writer and closes the fd.
    const auto encoded = encoder.finish();
    if (encoded.firstError) result.media.fail(encoded.firstError->stage, encoded.firstError->code);
    if (encoded.finalizationError) result.media.fail(RecordingStage::Finalize, encoded.finalizationError->code);
    result.media.framesWritten = encoded.framesWritten;
    result.media.finalized = encoded.finalized && !result.media.finalizationError;
    return result;
}
}

Recording::BackendResult runNativeGif(const Recording::BackendRequest& request,
    FileHandle writer, const Recording::BackendEvents& events, std::stop_token stop)
{
    Recording::BackendResult result;
    try {
        if (!writer) result.media.fail(Core::RecordingStage::Prepare, E_INVALIDARG);
        else result = capture(request, writer, events, stop);
    } catch (const std::bad_alloc&) { result.media.fail(Core::RecordingStage::Encode, E_OUTOFMEMORY); }
    catch (...) { result.media.fail(Core::RecordingStage::Encode, E_FAIL); }
    result.stopRequested = stop.stop_requested();
    std::error_code error;
    if (!writer.close(error)) result.media.finish(false, error.value());
    return result;
}

} // namespace HuhuCapture::Platform::Windows
