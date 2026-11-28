# Native screenshots and source identity

[`DesktopCapture`](../src/services/capture/DesktopCapture.cpp) is the single
asynchronous screenshot service owned by `DesktopApplication`. Selection, editing,
QR, clipboard and archive rules live in their shared application/services layers.
The common `DesktopSnapshot` adapter delegates native readback to one
`ScreenCaptureBackend` implementation per OS.

## Native pixels and screen bindings

Windows reuses GDI and the shared `CaptureGeometry` filter for visible, uncloaked
windows and extended frame bounds. The retained Windows product calls the same
geometry helper. Native capture requires per-monitor-v2 DPI awareness; the Qt
application embeds that manifest and package validation inspects the actual
executable resource. Qt screens are matched to real HMONITOR values through the
fixed Qt public native interface, not friendly display names.

X11 uses the owned XCB/RandR/SHM/XFixes implementation. It reads actual server
formats, preserves opaque BGRA output, falls back to ordinary image replies when
shared memory is unavailable, and rejects an unverified XWayland root. Native
scroll input uses XTest. Capture controls use real surface geometry and shape
checks before and after readback. The fixed FFmpeg xcbgrab reuse and its notices
remain in [the provenance record](../third_party/ffmpeg-xcbgrab/upstream.md).

Each native snapshot owns its image with DPR 1 and verified per-screen logical
geometry, physical bounds, scale, rotation metadata and identity. A mismatch,
removed screen or duplicate native binding fails instead of guessing a global
scale. Already-captured pixels are not rotated a second time. DIB/SHM/reply buffers
are not exposed as persistent UI image storage.

Wayland uses the existing `PortalCapture` and actual authorized local PNG. The
worker fully decodes and validates the owned file before publishing a snapshot.
Portal image coordinates remain source pixels and do not imply a global desktop
origin. Its source file is not deleted by this consumer. A later live recording
requires a separate verified ScreenCast contract and same-grant refresh.

## Admission, cancellation and release

`request()` returns zero when admission is closed or unavailable. Every accepted
token gets exactly one queued terminal result, including cancellation. A second
request cannot reuse an active readback. Native workers never dereference Qt
screen objects: the GUI snapshots bindings before work and verifies them again
before publishing the result.

Cancellation stops continuation and retains the worker until its actual readback
returns. Shutdown waits for worker completion and the actual Portal callback drain;
a cleared pointer or hidden window is not a release receipt. The final destructor
is a join fallback, while normal application exit keeps the event dispatcher alive
through `shutdownFinished()`.

The selection view's native cursor move uses the common `DesktopPointer` adapter.
Tests observe its resulting cursor position through a subsequent native snapshot.
Authorized-image views keep local source coordinates and do not request a global
warp without a genuine platform capability.

## Validation boundary

`screen_capture_tests` runs against the final screenshot service. Native mode
reads a visible painted window, checks changed pixels while retaining the old
snapshot, verifies native cursor movement/restoration, and checks concurrent
admission, cancellation and resource drain. Its offscreen mode verifies refusal
and no invented image. Windows and X11 modes are distinct required CTest entries.

The real X11 capture tests additionally exercise physical rectangles, native
window identities, actual wheel events, cursor compositing, controls and cancelled
server reads. Portal lifecycle/refresh probes use a real owned bus/compositor.
These controlled checks do not replace physical mixed-DPI/rotated monitors,
GNOME/Plasma consent, real SDR/HDR appearance or performance measurements.
