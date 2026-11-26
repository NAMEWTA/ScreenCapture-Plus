#pragma once

#include "capture/CaptureTypes.h"

namespace HuhuCapture::UI::Qt {
class EditorWindow;

// Place a newly opened native-desktop selection at its source position, using
// only still-valid monitor bindings. The editor refits once on that monitor.
// Authorized Portal source pixels have no desktop placement and return false.
// Failure leaves the compositor/host's existing window placement untouched.
bool placeCapturedEditor(EditorWindow* editor, const Capture::ActionRequest& request);

} // namespace HuhuCapture::UI::Qt
