#include "platform/DesktopSnapshot.h"
#include "platform/linux/X11Capture.h"

#include <QColorSpace>

namespace HuhuCapture::Platform {
namespace {
Capture::Failure failed(const std::string& message)
{ return {Capture::Failure::Code::Platform, 0, QString::fromStdString(message)}; }
}
NativeSnapshot captureNativeDesktop(std::stop_token stop)
{
    NativeSnapshot result;
    if (stop.stop_requested()) { result.failure.code = Capture::Failure::Code::Cancelled; return result; }
    Linux::X11Capture capture(stop);
    Linux::X11Desktop before, after;
    if (!capture.desktop(before) || !capture.capture(before.bounds.x, before.bounds.y,
        before.bounds.width, before.bounds.height)) {
        result.failure = stop.stop_requested() ? Capture::Failure{Capture::Failure::Code::Cancelled, 0, {}} :
            failed(capture.error()); return result;
    }
    result.image = QImage(capture.pixels().data(), capture.width(), capture.height(),
        static_cast<qsizetype>(capture.stride()), QImage::Format_ARGB32).copy();
    if (result.image.isNull()) { result.failure = failed("The X11 snapshot could not be allocated"); return result; }
    if (!capture.desktop(after) || before.bounds != after.bounds || before.monitors != after.monitors) {
        result.image = {}; result.failure = stop.stop_requested() ? Capture::Failure{Capture::Failure::Code::Cancelled, 0, {}} :
            Capture::Failure{Capture::Failure::Code::SourceChanged, 0, QStringLiteral("The X11 display layout changed during capture")}; return result;
    }
    if (stop.stop_requested()) { result.image = {}; result.failure.code = Capture::Failure::Code::Cancelled; return result; }
    result.image.setColorSpace(QColorSpace::SRgb);
    result.desktop = {before.bounds.x, before.bounds.y, before.bounds.width, before.bounds.height};
    result.cursor = {before.cursorX, before.cursorY};
    for (const auto& monitor : before.monitors) {
        Core::FrameTransform rotation = Core::FrameTransform::None;
        if (monitor.rotation & 2u) rotation = Core::FrameTransform::Rotate90;
        else if (monitor.rotation & 4u) rotation = Core::FrameTransform::Rotate180;
        else if (monitor.rotation & 8u) rotation = Core::FrameTransform::Rotate270;
        result.screens.push_back({QString::fromStdString(monitor.name),
            QRect(monitor.pixels.x, monitor.pixels.y, monitor.pixels.width, monitor.pixels.height), rotation});
    }
    for (const auto& rect : before.windows) result.windows.push_back({rect.x, rect.y, rect.width, rect.height});
    return result;
}
} // namespace HuhuCapture::Platform
