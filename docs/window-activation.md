# Window activation

The application uses the single platform entry
[`applyActivationToken`](../src/platform/WindowActivation.h). It attaches an
existing compositor-issued token to an already-created Qt Wayland top-level
surface. The caller then uses Qt's `QWindow::requestActivate()`. Attaching a
token or submitting that request does not prove that the OS granted focus.

The Linux adapter is compiled against exactly Qt 6.12.1. Its reviewed private
Wayland interface sets the token on Qt's existing shell surface, which consumes
it when requesting activation. It does not create a second protocol manager,
connection, event loop, token or input serial. It rejects invalid UTF-16/NUL
input, a missing or non-focusable top-level surface, a wrong calling thread,
and an unavailable `xdg_activation_v1` global. It does not persist the token or
change the process-wide `XDG_ACTIVATION_TOKEN` environment.

An empty token and a real non-Wayland backend return `NotNeeded`. The Windows
adapter follows the same no-token path; Qt and the OS retain normal foreground
restrictions. Callers show usable windows even when a token cannot be attached.
They never describe an ignored or expired token as successful focus acquisition.

`qt_capture_placement` checks that empty/non-Wayland token handling cannot create
a native surface. `window_activation_unavailable` also drives the actual capture
presentation through synchronous show cancellation and owner destruction. The
distinct `window_activation_native` case requires Windows or X11, checks the
actual product's capture focus, cancellation, repeated activation and destruction,
and corroborates Qt focus with the real Windows foreground window or an
independent XCB input-focus query. Its Windows
input fixture first establishes foreground ownership with a checked native click
within its own window; it does not change the OS foreground policy.

Actual granted Wayland activation still requires a real compositor-issued token
and its desktop journey; the offscreen and X11 cases make no claim about it.
Capture presentation checks its owner and surface after each synchronous native
show/focus call, so a callback that closes the surface cannot leave a dangling
pointer or report successful presentation. The application and recording controller retain their
existing visible-window fallback and pass each external token only to the
appropriate current window.
