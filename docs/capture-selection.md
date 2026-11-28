# Capture selection and runtime

The shared `capture/SelectionModel.h` extracts the existing `CutMask` behavior
from the fixed Windows baseline. The retained Windows `CutMask` calls it for
every hover, creation, hit test, move and resize; its Direct2D rectangle is a
presentation snapshot. The Qt capture window uses the same model through
`CaptureSession`. This document distinguishes the shared interaction model,
the implemented native/Portal runtime, and the desktop acceptance evidence
still required. An offscreen component test does not establish mixed-DPI
desktop behavior.

## Source pixels and gestures

The model reuses the core point/bounds types and has no Qt, Win32, image or
window ownership. Its extent is the original image's `[0,width] × [0,height]`
boundary. Platform adapters translate native window rectangles to this source
space in front-to-back order. Candidate clipping occurs once; hovering neither
allocates nor invalidates a label when the selected rectangle stays unchanged.

Window interiors use strict comparisons. A pointer exactly on an edge can
therefore select a lower window containing that point. A press without movement
keeps the existing highlight. A manual drag can run in all four directions, and
a one-pixel initial region remains valid; the four-pixel minimum belongs to
adjusting an existing region, as in the baseline.

The selected rectangle's four extended edges divide the complete canvas into
nine hit regions. The inclusive interior and exact border move the whole
selection. The eight outside regions resize their corresponding sides or
corners, immediately snapping that edge on press. This behavior is intentionally
different from editor shape handles and annotation transparent interiors.

Every adjustment derives from its press rectangle and pointer position, avoiding
incremental drift. Moves retain their width and height and stop at the image
boundary. Resizing can cross the opposite edge. The original minimum-size
side convention after crossing is retained, including its small crossing
transition; replacing it with annotation resize rules would change interaction.

## Explicit boundary correction

In the baseline, applying the minimum size near an image edge could produce a
negative left/top or a right/bottom beyond the source. That result was later
passed to image-copy APIs. The shared model fits the same minimum-sized region
back inside the source. Images smaller than four pixels use their available
extent. Creation also clips a pointer dragged outside the canvas while mouse
capture remains active. These corrections preserve valid source pixels rather
than relying on a later rasterizer to accept an invalid crop.

The Windows candidate enumerator now skips a failed
`DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` result instead of inspecting
an uninitialized `RECT`. Window visibility, cloaking, order and the existing
greater-than-six-pixel candidate filter remain in the Windows adapter.

## Selection-model evidence

`CaptureSession` owns one immutable source, one selection and one outstanding
action. Window views use explicit per-screen source rectangles and X/Y scale
values; the logical virtual desktop is never multiplied by one DPR. A normal
authorized-image source has neither a desktop origin nor native window
candidates. It initially selects the complete authorized image so a click can
accept that source, while dragging performs an application crop. Position copy
uses `source(x,y)` unless a verified desktop origin exists.

Native color sampling and the Qt session share `PixelSample.h` for the original
CMYK percentage rounding. HEX, RGB and CMYK copy strings retain the old format.
One queued worker creates an independent selection image before dispatching
the real host action. Preparation and host output have distinct lifetimes;
failed output and cancelled Save As retain the selected source. Request tokens
remain unique when a closed session loads a new source, so cancelled worker
results cannot complete a later selection. An explicit CLI entry takes
precedence over Ctrl-release pinning. Dismissing a QR result closes capture,
matching the old QR dialog journey.

The capture toolbar's existing mark action uses `Action::Edit` to open the same
shared `EditorWindow`/`EditorCanvas` with all nine tools and range annotations.
It passes the actual prepared source crop to the host, without making the user
pin first or creating another editor implementation. `Action::Pin`, Ctrl-release
and the explicit pin CLI entry retain the pinned-window policy. The host accepts
the real editor before completing capture; failed admission retains the source.

