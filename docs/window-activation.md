# Window activation

`Desktop::WindowActivation` is the shared application-owned entry for focusing
capture, settings, history and other actual `QWindow` instances. It queues one
request at a time, keeps only a weak window reference, observes Qt's actual
window/focus notifications and uses a three-second deadline. A superseding
request or a destroyed window settles the old token as cancelled. Callers must
show their window and keep QApplication's dispatcher alive until completion.

The result separates `requested` from `active`: sending a native request does
not prove that the operating system gave the application focus. Success requires
an actual exposed target matching `QGuiApplication::focusWindow()` on the next
Qt dispatcher turn, immediately before result delivery. `QWindow::isActive()`
also includes transient ancestors and alone cannot prove focus on the requested
window. Windows additionally checks `GetForegroundWindow` and the GUI thread's
actual `GetFocus` target (or its child). A rejected foreground request does not
become success merely because a thread-local focus notification arrived.
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
backend, queued cancellation, target destruction and a window hidden during its
focus notification. The target must first be exposed and have painted, as in
the existing native screen-capture fixture. Merely returning from `show()` does
not establish that precondition. The disappearing-window case also waits for
exposure before requesting focus, and must not deliver stale success after the
focus observer hides it.

On Windows the positive test then receives a real `SendInput` click in a
separate, visible, owned input window before asking the production service to
activate the target. It reuses the guarded `ClientToScreen`, `SetCursorPos`,
`WindowFromPoint`, mouse-down/up sequence in
`tests/windows/MediaHistoryChecks.h`. The small input fixture alone is topmost
so a runner console is not accidentally clicked; the activation target remains
an ordinary window. Actual spontaneous Qt mouse events and native foreground
focus must corroborate the click, and the target must not already own focus.
No input-queue attachment, foreground-lock change or forced Qt activation policy
is used. The original cursor position is restored on exit.

This positive case proves activation while the user is already interacting with
the application. It cannot promise unattended focus over a different application
or override Windows' foreground restrictions. A refused request still fails the
same exact Qt/native focus assertion. Diagnostics record exposure, Qt events,
the foreground window's class, process image, and `GetGUIThreadInfo` for both
GUI threads, so an external foreground takeover can be distinguished from an
unexposed fixture or a transient thread-local focus notification. API inspection
failures are recorded explicitly; inaccessible processes do not receive an
invented identity.

Offscreen and minimal modes only check explicit refusal, not positive desktop
behavior. A Linux SDK compile or this negative test is not evidence that Windows
input or an externally supplied Wayland activation token worked; those require
their distinct real desktop/compositor runs.

The fix was checked against fixed Qt 6.12.1 commit
`5af1a37e31a869cd6eb8a73d82e2c71a450e6634`: `qwindowswindow.cpp`
(`ac396ee00df52f74a59fe6298a5a7d0fa86ac813`) calls `SetForegroundWindow`
then `SetFocus`; `qguiapplication.cpp`
(`fec2e580d808fe46a6715aee1e5e217fb8dbcef0`) delivers actual focus changes;
`qwindow.cpp` (`84699bcf882937a261b38460254e0023e1cf562e`)
defines the broader transient-chain semantics of `isActive`. No Qt code is
copied. Microsoft documents the distinction between
[foreground ownership](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getforegroundwindow),
[thread-local focus](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setfocus),
and an OS-permitted
[foreground request](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow).
The test observes native
[GUI thread state](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getguithreadinfo)
and checks the actual delivery of
[injected input](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput),
whose insertion alone does not prove that the intended window received it.
