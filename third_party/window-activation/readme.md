# Qt Wayland activation bridge

The small application adapter in
[`WindowActivation.cpp`](../../src/platform/linux/WindowActivation.cpp) calls
Qt's private activation-token setter. Qt's public native window interface can
request a new token, but cannot attach the token supplied by a GlobalShortcuts
Portal activation. No Qt implementation is copied or patched here.

The exact Qt 6.12.1 source and blob identities reviewed for this seam are recorded
in [`upstream.json`](upstream.json). The adapter is compiled against that exact
version and requires a new source review when Qt changes. Qt's existing XDG shell
stores the supplied token only when the shell role and activation protocol exist;
the next activation request consumes it and clears Qt's stored copy. Calling the
setter before a shell role exists would discard the token, so the adapter checks
the actual visible native XDG toplevel and advertised protocol first.

The caller never supplies a guessed input serial, fabricates a token, changes the
process activation environment, or logs the opaque token. Successful attachment
is not evidence that a compositor accepted focus. Live compositor activation and
Portal hotkey acceptance remain separate desktop validation gates.