`tests/capture_selection_tests.cpp` checks candidate order/clipping, exact edge
hits, unchanged hover, click acceptance, four drag directions, one-pixel regions,
all nine hit regions, immediate snapping, press-based movement, crossed edges
and minimum sizes at every source edge. It has been compiled and executed with
g++ 14, C++20, `-O2 -Wall -Wextra -Wpedantic -Werror` on the local Linux executor.
The retained Windows adapter is validated by its separate native regression build.
`tests/qt/capture_session_tests.cpp` exercises real QImage worker crops/alpha,
output completion/cancellation, CLI/Ctrl behavior, text and source-coordinate
formatting, per-screen conversion unit data and synchronous host deletion. These
Qt tests have now been compiled and executed with the fixed local Qt 6.12.1 SDK
using offscreen QPA; that component run is not a native desktop or physical DPI pass. The pure test additionally covers shared
CMYK rounding and has been executed with the same strict C++20 flags.

The model does not infer a desktop origin, screen identity, display scale or
Portal authorization. The capture service/UI must explicitly carry and verify
those mappings; a source-local coordinate is not a desktop coordinate. Physical
multi-monitor input and mixed-DPI acceptance remain separate required evidence.

## Snapshot ownership and coordinates

`services/capture/DesktopCapture` publishes an owned, immutable CPU image. The
selection UI never retains a view of a reusable capture DIB, XCB SHM segment or
PipeWire buffer. `CaptureSession` validates the snapshot, keeps the shared
selection model, and copies the selected pixels on its worker before publishing
an `ActionRequest`. The copy preserves source alpha and RGB values; screen
display scaling is not part of the export. A completed or cancelled session
releases the full desktop image. A failed or cancelled output action retains
the original selection so that the user can retry or adjust it.

There are two explicit coordinate spaces:

| Source | Position contract | Presentation |
| --- | --- | --- |
| Windows or native X11 desktop | Source pixels plus a verified desktop origin; every screen carries its native identity, native pixel rectangle, Qt logical geometry, work area and separate X/Y pixel-to-DIP scale | One capture surface for each verified screen; native negative coordinates and gaps remain explicit |
| Portal Screenshot or an authorized ScreenCast preview | Pixels within the authorized image or normalized stream frame; no desktop origin or inferred screen binding | One source-image selection window, with coordinates identified as source-local |

The native adapter captures already rotated desktop pixels. Rotation metadata
describes the verified layout; the UI does not rotate those pixels again. The
service compares the native monitor identity and layout with the GUI's
`QScreen` state before publishing. A changed layout invalidates the selection
instead of applying one virtual-desktop scale. QScreen objects remain borrowed
GUI-thread references and are never dereferenced by the capture worker.

The Qt view preserves four-way creation, window snapping, nine-region
adjustment, Ctrl-release pinning, double-click/Enter copying, right-button
clear/close behavior, and the shared pixel sample formats. An explicit capture
entry action takes precedence over Ctrl-release. Ctrl+H/R/K/P publish the
sample's hexadecimal/RGB/CMYK/position text. Ctrl+C/S remain commands for the
scrolling and recording stages, as in the retained Windows baseline; the
selection toolbar still has explicit copy and save buttons.

The magnifier samples 50 by 30 source pixels and displays each sample at five
physical display pixels. Its hint and selection-coordinate text use a
10-DIP font. The toolbar keeps 44-DIP hit areas and 24-DIP icons, is bounded by
the verified work area, and chooses the screen with the largest selected
intersection. Native arrow-key pointer movement goes through
`platform/DesktopPointer`, using physical coordinates on the actual X11 or
Windows connection. It does not round through a global Qt logical coordinate.

`CaptureWindow` does not automatically hide every surface on submission.
Clipboard ownership and pixel-text output can require the still-visible,
focused origin surface on Wayland. The application hides the selection when
opening pinning, scrolling, recording or a dialog, and restores retained
selection after a cancelled output. Widget destruction is deferred until the
active event handler has returned.

## Native and Portal screenshot adapters

