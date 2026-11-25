#pragma once

#include <QString>

class QWindow;

namespace HuhuCapture::Platform {

enum class ActivationTokenResult {
    NotNeeded, Attached, WindowUnavailable, Unsupported, InvalidToken,
};

// GUI-thread call after showing the native toplevel, immediately before
// QWindow::requestActivate(). The caller supplies an actual Portal/compositor
// token and discards its own copy after use. Attached means Qt accepted the
// token for the next activation request; the compositor still decides focus.
// Empty tokens and platforms without Wayland activation are harmless no-ops.
// No token is generated, logged, stored globally or substituted with a serial.
ActivationTokenResult applyActivationToken(QWindow* window, const QString& token);

} // namespace HuhuCapture::Platform
