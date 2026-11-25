#include "platform/DesktopSnapshot.h"
#include "platform/windows/GdiCapture.h"
#include "core/PixelBuffer.h"

#include <windows.h>
#include <dwmapi.h>
#include <QColorSpace>
#include <algorithm>

namespace HuhuCapture::Platform {
namespace {
QVector<NativeScreen> monitors()
{
    QVector<NativeScreen> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return TRUE;
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        Core::FrameTransform rotation = Core::FrameTransform::None;
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            if (mode.dmDisplayOrientation == DMDO_90) rotation = Core::FrameTransform::Rotate90;
            else if (mode.dmDisplayOrientation == DMDO_180) rotation = Core::FrameTransform::Rotate180;
            else if (mode.dmDisplayOrientation == DMDO_270) rotation = Core::FrameTransform::Rotate270;
        }
        auto& list = *reinterpret_cast<QVector<NativeScreen>*>(parameter);
        list.push_back({QString::fromWCharArray(info.szDevice),
            QRect(info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                info.rcMonitor.bottom - info.rcMonitor.top), rotation});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    return result;
}
}
NativeSnapshot captureNativeDesktop(std::stop_token stop)
{
    NativeSnapshot result;
    if (stop.stop_requested()) { result.failure.code = Capture::Failure::Code::Cancelled; return result; }
    result.screens = monitors();
    if (result.screens.isEmpty()) {
        result.failure = {Capture::Failure::Code::Platform, GetLastError(), QStringLiteral("No Windows display is available")}; return result;
    }
    for (const auto& monitor : result.screens) result.desktop = result.desktop.united(monitor.pixels);
    const auto layout = Core::pixelLayout(result.desktop.width(), result.desktop.height(), 4);
    if (!layout || layout->bytes > 512ull * 1024 * 1024) {
        result.failure = {Capture::Failure::Code::Allocation, 0, QStringLiteral("The virtual desktop exceeds the screenshot pixel budget")}; return result;
    }
    GdiCapture capture;
    if (!capture.capture(result.desktop.x(), result.desktop.y(), result.desktop.width(), result.desktop.height())) {
        result.failure = {Capture::Failure::Code::Platform, GetLastError(), QStringLiteral("Windows could not capture the desktop")}; return result;
    }
    result.image = QImage(capture.pixels().data(), capture.width(), capture.height(),
        static_cast<qsizetype>(capture.stride()), QImage::Format_ARGB32).copy();
    if (result.image.isNull()) {
        result.failure = {Capture::Failure::Code::Allocation, 0, QStringLiteral("The captured image could not be retained")}; return result;
    }
    if (result.screens != monitors()) {
        result.image = {}; result.failure = {Capture::Failure::Code::SourceChanged, 0,
            QStringLiteral("The display layout changed during capture")}; return result;
    }
    POINT pointer{};
    if (GetCursorPos(&pointer)) result.cursor = {pointer.x, pointer.y};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(window) || IsIconic(window)) return TRUE;
        BOOL cloaked{};
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        RECT rectangle{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rectangle, sizeof(rectangle))) ||
            rectangle.right - rectangle.left <= 6 || rectangle.bottom - rectangle.top <= 6) return TRUE;
        auto& snapshot = *reinterpret_cast<NativeSnapshot*>(parameter);
        snapshot.windows.push_back(QRect(rectangle.left, rectangle.top,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    if (stop.stop_requested()) { result.image = {}; result.failure.code = Capture::Failure::Code::Cancelled; return result; }
    result.image.setColorSpace(QColorSpace::SRgb);
    return result;
}
} // namespace HuhuCapture::Platform
