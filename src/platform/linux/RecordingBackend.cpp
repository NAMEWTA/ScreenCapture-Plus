#include "recording/PlatformRecording.h"
#include "recording/GifRecording.h"
#include "recording/SoftwareMp4Recording.h"
#include "platform/linux/X11Capture.h"
#include "platform/linux/pipewire/PipeWireAudio.h"

#include <cerrno>
#include <memory>
#include <mutex>

namespace HuhuCapture::Recording {
namespace {
BackendResult run(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> source, const BackendEvents& events, std::stop_token stop)
{
    if (request.format == Format::Gif) return runGifRecording(request, std::move(writer), std::move(source), events, stop);
    return runSoftwareMp4Recording(request, std::move(writer), std::move(source),
        Platform::Linux::makePipeWireAudio(request.systemAudio, request.microphone), events, stop);
}
}

ControlsExclusion excludeControlsFromCapture(std::uintptr_t)
{ return ControlsExclusion::Unsupported; }

Backend desktopRecordingBackend(DesktopRecordingControls controls)
{
    return [controls](const BackendRequest& request, Platform::FileHandle writer, const BackendEvents& events, std::stop_token stop) {
        // Construct and destroy the X connection on the media worker. The GUI
        // routes Wayland/XWayland selections through a fresh ScreenCast grant.
        auto capture = std::make_shared<Platform::Linux::X11Capture>(stop);
        auto source = makePollingFrameSource(request, CaptureSource::X11,
            [capture, controls, checked = false](PixelRegion region, bool cursor) mutable -> BorrowedBgraFrame {
                if (!checked && controls.expectedOutside && controls.nativeWindow) {
                    Platform::Linux::X11Rectangle actual;
                    if (!capture->windowRectangle(controls.nativeWindow, actual))
                        return {{}, 0, EIO, capture->error()};
                    if (std::int64_t{actual.x} < std::int64_t{region.x} + region.width &&
                        std::int64_t{actual.y} < std::int64_t{region.y} + region.height &&
                        std::int64_t{actual.x} + actual.width > region.x &&
                        std::int64_t{actual.y} + actual.height > region.y)
                        return {{}, 0, EAGAIN, "The window manager placed the recording controls inside the selected area; move them outside and start again"};
                }
                checked = true;
                if (!capture->capture(region.x, region.y, region.width, region.height, cursor))
                    return {{}, 0, EIO, capture->error()};
                return {capture->pixels(), capture->stride(), 0, {}};
            });
        return run(request, std::move(writer), std::move(source), events, stop);
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
        // A permission grant is consumed once. Reusing a copied std::function
        // cannot start another native consumer or infer another screen region.
        const auto budget = request.maximumFrameBytes <= 512ull * 1024 * 1024 ?
            static_cast<std::size_t>(request.maximumFrameBytes * 2) : 0;
        auto cropped = makeCroppedFrameSource(std::move(source), request.region, expected, budget);
        return run(request, std::move(writer), std::move(cropped), events, stop);
    };
}

} // namespace HuhuCapture::Recording
