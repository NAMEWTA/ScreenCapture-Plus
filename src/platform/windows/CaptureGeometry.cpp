#include "platform/windows/CaptureGeometry.h"
#include <dwmapi.h>
#include <algorithm>
#include <new>
#include <utility>

namespace HuhuCapture::Platform {
bool captureWindowRectangles(const RECT& desktop, std::vector<RECT>& rectangles, DWORD& error)
{
    struct Enumeration { const RECT* bounds; std::vector<RECT> values; DWORD failure{}; } result{&desktop, {}, 0};
    SetLastError(ERROR_SUCCESS);
    const auto succeeded = EnumWindows([](HWND window, LPARAM context) -> BOOL {
        if (!window || !IsWindowVisible(window)) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        RECT rectangle{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rectangle, sizeof(rectangle)))) return TRUE;
        if (static_cast<long long>(rectangle.right) - rectangle.left <= 6 ||
            static_cast<long long>(rectangle.bottom) - rectangle.top <= 6) return TRUE;
        auto& state = *reinterpret_cast<Enumeration*>(context);
        rectangle.left = (std::max)(rectangle.left, state.bounds->left);
        rectangle.top = (std::max)(rectangle.top, state.bounds->top);
        rectangle.right = (std::min)(rectangle.right, state.bounds->right);
        rectangle.bottom = (std::min)(rectangle.bottom, state.bounds->bottom);
        if (rectangle.left >= rectangle.right || rectangle.top >= rectangle.bottom) return TRUE;
        try { state.values.push_back(rectangle); }
        catch (const std::bad_alloc&) { state.failure = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    if (!succeeded) {
        error = result.failure ? result.failure : GetLastError();
        if (!error) error = ERROR_GEN_FAILURE;
        return false;
    }
    rectangles = std::move(result.values); error = ERROR_SUCCESS; return true;
}
} // namespace HuhuCapture::Platform
