#include "platform/NativeScrollSource.h"
#include "platform/windows/GdiCapture.h"
#include "platform/windows/RecordingControls.h"
#include <limits>
#include <utility>
#include <windows.h>

namespace HuhuCapture::Platform {
struct NativeScrollSource::State {
    int x{}, y{}, width{}, height{};
    GdiCapture capture;
    HWND target{};
    std::stop_token stop;
    std::shared_ptr<Core::CaptureControls> controls;
    std::string error;
};
NativeScrollSource::NativeScrollSource(int x, int y, int width, int height, std::stop_token stop,
    std::shared_ptr<Core::CaptureControls> controls) : state_(std::make_unique<State>())
{ state_->x = x; state_->y = y; state_->width = width; state_->height = height; state_->stop = stop; state_->controls = std::move(controls); }
NativeScrollSource::~NativeScrollSource() = default;
bool NativeScrollSource::capture()
{
    if (state_->stop.stop_requested()) { state_->error = "Scrolling capture was cancelled"; return false; }
    const auto right = std::int64_t{state_->x} + state_->width;
    const auto bottom = std::int64_t{state_->y} + state_->height;
    if (state_->width <= 0 || state_->height <= 0 || right > (std::numeric_limits<LONG>::max)() ||
        bottom > (std::numeric_limits<LONG>::max)()) {
        state_->error = "The scrolling rectangle is outside native pixel bounds"; return false;
    }
    const RECT selected{state_->x, state_->y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    const auto snapshot = state_->controls ? state_->controls->snapshot.load(std::memory_order_acquire) : nullptr;
    if (state_->controls && (!snapshot || FAILED(Windows::verifyRecordingControls(*snapshot, &selected)))) {
        state_->error = "The scrolling controls are not protected before native readback"; return false;
    }
    if (!state_->capture.capture(state_->x, state_->y, state_->width, state_->height)) {
        state_->error = "Windows could not capture the scrolling region"; return false;
    }
    if (state_->controls && (state_->controls->snapshot.load(std::memory_order_acquire) != snapshot ||
        FAILED(Windows::verifyRecordingControls(*snapshot, &selected)))) {
        state_->error = "The scrolling controls changed during native readback"; return false;
    }
    return true;
}
ScrollInputResult NativeScrollSource::scroll()
{
    POINT pointer{};
    if (state_->stop.stop_requested()) { state_->error = "Scrolling capture was cancelled"; return ScrollInputResult::Failed; }
    if (!GetCursorPos(&pointer)) { state_->error = "The current pointer position is unavailable"; return ScrollInputResult::Failed; }
    if (!state_->target) {
        // Preserve CapLong's actual clicked target; do not warp to another
        // nested scrolling panel in the middle of a user's selected rectangle.
        if (pointer.x < state_->x || pointer.y < state_->y ||
            std::int64_t(pointer.x) >= std::int64_t(state_->x) + state_->width ||
            std::int64_t(pointer.y) >= std::int64_t(state_->y) + state_->height) return ScrollInputResult::PointerOutsideTarget;
        state_->target = WindowFromPoint(pointer);
        if (!state_->target) { state_->error = "No scrolling target is available"; return ScrollInputResult::Failed; }
    }
    if (!IsWindow(state_->target)) { state_->error = "The scrolling target was closed"; return ScrollInputResult::Failed; }
    if (WindowFromPoint(pointer) != state_->target) return ScrollInputResult::PointerOutsideTarget;
    INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    if (SendInput(1, &input, sizeof(input)) != 1) { state_->error = "Windows rejected the scrolling input"; return ScrollInputResult::Failed; }
    return ScrollInputResult::Scrolled;
}
std::span<const std::uint8_t> NativeScrollSource::pixels() const noexcept { return state_->capture.pixels(); }
const std::string& NativeScrollSource::error() const noexcept { return state_->error; }
} // namespace HuhuCapture::Platform