The Windows adapter uses the retained `GdiCapture` implementation with explicit
monitor bounds and display orientation. Window snapping uses visible,
uncloaked DWM rectangles. The capture is copied before the reusable DIB can
change; the desktop layout is checked before and after acquisition.

The X11 adapter owns a separate worker connection. It uses a checked MIT-SHM
segment when available and real `GetImage` otherwise, converts supported
TrueColor layouts into opaque BGRA, and reuses its owned storage for repeated
frames. RandR provides monitor identities; EWMH stacking and a checked tree
fallback provide visible window candidates. Optional cursor inclusion uses
the actual XFixes cursor image. Unsupported pixel formats, stale windows,
invalid bounds and failed queries return a failure with no fabricated frame.
`windowRectangle` queries a mapped client's actual root-relative physical
bounds, including valid window-manager frame extents, for native control-window
exclusion.

Screenshot, scrolling and recording workers pass their stop token into this
owned X11 connection. Once `xcb_connect` has returned, cancellation calls
`shutdown(SHUT_RDWR)` on that connection's still-owned socket to interrupt a
blocked reply. The callback neither closes the descriptor nor invokes XCB or
Qt. It stays registered through worker cleanup and is deregistered before
`xcb_disconnect` can close or recycle the descriptor. A stopped capture
instance remains stopped across `reset()` and cannot silently reconnect.
XCB calls temporarily block SIGPIPE on their worker and restore its previous
mask/pending-signal policy; no process-wide signal handler is replaced.

This does not make initial connection setup interruptible: libxcb performs its
authentication/setup handshake before exposing the socket. A server already
stalled during that first `xcb_connect` remains an explicit limitation. The
adapter does not reimplement X11 authentication or close Qt's shared display
connection to conceal that boundary.

On Wayland, including an XWayland process inside a Wayland session,
`DesktopCapture` uses the Screenshot Portal. The request has a real visible
native parent. A returned URI must be a supported local file URI; decoding
goes through the strict PNG inspection path on a worker. The returned image
remains source-local. The Portal owns its file: opening or cancelling a capture
does not remove that file, infer a PipeWire node from its name, or treat the
image as permission for later streaming.

An accepted `DesktopCapture::request()` returns a nonzero token before any
terminal `finished` delivery, including cancellation during admission and an
immediate Portal rejection. Every accepted request has one terminal result;
stale worker results cannot complete a newer request. Busy state includes
capability discovery, Portal requests, actual workers and queued terminal
delivery. `shutdownFinished` is emitted once after all of these have drained.
Cancellation requests stop work; it does not assert that a native worker has
already returned.

## Shared ScreenCast source

`services/capture/ScreenCastSource` is the GUI owner of a new ScreenCast Portal
authorization. It negotiates only source types the Portal advertises, passes
the authorized remote FD and selected stream identity to the existing
PipeWire producer, and owns the Portal session until producer teardown.
Scrolling and recording share this service and the standard-C++
`Core::FrameSource` interface. A Screenshot URI is never reused as a streaming
authorization.

The producer publishes a first owned `FrameLease` for the source-image selector
and a bounded latest-frame mailbox for the consumer. The pool contains four
slots with a 256-MiB total budget; capture dimensions are capped at 16384 per
axis. `FramePixels` applies the validated crop and transform when preparing
the source preview or BGRA output. Selection is in normalized source pixels.
Consumers retain the preview's session/source identity, generation, exact
format, crop and transform, and fail if that contract changes even when the
new frame has the same normalized dimensions.

`stopAndWait()` is a consumer-worker barrier: it requests producer stop and
waits for the actual producer's completion notification, independent of GUI
event delivery. Both recording and scrolling invoke it before final image or
codec finalization. Abandoning an unused reader on the GUI thread only
requests stop; the source owner reports `stopped` after the producer and
Portal resources settle. An exit guard publishes producer completion on
normal return and exceptions, so error handling cannot strand a waiting
consumer. Stopping during capability discovery retains the source owner until
the real discovery callback returns.

