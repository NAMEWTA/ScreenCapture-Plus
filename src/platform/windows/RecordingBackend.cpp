#include "recording/PlatformRecording.h"
#include "recording/GifRecording.h"
#include "platform/windows/GdiCapture.h"
#include "platform/windows/NativeMp4Recording.h"

#include <memory>

namespace HuhuCapture::Recording {
namespace {
class PhysicalPixels final {
public:
    PhysicalPixels() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
    explicit operator bool() const noexcept { return previous_ != nullptr; }
private:
    DPI_AWARENESS_CONTEXT previous_;
};
}

ControlsExclusion excludeControlsFromCapture(std::uintptr_t nativeWindow)
{
    // Preserve App::excludeFromCapture's real-version guard. Older DWM accepts
    // the numeric value but paints a black rectangle instead of excluding it.
    static const bool supported = [] {
        OSVERSIONINFOW version{sizeof(version)};
        const auto query = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return query && query(&version) == 0 && version.dwBuildNumber >= 19041;
    }();
    if (!supported) return ControlsExclusion::Unsupported;
    const auto window = reinterpret_cast<HWND>(nativeWindow);
    DWORD actual{};
    if (!window || !SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) ||
        !GetWindowDisplayAffinity(window, &actual) || actual != WDA_EXCLUDEFROMCAPTURE)
        return ControlsExclusion::Failed;
    return ControlsExclusion::Excluded;
}

Backend desktopRecordingBackend(DesktopRecordingControls controls)
{
    return [controls](const BackendRequest& request, Platform::FileHandle writer,
        const BackendEvents& events, std::stop_token stop) {
        PhysicalPixels physical;
        if (!physical) {
            BackendResult result;
            result.media.fail(Core::RecordingStage::Prepare, GetLastError());
            result.detail = "Cannot establish physical desktop coordinates for recording";
            std::error_code error;
            if (!writer.close(error)) result.media.finish(false, error.value());
            return result;
        }
        if (controls.expectedOutside && controls.nativeWindow) {
            const auto window = reinterpret_cast<HWND>(controls.nativeWindow);
            RECT actual{};
            if (!GetWindowRect(window, &actual) ||
                (IsWindowVisible(window) && actual.left < std::int64_t{request.region.x} + request.region.width &&
                 actual.top < std::int64_t{request.region.y} + request.region.height &&
                 actual.right > request.region.x && actual.bottom > request.region.y)) {
                BackendResult result; result.media.fail(Core::RecordingStage::Prepare, ERROR_INVALID_WINDOW_HANDLE);
                result.detail = "The recording controls are not outside the selected area; move them outside and start again";
                std::error_code error;
                if (!writer.close(error)) result.media.finish(false, error.value());
                return result;
            }
        }
        if (request.format == Format::Mp4)
            return Platform::Windows::runNativeMp4(request, std::move(writer), events, stop);
        auto capture = std::make_shared<Platform::GdiCapture>();
        auto source = makePollingFrameSource(request, CaptureSource::Gdi,
            [capture](PixelRegion region, bool cursor) -> BorrowedBgraFrame {
                const auto left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if (region.x < left || region.y < top ||
                    std::int64_t{region.x} + region.width > std::int64_t{left} + width ||
                    std::int64_t{region.y} + region.height > std::int64_t{top} + height)
                    return {{}, 0, ERROR_INVALID_PARAMETER, "The selected desktop region is no longer available"};
                if (!capture->capture(region.x, region.y, region.width, region.height, cursor))
                    return {{}, 0, ERROR_GEN_FAILURE, "Windows could not capture the selected GIF frame"};
                return {capture->pixels(), capture->stride(), 0, {}};
            });
        return runGifRecording(request, std::move(writer), std::move(source), events, stop);
    };
}

} // namespace HuhuCapture::Recording
