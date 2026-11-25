#include "platform/NativeScrollSource.h"
#include "platform/linux/X11Capture.h"

namespace HuhuCapture::Platform {
struct NativeScrollSource::State {
    int x{}, y{}, width{}, height{};
    Linux::X11Capture capture;
};
NativeScrollSource::NativeScrollSource(int x, int y, int width, int height) : state_(std::make_unique<State>())
{ state_->x = x; state_->y = y; state_->width = width; state_->height = height; }
NativeScrollSource::~NativeScrollSource() = default;
bool NativeScrollSource::capture() { return state_->capture.capture(state_->x, state_->y, state_->width, state_->height); }
ScrollInputResult NativeScrollSource::scroll()
{
    if (state_->capture.scroll(state_->x + state_->width / 2, state_->y + state_->height / 2, 1)) return ScrollInputResult::Scrolled;
    return state_->capture.scrollPaused() ? ScrollInputResult::PointerOutsideTarget : ScrollInputResult::Failed;
}
std::span<const std::uint8_t> NativeScrollSource::pixels() const noexcept { return state_->capture.pixels(); }
const std::string& NativeScrollSource::error() const noexcept { return state_->capture.error(); }
} // namespace HuhuCapture::Platform
