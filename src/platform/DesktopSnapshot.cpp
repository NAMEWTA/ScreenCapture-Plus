#include "platform/DesktopSnapshot.h"
#include "platform/ScreenCaptureBackend.h"
#include <QColorSpace>

namespace HuhuCapture::Platform {

NativeSnapshot captureNativeDesktop(std::stop_token stop)
{
    NativeSnapshot result;
    if (stop.stop_requested()) { result.failure.code = Capture::Failure::Code::Cancelled; return result; }
    // The source and all its native resources live on this one worker. X11's
    // established connection is interrupted by the same lifetime stop token.
    ScreenCaptureBackend capture(stop);
    if ((result.failure = capture.initialize())) return result;
    if (capture.requiresPortal()) {
        result.failure = {Capture::Failure::Code::Unavailable, 0,
            QStringLiteral("This XWayland connection requires an authorized Portal screenshot")};
        return result;
    }
    auto native = capture.capture(stop);
    if ((result.failure = std::move(native.failure))) return result;
    result.image = std::move(native.image);
    result.image.setColorSpace(QColorSpace::SRgb);
    result.desktop = native.desktop.bounds;
    result.cursor = native.desktop.cursor;
    result.windows = std::move(native.desktop.windows);
    for (const auto& display : native.desktop.displays)
        result.screens.push_back({display.name, display.pixels, display.rotation, display.nativeId});
    return result;
}

} // namespace HuhuCapture::Platform
