#pragma once

#include "services/desktop/DesktopResult.h"

class QWindow;

namespace HuhuCapture::Desktop {

struct CaptureExclusionResult {
    bool applied{};
    Failure failure;
};

// An actual platform capability, called after the window has a native surface.
// Linux has no universal window exclusion request for X11/Portal capture. The
// caller must retain that fact when placing the controls; never mask pixels or
// invent desktop positions to suggest exclusion has succeeded.
CaptureExclusionResult excludeFromCapture(QWindow* window);

} // namespace HuhuCapture::Desktop
