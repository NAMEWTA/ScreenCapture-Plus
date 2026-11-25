#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/ScreenCaptureBackend.h"
#include "platform/windows/CaptureGeometry.h"
#include "platform/windows/GdiCapture.h"
#include "core/PixelBuffer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace HuhuCapture::Platform {
namespace {
using Failure = Capture::Failure;
Failure nativeFailure(const char* detail, DWORD native = GetLastError())
{ return {native == ERROR_NOT_ENOUGH_MEMORY ? Failure::Code::Allocation : Failure::Code::Platform,
    native, QString::fromLatin1(detail)}; }
Failure cancelled() { return {Failure::Code::Cancelled, 0, QStringLiteral("Screen capture was cancelled")}; }
QRect rectangle(const RECT& value)
{ return {value.left, value.top, value.right - value.left, value.bottom - value.top}; }
Failure desktop(CaptureDesktop& output, bool windows)
{
    struct Enumeration { CaptureDesktop value; DWORD error{}; } state;
    SetLastError(ERROR_SUCCESS);
    const auto success = EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
        auto& state = *reinterpret_cast<Enumeration*>(context);
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) { state.error = GetLastError(); return FALSE; }
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0)) {
            state.error = GetLastError(); if (!state.error) state.error = ERROR_NOT_SUPPORTED; return FALSE;
        }
        try {
            CaptureDisplay display;
            display.name = QString::fromWCharArray(info.szDevice);
            display.nativeId = reinterpret_cast<quintptr>(monitor);
            display.pixels = rectangle(info.rcMonitor);
            // Windows DMDO is counter-clockwise, matching FrameTransform.
            switch (mode.dmDisplayOrientation) {
            case DMDO_DEFAULT: break;
            case DMDO_90: display.rotation = Core::FrameTransform::Rotate90; break;
            case DMDO_180: display.rotation = Core::FrameTransform::Rotate180; break;
            case DMDO_270: display.rotation = Core::FrameTransform::Rotate270; break;
            default: state.error = ERROR_NOT_SUPPORTED; return FALSE;
            }
            state.value.bounds = state.value.bounds.united(display.pixels);
            state.value.displays.push_back(std::move(display));
        } catch (const std::bad_alloc&) { state.error = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    if (!success || state.value.displays.isEmpty())
        return nativeFailure("Read the actual Windows monitor configuration", state.error ? state.error : GetLastError());
    std::sort(state.value.displays.begin(), state.value.displays.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    const QRect virtualDesktop(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (virtualDesktop != state.value.bounds)
        return {Failure::Code::SourceChanged, 0, QStringLiteral("The Windows monitor configuration changed during enumeration")};
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return nativeFailure("Read the actual desktop cursor");
    state.value.cursor = {cursor.x, cursor.y};
    if (windows) {
        const RECT bounds{virtualDesktop.x(), virtualDesktop.y(),
            virtualDesktop.x() + virtualDesktop.width(), virtualDesktop.y() + virtualDesktop.height()};
        std::vector<RECT> native;
        DWORD error{};
        if (!captureWindowRectangles(bounds, native, error)) return nativeFailure("Enumerate visible desktop windows", error);
        state.value.windows.reserve(static_cast<qsizetype>(native.size()));
        for (const auto& window : native) state.value.windows.push_back(rectangle(window));
    }
    output = std::move(state.value); return {};
}
}
struct ScreenCaptureBackend::State { GdiCapture capture; };
ScreenCaptureBackend::ScreenCaptureBackend() : state_(std::make_unique<State>()) {}
ScreenCaptureBackend::~ScreenCaptureBackend() = default;
Capture::Failure ScreenCaptureBackend::initialize()
{
    // The Qt executable is per-monitor-aware. Verify this worker inherited the
    // same physical-coordinate contract instead of silently capturing scaled UI.
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return {Failure::Code::Unavailable, 0, QStringLiteral("Physical desktop capture requires per-monitor-v2 DPI awareness")};
    CaptureDesktop value; return desktop(value, false);
}
bool ScreenCaptureBackend::requiresPortal() const noexcept { return false; }
bool ScreenCaptureBackend::supportsAutomaticScroll() const noexcept { return true; }
NativeScreenshot ScreenCaptureBackend::capture(std::stop_token stop)
{
    NativeScreenshot result;
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    if ((result.failure = desktop(result.desktop, true))) return result;
    const auto& bounds = result.desktop.bounds;
    const auto layout = Core::pixelLayout(bounds.width(), bounds.height(), 4);
    if (!layout || layout->bytes > 512ull * 1024 * 1024) {
        result.failure = {Failure::Code::Allocation, 0, QStringLiteral("The captured desktop exceeds the image memory budget")}; return result;
    }
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    if (!state_->capture.capture(bounds.x(), bounds.y(), bounds.width(), bounds.height())) {
        result.failure = nativeFailure("Capture the complete Windows desktop with GDI"); return result;
    }
    CaptureDesktop after;
    if ((result.failure = desktop(after, false))) return result;
    if (!after.sameLayout(result.desktop)) {
        result.failure = {Failure::Code::SourceChanged, 0, QStringLiteral("The native desktop changed while pixels were captured")}; return result;
    }
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    QImage image(bounds.size(), QImage::Format_ARGB32);
    if (image.isNull()) { result.failure = {Failure::Code::Allocation, 0, QStringLiteral("The owned screenshot could not be allocated")}; return result; }
    const auto pixels = state_->capture.pixels();
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(image.scanLine(y), pixels.data() + static_cast<std::size_t>(y) * layout->stride, layout->stride);
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    result.image = std::move(image); return result;
}
Capture::Failure ScreenCaptureBackend::moveCursor(const CaptureDesktop& expected, QPoint target,
    QPoint& actual, std::stop_token stop)
{
    if (stop.stop_requested()) return cancelled();
    CaptureDesktop current;
    if (auto failure = desktop(current, false)) return failure;
    if (!expected.sameLayout(current)) return {Failure::Code::SourceChanged, 0, QStringLiteral("The captured desktop is no longer current")};
    if (!current.bounds.contains(target)) return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("The requested cursor is outside the captured desktop")};
    if (stop.stop_requested()) return cancelled();
    if (!SetCursorPos(target.x(), target.y())) return nativeFailure("Move the actual desktop cursor");
    POINT value{}; if (!GetCursorPos(&value)) return nativeFailure("Read the moved desktop cursor");
    actual = {value.x, value.y}; return {};
}
} // namespace HuhuCapture::Platform
