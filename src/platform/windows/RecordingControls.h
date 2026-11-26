#pragma once

#include "core/CaptureControls.h"
#include <Windows.h>

namespace HuhuCapture::Platform::Windows {

inline HRESULT verifyRecordingControls(const Core::CaptureControlSnapshot& controls,
    const RECT* selected = nullptr) noexcept
{
    if (controls.surfaces.empty() || controls.surfaces.size() > 64) return E_INVALIDARG;
    for (const auto& surface : controls.surfaces) {
        const auto window = reinterpret_cast<HWND>(surface.nativeId);
        DWORD process{};
        if (!window || !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId())
            return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        if (!IsWindowVisible(window)) continue;
        if (surface.placement == Core::CaptureControlPlacement::Outside && selected) {
            RECT actual{};
            if (selected->right <= selected->left || selected->bottom <= selected->top ||
                !GetWindowRect(window, &actual) || actual.right <= actual.left || actual.bottom <= actual.top)
                return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
            if (actual.left < selected->right && actual.top < selected->bottom &&
                actual.right > selected->left && actual.bottom > selected->top)
                return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            continue;
        }
        if (surface.placement != Core::CaptureControlPlacement::Excluded)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        DWORD affinity{};
        if (!GetWindowDisplayAffinity(window, &affinity)) return HRESULT_FROM_WIN32(GetLastError());
        // The shared desktop adapter already checks the supported Windows
        // build before publishing Excluded; a successful setter on old Windows
        // must never be interpreted as the modern exclusion capability.
        if (affinity != WDA_EXCLUDEFROMCAPTURE) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    return S_OK;
}

} // namespace HuhuCapture::Platform::Windows
