#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace HuhuCapture::Platform::Linux {

struct X11Rectangle {
    int x{}, y{}, width{}, height{};
    bool operator==(const X11Rectangle&) const = default;
};
struct X11Monitor {
    std::string name;
    std::uint32_t identity{};
    bool latin1Name{};
    X11Rectangle pixels;
    unsigned rotation{};
    bool operator==(const X11Monitor&) const = default;
};
struct X11Desktop {
    X11Rectangle bounds;
    std::vector<X11Monitor> monitors;
    std::vector<X11Rectangle> windows; // EWMH stacking order, front to back
    int cursorX{}, cursorY{};
};

// One worker owns its connection, SHM segment, reply and reusable pixel storage.
// Callers must reject XWayland before constructing this native X11 adapter.
// Returned pixels are owned, tightly packed top-down opaque BGRA, and borrowed
// only until the next capture/reset/destruction. All failures preserve a reason.
class X11Capture final {
public:
    X11Capture();
    ~X11Capture();
    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;
    bool initialize();
    bool isXWayland() const noexcept;
    bool desktop(X11Desktop& result, bool includeWindows = true);
    bool capture(int x, int y, int width, int height, bool includeCursor = false);
    bool moveCursor(int x, int y, int& actualX, int& actualY);
    // Positive steps scroll down; negative steps scroll up.
    bool scroll(int x, int y, int steps);
    void reset() noexcept;
    std::span<const std::uint8_t> pixels() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    std::size_t stride() const noexcept;
    const std::string& error() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Platform::Linux
