#pragma once

#include <Windows.h>

namespace HuhuCapture::Platform::Windows {

// SetFocus is thread-local; it is not evidence that Windows granted the
// foreground. Called on the GUI thread for an already-created native window.
inline bool hasForegroundFocus(HWND window) noexcept
{
    if (!window || !IsWindowVisible(window)) return false;
    const auto foreground = GetForegroundWindow();
    const auto focus = GetFocus();
    return foreground && foreground == GetAncestor(window, GA_ROOT) && focus &&
        (focus == window || IsChild(window, focus));
}

} // namespace HuhuCapture::Platform::Windows
