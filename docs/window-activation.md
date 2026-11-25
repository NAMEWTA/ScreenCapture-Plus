# Window activation

`Desktop::WindowActivation` is the shared application-owned entry for focusing
capture, settings, history and other actual `QWindow` instances. It queues one
request at a time, keeps only a weak window reference, observes Qt's actual
window/focus notifications and uses a three-second deadline. A superseding
request or a destroyed window settles the old token as cancelled. Callers must
show their window and keep QApplication's dispatcher alive until completion.

The result separates `requested` from `active`: sending a native request does
not prove that the operating system gave the application focus. Success requires
an actual `QWindow::isActive()` / `QGuiApplication::focusWindow()` observation.
Non-desktop QPA backends such as offscreen and minimal return `Unavailable`.

Windows, X11 and requests without an external token use Qt's normal
`QWindow::requestActivate()`. A supplied Wayland token takes the standard
`xdg_activation_v1.activate` path after the target is exposed. It uses the
compositor's real token, the existing Qt-owned connection/surface and Qt's event
dispatcher. There is no fabricated serial, nested event loop, synchronous
Wayland roundtrip or process-wide `XDG_ACTIVATION_TOKEN` mutation. Tokens are
kept only until dispatch/cancellation and are not logged or persisted.

The narrow protocol manager is reused from fixed KWindowSystem 6.32.0, with
[exact sources, changes and LGPL notices](../third_party/kwayland-activation/upstream.md).
Qt 6.12.1's public native APIs have no per-window token setter. The Linux adapter
therefore uses the same version-matched private surface interface already used
by PortalParent. This is a fixed Qt build dependency, not a promise of binary
compatibility with arbitrary system Qt versions.

A compositor may ignore a token, including one that expired while the user
completed authorization. The visible window remains usable; an activation
failure must not be described as a successful focus operation or silently
replace another user's window state. Application hosts decide how to present
that result without destroying captured pixels or an existing document.

`tests/qt/window_activation_tests.cpp` exercises actual native focus on a desktop
backend, queued cancellation and target destruction. Its offscreen mode only
checks explicit refusal, not positive desktop behavior. The fixed Linux SDK has
actually compiled the manager, generated protocol, service and tests, and run
the offscreen refusal. Windows focus and externally supplied Wayland tokens still
require their distinct real desktop/compositor runs; the existence of this
adapter or a successful compile is not that evidence.
