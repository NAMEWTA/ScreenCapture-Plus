#include "recording/PlatformRecording.h"
#include "platform/windows/NativeGifRecording.h"
#include "platform/windows/NativeMp4Recording.h"

#include <windows.h>
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
        auto guarded = request;
        if (!guarded.controls && controls.nativeWindow) {
            guarded.controls = std::make_shared<Core::CaptureControls>();
            auto snapshot = std::make_shared<Core::CaptureControlSnapshot>();
            snapshot->surfaces.push_back({controls.nativeWindow, controls.expectedOutside
                ? Core::CaptureControlPlacement::Outside : Core::CaptureControlPlacement::Excluded});
            guarded.controls->snapshot.store(std::move(snapshot), std::memory_order_release);
        }
        if (guarded.format == Format::Mp4)
            return Platform::Windows::runNativeMp4(guarded, std::move(writer), events, stop);
        return Platform::Windows::runNativeGif(guarded, std::move(writer), events, stop);
    };
}

} // namespace HuhuCapture::Recording
