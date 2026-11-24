#include "pch.h"
#include "platform/windows/GdiCapture.h"
#include "core/PixelBuffer.h"
#include <limits>

namespace HuhuCapture::Platform {

GdiCapture::~GdiCapture()
{
    reset();
}

void GdiCapture::reset() noexcept
{
    if (memory_ && previous_) SelectObject(memory_, previous_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_) DeleteDC(memory_);
    if (screen_) ReleaseDC(nullptr, screen_);
    screen_ = nullptr;
    memory_ = nullptr;
    bitmap_ = nullptr;
    previous_ = nullptr;
    pixels_ = nullptr;
    width_ = height_ = 0;
    stride_ = bytes_ = 0;
}

bool GdiCapture::ensureSurface(int width, int height)
{
    const auto layout = Core::pixelLayout(width, height, 4);
    if (!layout || layout->stride > (std::numeric_limits<UINT>::max)()) return false;
    if (bitmap_ && width == width_ && height == height_) return true;
    reset();
    screen_ = GetDC(nullptr);
    if (!screen_) return false;
    memory_ = CreateCompatibleDC(screen_);
    if (!memory_) { reset(); return false; }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_ || !bits) { reset(); return false; }
    previous_ = SelectObject(memory_, bitmap_);
    if (!previous_ || previous_ == HGDI_ERROR) {
        previous_ = nullptr;
        reset();
        return false;
    }
    pixels_ = static_cast<std::uint8_t*>(bits);
    width_ = width;
    height_ = height;
    stride_ = layout->stride;
    bytes_ = layout->bytes;
    return true;
}

bool GdiCapture::capture(int x, int y, int width, int height, bool includeCursor)
{
    if (!ensureSurface(width, height)) return false;
    if (!BitBlt(memory_, 0, 0, width, height, screen_, x, y, SRCCOPY)) return false;
    if (includeCursor) drawCursor(x, y);
    // GDI may batch commands. Flush before the CPU or encoder reads DIB memory.
    if (!GdiFlush()) return false;
    return Core::makeOpaque({ pixels_, bytes_ });
}

void GdiCapture::drawCursor(int x, int y) noexcept
{
    CURSORINFO cursor{ sizeof(CURSORINFO) };
    if (!GetCursorInfo(&cursor) || cursor.flags != CURSOR_SHOWING) return;
    ICONINFO icon{};
    if (!GetIconInfo(cursor.hCursor, &icon)) return;
    const auto localX = cursor.ptScreenPos.x - x - static_cast<LONG>(icon.xHotspot);
    const auto localY = cursor.ptScreenPos.y - y - static_cast<LONG>(icon.yHotspot);
    if (localX >= 0 && localX < width_ && localY >= 0 && localY < height_)
        DrawIconEx(memory_, localX, localY, cursor.hCursor, 0, 0, 0, nullptr,
                   DI_NORMAL | DI_DEFAULTSIZE);
    // GetIconInfo creates these bitmaps; GetCursorInfo only borrows the cursor.
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
}

} // namespace HuhuCapture::Platform
