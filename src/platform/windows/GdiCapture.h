#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <span>

namespace HuhuCapture::Platform {

// Thread-confined screen capture surface. Reuses its DCs and opaque top-down BGRA DIB
// until the capture dimensions change; borrowed pixels remain valid until then.
class GdiCapture final {
public:
    GdiCapture() = default;
    ~GdiCapture();
    GdiCapture(const GdiCapture&) = delete;
    GdiCapture& operator=(const GdiCapture&) = delete;

    bool capture(int x, int y, int width, int height, bool includeCursor = false);
    void reset() noexcept;
    std::span<const std::uint8_t> pixels() const noexcept { return { pixels_, bytes_ }; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::size_t stride() const noexcept { return stride_; }

private:
    bool ensureSurface(int width, int height);
    void drawCursor(int x, int y) noexcept;

    HDC screen_{};
    HDC memory_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_{};
    std::uint8_t* pixels_{};
    int width_{};
    int height_{};
    std::size_t stride_{};
    std::size_t bytes_{};
};

} // namespace HuhuCapture::Platform
