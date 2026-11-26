#include "platform/ScreenCaptureBackend.h"
#include "platform/linux/X11Capture.h"
#include "core/PixelBuffer.h"
#include <xcb/randr.h>
#include <cstring>

namespace HuhuCapture::Platform {
namespace {
using Failure = Capture::Failure;
Failure cancelled() { return {Failure::Code::Cancelled, 0, QStringLiteral("Screen capture was cancelled")}; }
Failure failed(const Linux::X11Capture& capture)
{ return {Failure::Code::Platform, 0, QString::fromStdString(capture.error())}; }
QRect rectangle(const Linux::X11Rectangle& value)
{ return {value.x, value.y, value.width, value.height}; }
Core::FrameTransform rotation(unsigned value)
{
    // RandR rotates counter-clockwise. Compose X/Y reflection into the same
    // vertical-first convention as the existing frame contract; root pixels
    // are already transformed and are never rotated again by the UI.
    unsigned turns = value & XCB_RANDR_ROTATION_ROTATE_90 ? 1 :
        value & XCB_RANDR_ROTATION_ROTATE_180 ? 2 : value & XCB_RANDR_ROTATION_ROTATE_270 ? 3 : 0;
    const bool horizontal = value & XCB_RANDR_ROTATION_REFLECT_X;
    const bool vertical = value & XCB_RANDR_ROTATION_REFLECT_Y;
    const bool reflected = horizontal != vertical;
    // RandR applies reflection after rotation (RRTransformCompute). The frame
    // contract reflects first, so a single reflection reverses the turn sign.
    if (reflected) turns = (4 - turns) % 4;
    if (vertical) turns = (turns + 2) % 4;
    constexpr Core::FrameTransform plain[]{Core::FrameTransform::None, Core::FrameTransform::Rotate90,
        Core::FrameTransform::Rotate180, Core::FrameTransform::Rotate270};
    constexpr Core::FrameTransform reflect[]{Core::FrameTransform::Reflected, Core::FrameTransform::Reflected90,
        Core::FrameTransform::Reflected180, Core::FrameTransform::Reflected270};
    return reflected ? reflect[turns] : plain[turns];
}
CaptureDesktop converted(const Linux::X11Desktop& native)
{
    CaptureDesktop value; value.bounds = rectangle(native.bounds); value.cursor = {native.cursorX, native.cursorY};
    value.displays.reserve(static_cast<qsizetype>(native.monitors.size()));
    for (const auto& screen : native.monitors) value.displays.push_back({
        screen.latin1Name ? QString::fromLatin1(screen.name.data(), static_cast<qsizetype>(screen.name.size())) : QString::fromStdString(screen.name),
        screen.identity, rectangle(screen.pixels), rotation(screen.rotation)});
    value.windows.reserve(static_cast<qsizetype>(native.windows.size()));
    for (const auto& window : native.windows) {
        const auto clipped = rectangle(window).intersected(value.bounds);
        if (!clipped.isEmpty()) value.windows.push_back(clipped);
    }
    return value;
}
}
struct ScreenCaptureBackend::State {
    Linux::X11Capture capture;
    explicit State(std::stop_token lifetime) : capture(lifetime) {}
};
ScreenCaptureBackend::ScreenCaptureBackend(std::stop_token lifetime) : state_(std::make_unique<State>(lifetime)) {}
ScreenCaptureBackend::~ScreenCaptureBackend() = default;
Capture::Failure ScreenCaptureBackend::initialize()
{ return state_->capture.initialize() ? Failure{} : failed(state_->capture); }
bool ScreenCaptureBackend::requiresPortal() const noexcept { return state_->capture.isXWayland(); }
bool ScreenCaptureBackend::supportsAutomaticScroll() const noexcept { return state_->capture.supportsAutomaticScroll(); }
NativeScreenshot ScreenCaptureBackend::capture(std::stop_token stop)
{
    NativeScreenshot result;
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    Linux::X11Desktop before;
    if (!state_->capture.desktop(before)) { result.failure = failed(state_->capture); return result; }
    result.desktop = converted(before);
    const auto& bounds = result.desktop.bounds;
    const auto layout = Core::pixelLayout(bounds.width(), bounds.height(), 4);
    if (!layout || layout->bytes > 512ull * 1024 * 1024) {
        result.failure = {Failure::Code::Allocation, 0, QStringLiteral("The captured desktop exceeds the image memory budget")}; return result;
    }
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    if (!state_->capture.capture(bounds.x(), bounds.y(), bounds.width(), bounds.height())) {
        result.failure = failed(state_->capture); return result;
    }
    Linux::X11Desktop after;
    if (!state_->capture.desktop(after, false)) { result.failure = failed(state_->capture); return result; }
    if (before.bounds != after.bounds || before.monitors != after.monitors) {
        result.failure = {Failure::Code::SourceChanged, 0, QStringLiteral("The X11 desktop changed while pixels were captured")}; return result;
    }
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    QImage image(bounds.size(), QImage::Format_ARGB32);
    if (image.isNull()) { result.failure = {Failure::Code::Allocation, 0, QStringLiteral("The owned screenshot could not be allocated")}; return result; }
    const auto pixels = state_->capture.pixels();
    for (int row = 0; row < image.height(); ++row)
        std::memcpy(image.scanLine(row), pixels.data() + static_cast<std::size_t>(row) * layout->stride, layout->stride);
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    result.image = std::move(image); return result;
}
Capture::Failure ScreenCaptureBackend::moveCursor(const CaptureDesktop& expected, QPoint target,
    QPoint& actual, std::stop_token stop)
{
    if (stop.stop_requested()) return cancelled();
    Linux::X11Desktop desktop;
    if (!state_->capture.desktop(desktop, false)) return failed(state_->capture);
    const auto current = converted(desktop);
    if (!expected.sameLayout(current)) return {Failure::Code::SourceChanged, 0, QStringLiteral("The captured X11 desktop is no longer current")};
    if (!current.bounds.contains(target)) return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("The cursor target is outside the captured desktop")};
    if (stop.stop_requested()) return cancelled();
    int x{}, y{};
    if (!state_->capture.moveCursor(target.x(), target.y(), x, y)) return failed(state_->capture);
    actual = {x, y}; return {};
}
} // namespace HuhuCapture::Platform