## Scrolling capture

`Application::ScrollController` owns the control window, worker and output
intent. Native Windows/X11 scrolling retains one capture adapter for the
session and sends actual wheel input to the selected target. Moving the
pointer onto another native target pauses automatic scrolling. The native
control window is placed outside the selected region when the verified screen
bindings allow it; if it overlaps, the capture worker waits for the GUI's
hide-and-settle acknowledgement before acquisition. Stop wakes this wait, so
shutdown does not depend on a GUI thread joining itself.

Wayland scrolling opens a new ScreenCast authorization and lets the user
select pixels from that stream's own preview. The user scrolls the authorized
source manually. The controls explain that they must remain outside the
selected source region because there is no invented global Wayland window
mapping or synthetic input fallback. Repeating the hotkey recovers the active
controls or source selector; an actual activation token is deferred while
native controls are hidden for an in-progress acquisition.

`ScrollAccumulator` retains the shipping `CapLong` sequence: first-change
detection, 100-row matching strips, the bottom-strip fallback, two settling
rechecks, and more than eight unsuccessful automatic scroll attempts before
reporting the bottom. Initial acquisition waits 88 ms, actual wheel input is
followed by 250 ms of settling, and a successful append uses the baseline
500-ms delay. A still manual stream is not evidence that the page has reached
its bottom. Output is bounded to 512 MiB and 36000 rows; a limit or a later
capture failure retains the last complete image.

Pin, copy and save first stop the worker, then send the owned complete image
through `MediaWorkspace`. A cancelled save or failed output retains the image
and its controls. `shutdownFinished` follows worker/source drainage and
control release, once only. User-facing normal status text is passed through
the shared translator; actual adapter errors remain available as diagnostic
detail.

## Pin placement and activation

`UI::Qt::placeCapturedEditor` maps the selected image's native origin through
one verified screen binding for initial pin placement. It refuses stale
screens, changed work areas, invalid scales and source-local Portal images.
The editor performs its initial fit on the chosen screen and clamps initial
placement to that screen's work area. Subsequent drag, zoom or DPI changes do
not recenter the pin.

`Platform::applyActivationToken` accepts only the real token supplied by the
Portal/compositor and attaches it to a visible native Wayland toplevel before
the caller requests activation. The fixed Qt 6.12.1 private API, source revision
and rationale are recorded in `third_party/window-activation`. An empty token
or a non-Wayland platform is a no-op. No token, serial or global environment
state is fabricated. A successful attachment is not proof that the compositor
granted focus; that requires a real Wayland acceptance session.

## Runtime validation and remaining gates

The following tests are real executable tests, with their environment limits
kept explicit:

| Target or harness | Checks | Required environment |
| --- | --- | --- |
| `capture_selection` | Exact baseline selection gestures, candidate order, crossed edges and valid image bounds | C++20 |
| `scroll_accumulator` | Exact append bytes, unchanged/settling behavior, bottom detection and bounded retained output | C++20 |
| `qt_capture_session` | Real Qt capture widgets, owned crop pixels and alpha, action tokens, retry retention, clipboard-origin visibility, baseline shortcut stages, deferred surface lifetime and toolbar metrics | Qt Widgets; offscreen is supported |
| `qt_capture_placement` | Explicit per-screen mapping, initial editor placement, stale/source-local rejection, repeated-move/zoom preservation and non-Wayland activation no-op | Qt Widgets; offscreen exercises mapping contracts, not physical monitors |
| `qt_editor_components` | Real editor input/rendering, including top-level deactivation cancelling unfinished drawing while retaining completed geometry | Qt Widgets; native IME and physical screens remain separate |
| `qt_capture_lifecycle` | Actual Qt/GLib dispatch and missing Portal service discovery, cancellation during discovery, source-owner deletion and screenshot/scroll shutdown | Private D-Bus session, Linux Portal build, offscreen Qt |
| `x11_capture_tests` through `scripts/test-x11-capture.py` | Painted X11 pixels, actual RandR/window geometry, physical pointer motion, SHM/storage reuse, XFixes cursor, XTEST scrolling, screenshot token admission and worker drain, scroll-to-editor bytes and control recovery | Isolated owned Xvfb or dummy Xorg display; never the user's desktop |
| `x11_capture_cancellation_tests` through `scripts/test-x11-capture.py --paused-server` | Established native connections with real SHM, confirmed blocked worker poll while the owned server is SIGSTOPped, cancellation/destructor drainage before resume, independent connection and recycled-FD safety, concurrent query cancellation and preserved signal policy | Owned Xvfb/dummy Xorg plus Linux `/proc` inspection; no Qt event dispatcher while the server is paused |

