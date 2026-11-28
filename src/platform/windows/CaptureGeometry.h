#pragma once

#include <windows.h>
#include <vector>

namespace HuhuCapture::Platform {

// The shipping CutMask visibility/cloaking/frame-bound filter, shared with Qt.
// Coordinates remain physical desktop pixels; allocation never unwinds through
// an EnumWindows callback. Failure leaves the caller's prior result unchanged.
bool captureWindowRectangles(const RECT& desktop, std::vector<RECT>& rectangles,
    DWORD& error);

} // namespace HuhuCapture::Platform
