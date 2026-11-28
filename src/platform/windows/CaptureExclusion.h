#pragma once

#include <Windows.h>

namespace HuhuCapture::Platform::Windows {

enum class CaptureExclusionStatus { Applied, Unsupported, Failed };
struct CaptureExclusionResult {
    CaptureExclusionStatus status{CaptureExclusionStatus::Unsupported};
    DWORD nativeError{};
};

// Extracted from App::excludeFromCapture. Earlier Windows accepts the 0x11
// flag but renders a black protected rectangle; it does not exclude the window.
// Keep the observed build boundary instead of relying on a successful setter.
inline CaptureExclusionResult excludeFromCapture(HWND window) noexcept
{
    if (!window) return {CaptureExclusionStatus::Failed, ERROR_INVALID_WINDOW_HANDLE};
    static const bool supported = [] {
        OSVERSIONINFOW version{sizeof(version)};
        const auto runtime = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return runtime && runtime(&version) == 0 && version.dwBuildNumber >= 19041;
    }();
    if (!supported) return {};
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE))
        return {CaptureExclusionStatus::Failed, GetLastError()};
    DWORD affinity{};
    if (!GetWindowDisplayAffinity(window, &affinity)) return {CaptureExclusionStatus::Failed, GetLastError()};
    return affinity == WDA_EXCLUDEFROMCAPTURE ? CaptureExclusionResult{CaptureExclusionStatus::Applied, 0} :
        CaptureExclusionResult{CaptureExclusionStatus::Failed, ERROR_NOT_SUPPORTED};
}

} // namespace HuhuCapture::Platform::Windows