The local Linux recovery run compiled and executed the shared selection,
scroll accumulator, Qt capture, placement and editor component tests. The
capture/editor standalone target used C++20 with
`-Wall -Wextra -Wpedantic -Werror`. Native X11 sources and the runtime harness
also passed compilation against the restored SDK. The missing-Portal lifecycle
and isolated native X11 tests are registered in the application test build;
their runtime results must be read from that run's artifacts, not inferred
from compilation.

The local attempt to start an isolated dummy Xorg server was blocked before
the client ran: the executor could not create the server's local Unix
listening socket. No native X11 runtime pass is claimed from that attempt.
The harness records server logs and a result artifact only for an actual
client run, and CI must run it with a working owned display. The established
XCB cancellation probe has been compiled locally, but its paused-server
runtime result requires that owned display. Initial `xcb_connect` handshake
cancellation remains outside this repair and must not be reported as bounded.

These component and isolated-server checks do not replace the required
Windows runtime, GNOME/Plasma Portal permission and focus behavior, real
PipeWire source replacement/teardown, multi-monitor negative origins, mixed
DPI, rotated displays, physical one-pixel keyboard input, real Wayland
clipboard ownership, or live scrolling/pinning acceptance. No release or
complete Qt-migration claim follows from the tests listed here.

Source: [huhucapture baseline CutMask.cpp](https://github.com/NAMEWTA/huhucapture/blob/0efaf45aae1062436da8238814bba17c325fe4d3/src/capture/CutMask.cpp).
Existing project and inherited ScreenCapture attribution remain applicable;
this extraction adds no external runtime dependency.

## Shared selection presentation

`CaptureWindow` contains the single shared QWidget surface used by both systems.
Native desktops create one surface per verified Qt screen, with one common
`CaptureSession`; an authorized image without global mapping uses that same
surface in a normal application window. Pointer events use explicit source
coordinates, and display changes invalidate the native screen binding.

The view preserves mark, scrolling, recording, QR, history, close, save and copy
order, 44-DIP click targets and 24-DIP icon sizing. Ordinary mark dispatches Edit;
Ctrl-release/explicit Pin retain pinning semantics. History uses Qt's shared
file-list icon because the old Windows Segoe glyph is absent from the project
font. The toolbar wraps into complete rows inside the actual available width.
Unavailable actions stay disabled with the host's reason.

Enter/double-click copy an adjusted selection; Escape/right-click cancel.
Ctrl+H/R/K/P sample the same source and use the actual output service. Arrows move
one source pixel. Ctrl+C/S keep the baseline capture-stage rules. A successful
handoff closes only after the host acknowledges it; cancelled Save As or failed
output retains the selection and original pixels. Reopening the presentation
binds a new source generation and retires the old surfaces.

`capture_window_tests` exercises the final QWidget path, actual Qt pointer and
key dispatch, nine-tool editor handoff, narrow-toolbar geometry, retained failed
output and synchronous owner destruction. Native source acquisition is checked
separately by `screen_capture_tests` and the full-host capture journey. Offscreen
rendering and widget events do not establish physical DPI, IME, native pointer
capture, Portal authorization or real compositor focus.
