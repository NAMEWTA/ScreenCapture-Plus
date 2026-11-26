#pragma once

#include "core/CaptureControls.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>

namespace HuhuCapture::Platform {

enum class ScrollInputResult { Scrolled, PointerOutsideTarget, Failed };

// Worker-owned native rectangle and automatic wheel adapter. Its capture bytes
// have GdiCapture's borrowed-buffer contract; no widget or application state is
// accessed. User movement to a different target pauses automatic wheel input.
class NativeScrollSource final {
public:
    NativeScrollSource(int x, int y, int width, int height, std::stop_token stop = {},
        std::shared_ptr<Core::CaptureControls> controls = {});
    ~NativeScrollSource();
    NativeScrollSource(const NativeScrollSource&) = delete;
    NativeScrollSource& operator=(const NativeScrollSource&) = delete;
    bool capture();
    ScrollInputResult scroll();
    std::span<const std::uint8_t> pixels() const noexcept;
    const std::string& error() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Platform
