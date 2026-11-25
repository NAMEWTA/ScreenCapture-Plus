#include "platform/NativeScrollSource.h"
#include "platform/windows/GdiCapture.h"
#include <windows.h>

namespace HuhuCapture::Platform {
struct NativeScrollSource::State {
    int x{}, y{}, width{}, height{};
    GdiCapture capture;
    HWND target{};
    std::string error;
};
NativeScrollSource::NativeScrollSource(int x, int y, int width, int height, std::stop_token) : state_(std::make_unique<State>())
{ state_->x = x; state_->y = y; state_->width = width; state_->height = height; }
NativeScrollSource::~NativeScrollSource() = default;
bool NativeScrollSource::capture()
{
    if (state_->capture.capture(state_->x, state_->y, state_->width, state_->height)) return true;
    state_->error = "Windows could not capture the scrolling region"; return false;
}
ScrollInputResult NativeScrollSource::scroll()
{
    POINT pointer{};
    if (!state_->target) {
        if (!SetCursorPos(state_->x + state_->width / 2, state_->y + state_->height / 2) || !GetCursorPos(&pointer)) {
            state_->error = "Windows could not position the scrolling pointer"; return ScrollInputResult::Failed;
        }
        state_->target = WindowFromPoint(pointer);
        if (!state_->target) { state_->error = "No scrolling target is available"; return ScrollInputResult::Failed; }
    }
    if (!GetCursorPos(&pointer)) { state_->error = "The current pointer position is unavailable"; return ScrollInputResult::Failed; }
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
