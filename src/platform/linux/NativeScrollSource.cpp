#include "platform/NativeScrollSource.h"
#include "platform/linux/X11Capture.h"
#include <optional>
#include <utility>

namespace HuhuCapture::Platform {
struct NativeScrollSource::State {
    int x{}, y{}, width{}, height{};
    Linux::X11Capture capture;
    std::shared_ptr<Core::CaptureControls> controls;
    std::optional<Linux::X11Desktop> desktop;
    std::string error;
    bool ready{};
    explicit State(std::stop_token stop) : capture(stop) {}
    bool initialize() { if (!ready) ready = capture.initialize(); return ready; }
};
NativeScrollSource::NativeScrollSource(int x, int y, int width, int height, std::stop_token stop,
    std::shared_ptr<Core::CaptureControls> controls) : state_(std::make_unique<State>(stop))
{
    state_->x = x; state_->y = y; state_->width = width; state_->height = height;
    state_->controls = std::move(controls);
}
NativeScrollSource::~NativeScrollSource() = default;
bool NativeScrollSource::capture()
{
    state_->error.clear();
    if (!state_->initialize()) return false;
    Linux::X11Desktop current;
    if (!state_->capture.desktop(current, false)) return false;
    if (state_->desktop && (state_->desktop->bounds != current.bounds || state_->desktop->monitors != current.monitors)) {
        state_->error = "The native X11 desktop layout changed during scrolling capture"; return false;
    }
    if (!state_->desktop) state_->desktop = std::move(current);
    const auto snapshot = state_->controls ? state_->controls->snapshot.load(std::memory_order_acquire) : nullptr;
    if (state_->controls && !snapshot) {
        state_->error = "The scrolling control surface is changing"; return false;
    }
    const auto captured = snapshot ? state_->capture.captureGuarded(state_->x, state_->y, state_->width,
        state_->height, false, snapshot->surfaces) :
        state_->capture.capture(state_->x, state_->y, state_->width, state_->height);
    if (!captured) return false;
    if (state_->controls && state_->controls->snapshot.load(std::memory_order_acquire) != snapshot) {
        state_->error = "The scrolling control surface changed during native readback"; return false;
    }
    return true;
}
ScrollInputResult NativeScrollSource::scroll()
{
    if (!state_->initialize()) return ScrollInputResult::Failed;
    if (state_->capture.scrollAtPointer({state_->x, state_->y, state_->width, state_->height}, 1)) return ScrollInputResult::Scrolled;
    return state_->capture.scrollPaused() ? ScrollInputResult::PointerOutsideTarget : ScrollInputResult::Failed;
}
std::span<const std::uint8_t> NativeScrollSource::pixels() const noexcept { return state_->capture.pixels(); }
const std::string& NativeScrollSource::error() const noexcept { return state_->error.empty() ? state_->capture.error() : state_->error; }
} // namespace HuhuCapture::Platform
