#include "recording/PlatformRecording.h"
#include "recording/GifRecording.h"
#include "recording/SoftwareMp4Recording.h"
#include "platform/linux/X11Capture.h"
#include "platform/linux/pipewire/PipeWireAudio.h"

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <memory>
#include <mutex>

namespace HuhuCapture::Recording {
namespace {
BackendResult run(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> source, const BackendEvents& events, std::stop_token stop)
{
    if (request.format == Format::Gif) return runGifRecording(request, std::move(writer), std::move(source), events, stop);
    return runSoftwareMp4Recording(request, std::move(writer), std::move(source),
        Platform::Linux::makePipeWireAudio(request.systemAudio, request.microphone,
            request.maximumQueuedAudioBytes), events, stop);
}
}

ControlsExclusion excludeControlsFromCapture(std::uintptr_t)
{ return ControlsExclusion::Unsupported; }

Backend desktopRecordingBackend(DesktopRecordingControls controls)
{
    return [controls](const BackendRequest& request, Platform::FileHandle writer, const BackendEvents& events, std::stop_token stop) {
        // Reuse PR #6's explicit platform refusal at the native entry itself;
        // a caller cannot silently turn an XWayland root into authorized pixels.
        if (stop.stop_requested()) { BackendResult result; result.stopRequested = true; return result; }
        const auto* display = std::getenv("WAYLAND_DISPLAY");
        const auto* session = std::getenv("XDG_SESSION_TYPE");
        if ((display && *display) || (session && std::string_view(session) == "wayland")) {
            BackendResult result;
            result.detail = "A Wayland desktop requires an authorized ScreenCast source";
            result.media.fail(Core::RecordingStage::Prepare, ENOTSUP); return result;
        }
        // Construct and destroy the X connection on the media worker. The GUI
        // routes Wayland/XWayland selections through a fresh ScreenCast grant.
        auto capture = std::make_shared<Platform::Linux::X11Capture>(stop);
        auto guarded = request;
        if (guarded.format == Format::Mp4) {
            if (const auto aligned = alignedMp4Region(guarded.region, guarded.maximumFrameBytes)) guarded.region = *aligned;
        }
        if (!guarded.controls && controls.nativeWindow) {
            guarded.controls = std::make_shared<Core::CaptureControls>();
            auto snapshot = std::make_shared<Core::CaptureControlSnapshot>();
            snapshot->surfaces.push_back({controls.nativeWindow, Core::CaptureControlPlacement::Outside});
            guarded.controls->snapshot.store(std::move(snapshot), std::memory_order_release);
        }
        auto source = makePollingFrameSource(guarded, CaptureSource::X11,
            [capture, controlState = guarded.controls, baseline = std::optional<Platform::Linux::X11Desktop>{}]
            (PixelRegion region, bool cursor) mutable -> BorrowedBgraFrame {
                Platform::Linux::X11Desktop current;
                if (!capture->desktop(current, false)) return {{}, 0, EIO, capture->error()};
                if (capture->isXWayland()) return {{}, 0, ENOTSUP, "XWayland cannot replace an authorized Wayland source"};
                if (baseline && (current.bounds != baseline->bounds || current.monitors != baseline->monitors))
                    return {{}, 0, EPIPE, "The native X11 desktop layout changed during recording"};
                if (!baseline) baseline = std::move(current);
                const auto snapshot = controlState ? controlState->snapshot.load(std::memory_order_acquire) : nullptr;
                if (controlState && !snapshot)
                    return {{}, 0, EAGAIN, "The recording control surface is changing"};
                const bool captured = snapshot ? capture->captureGuarded(region.x, region.y, region.width, region.height,
                    cursor, snapshot->surfaces) : capture->capture(region.x, region.y, region.width, region.height, cursor);
                if (!captured) return {{}, 0, EIO, capture->error()};
                if (controlState && controlState->snapshot.load(std::memory_order_acquire) != snapshot)
                    return {{}, 0, EAGAIN, "The recording control surface changed during readback"};
                return {capture->pixels(), capture->stride(), 0, {}};
            });
        return run(guarded, std::move(writer), std::move(source), events, stop);
    };
}

Backend frameRecordingBackend(std::unique_ptr<Core::FrameSource> source, FrameSourceContract expected)
{
    struct Source {
        std::mutex mutex;
        std::unique_ptr<Core::FrameSource> value;
    };
    auto owned = std::make_shared<Source>(); owned->value = std::move(source);
    return [owned, expected = std::move(expected)](const BackendRequest& request, Platform::FileHandle writer,
        const BackendEvents& events, std::stop_token stop) {
        std::unique_ptr<Core::FrameSource> source;
        { std::lock_guard lock(owned->mutex); source = std::move(owned->value); }
        auto selected = request;
        if (selected.format == Format::Mp4) {
            if (const auto aligned = alignedMp4Region(selected.region, selected.maximumFrameBytes)) selected.region = *aligned;
        }
        // A permission grant is consumed once. Reusing a copied std::function
        // cannot start another native consumer or infer another screen region.
        const auto budget = request.maximumFrameBytes <= 512ull * 1024 * 1024 ?
            static_cast<std::size_t>(request.maximumFrameBytes * 2) : 0;
        auto cropped = makeCroppedFrameSource(std::move(source), selected.region, expected, budget);
        return run(selected, std::move(writer), std::move(cropped), events, stop);
    };
}

} // namespace HuhuCapture::Recording
