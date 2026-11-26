# Shared scrolling capture

## Preserved matching and timing

`capture/ScrollCaptureModel` extracts the shipping `CapLong` state and calls the
existing `core/ScrollStitch` matchers and incremental append. Both the retained
Windows `CapLong` and the application-owned `ScrollController` use this production
model through the thin `ScrollAccumulator` facade. It owns
pixels and decisions; it does not own a timer, window, native input or output
service. There is no second matching algorithm in the Qt UI.

The original rules were read from `CapLong.cpp`, `ToolLong.cpp` and `WinCap.cpp`:

| Rule | Preserved value or behavior |
| --- | --- |
| Matching strip | 100 rows, bounded by the remaining changed region |
| Changed-region start | First RGB difference between the first and incoming frame |
| Primary/fallback matcher | Top strip, then existing bottom-strip confidence rule |
| First automatic wheel | 88 ms after the first complete frame |
| Wheel settling | 250 ms |
| Changed frame without a match | Up to two further 250 ms observations before another wheel |
| Ordinary retry/next wheel | 500 ms |
| Stationary/readback retries | Eight retries; the ninth dismissal stops |
| Manual live source | A quiet frame does not count as an automatic wheel dismissal |
| Accepted progress | Resets both dismissal and settling counters |
| Height stopping | Stop after a complete accepted append makes height greater than 36,000 |
| Output cancellation/failure | Keep the last complete image for retry |

The height rule deliberately retains the entire final accepted append. It does
not cut an image at exactly 36,000 rows or discard the final frame. The model
separately reports page-bottom, capture failure, explicit stop, memory limit and
height limit, so the controller can present the actual reason.

The retained toolbar order is pin, close, save and copy. `Ctrl+S` saves,
`Ctrl+C`/Enter copy, and Escape closes. An output can be requested only after a
complete first frame. Canceling Save As or failing an output must not destroy
the accumulated result. Native auto-scrolling must preserve the original
target-window guard: moving the cursor away pauses input instead of scrolling
a different application.

Manual sources may remain still while the user reads. They use the same exact
matching and append rules, with only the automatic reached-bottom counter
disabled. Waiting for manual movement is not evidence that a page ended.

## Existing project reuse and native boundary

The project reuses the existing scrolling control/output flow, native adapters
and cancellation guard from PR #7, together with PR #6's extracted shipping
model, checked SHAPE/control surfaces and original start/toolbar presentation.
Exact reviewed commits, Git blobs and file digests are recorded in
[`scroll-capture-upstream.json`](../third_party/project-reuse/scroll-capture-upstream.json).
Those project contributions are MIT, Copyright (c) 2026 NAMEWTA. Existing FFmpeg
XCB readback attribution and LGPL-2.1-or-later obligations remain unchanged.

`NativeScrollSource` keeps the thin GDI/XCB seam. Windows reuses `GdiCapture`
and actual `SendInput`; Linux reuses the single `X11Capture` and actual XTest.
Missing native input support returns a real error. Wayland does not use an
XWayland desktop as a substitute for authorization.

The initial target is the user's actual pointer inside the selected rectangle.
There is no initial center warp that could choose a different nested scrolling
panel. Moving to another target pauses injection. X11 binds its root child
window; Windows retains `WindowFromPoint` and verifies that target still exists.
The current adapters do not claim arbitrary native child-element discovery.

The reviewed X11 cancellation and signal guard are reused in the existing
backend, retaining its newer RandR 1.5 bindings and cursor conversion. A stop
callback can interrupt established I/O by shutting down only that worker's
owned socket; it is unregistered before disconnection. The guard blocks SIGPIPE
only on that worker, preserves earlier pending signals, and restores its mask.
The initial `xcb_connect` authentication/setup remains outside that interruptible
socket boundary; this is an explicit cancellation limitation, not a completed
blocked-server stress test.

## Memory and ownership

The model owns one contiguous, geometrically growing BGRA result, one previous
BGRA frame and two grayscale frames. It preflights and accounts for the actual
vector capacities against a default 512 MiB working budget. The initial working
set requires `width × height × (4 + 4 + 1 + 1)` bytes before any append. The
facade and pure model use this same budget; a limit below that complete initial
set is rejected. Explicit bounded reserve prevents implicit vector doubling
from exceeding the budget. Native
readback buffers and the separately bounded preview remain separate owners.
This budget is not a measured total process memory figure.

An append writes only the incoming overlapping tail using `appendScrollFrame`.
It does not copy the accumulated image on every sample. The previous frame is
allocated at initialization, so updating it after a successful append cannot
allocate and invalidate that completed append. Memory refusal or a failed
capture stops with the last complete image still available. Stopping releases
the matcher buffers while retaining the result.

The preview continues to use `makeImagePreview`, which samples only the visible
tail of the bottom-aligned thumbnail. A very narrow long image therefore does
not allocate a many-million-row intermediate thumbnail. Image export and editor
handoff consume the original full-resolution result, not that preview.

The retained Win32 output declarations are now const-correct: `WinPin` already
read the vector through a const pointer, and `ImageExport`/`MediaHistory` already
passed read-only spans to the native encoder/clipboard helpers. This permits the
shared immutable result to reach those same mature paths without an extra full
long-image copy or a cast that pretends callers may mutate its bytes.

## One controller, controls and output

[`ScrollController`](../src/app/ScrollController.cpp) owns the sole Qt flow and
its internal QWidget controls. One worker performs capture, normalization and
matching. Native capture uses the timing table above. Manual Wayland capture
keeps the authorized selection contract without claiming a consumer during the
preview. Start hides the controls, waits for their actual native unexposure and
calls `ScreenCastSource::refresh()` for the same authorization. Only the real
`refreshed` first frame can pass the source/session, format, crop, transform and
normalized-size checks. The controller freezes the new generation, then queues
`takeFrameSource()` after that borrowed signal has returned.

The worker uses the shared `FrameNormalizer` and `LiveFrameReader`. It neither
authorizes another stream nor reinterprets a Screenshot rectangle. Every frame
must retain the selected source contract. `Idle` means the live source is still
Streaming; a real `Paused`, changed source or unsolicited terminal ends sampling
with the last complete image retained. Normal stop requests drain the actual
producer through `FrameSource::stopAndWait()` before returning the consumer;
Portal cleanup remains owned by the shared live source.

Preview preparation uses `makeImagePreview` to sample a bounded bottom-aligned
tail. `ScrollProgressMailbox` retains one latest preview and posts at most one
pending GUI callback; a blocked GUI coalesces intermediate progress instead of
accumulating image-bearing events. Stop, worker completion and destruction close
that slot and release its pixels. Progress contains the preview and dimensions,
never the complete long image. At completion `ScrollCaptureModel::takePixels()` moves the accumulated
allocation into a read-only QImage with a lifetime keeper. No whole-image copy
is needed for that handoff, and the worker has already stopped mutating it.
The result remains the original full-resolution opaque BGRA pixels; this path
does not claim additional color-space metadata propagation.

The controller's QWidget uses the shared 44-DIP controls and 24-DIP icons,
ordered pin, close, save and copy. `Ctrl+S` saves, `Ctrl+C` and both Enter keys
copy, and Escape closes. Its grip asks the native QWindow to begin a system
move. Status text is 12 logical pixels; preview and instructions can scroll in
a constrained work area while action strips wrap complete click targets.
Native mode retains the 30-DIP-radius, 16-pixel-text start circle over the
selected region and a pointer-transparent outline outside captured pixels.
The authorized-image mode uses a normal Start button because it has no global
desktop mapping.

Requesting output during sampling first stops and waits for the worker. The
last complete image then goes to the existing `MediaWorkspace` output/editor
pipeline. A canceled Save As or failed output keeps that image and controller
available for retry. Closing cancels only this controller's exact pending output
and source. Unstarted readers can be abandoned without blocking the GUI; a
started reader waits for its native producer and clears queued leases before
returning. The shared source separately finishes Portal teardown before the
application's shutdown barrier completes.

Native readback validates the same immutable control snapshot before and after
every frame. Windows checks actual capture-exclusion/physical rectangles; X11
checks actual viewability, coordinates and SHAPE inside its short server grab.
There is no per-frame GUI acknowledgement or fixed hide delay used as proof.
An interior start surface must first be unexposed before changing into the
outside outline. On Windows without proven exclusion that fullscreen outline
remains hidden; Linux checks its actual shape rather than trusting a rectangle.

When neither exclusion nor a verified outside placement is possible, controls
may hide only while a real external finish action is available. Manual Wayland
always follows that hide/refresh path. The host supplies
`setExternalStopAvailable(bool, hint)` and calls `finish()`; loss of the last
entry requests stop and retains any complete image. Activation does not reveal
capture-hidden controls. They return only after the native worker or canceled
refresh has ended.

The [shared refresh contract](wayland-capture-refresh.md) drains old preview
leases and data-plane resources before taking a replacement first frame.
Native unexposure, `wl_display.sync` and opening a new FD do **not** independently
prove compositor presentation. Its real pixel probe must first capture visible
controls and then inspect the exact first replacement lease. `receivedAt`, an
arbitrary delay, or discarding a fixed number of frames cannot establish that
guarantee. GNOME/Plasma and physical-desktop evidence remain a P08/P09 gate.

When no real completion action exists, Start remains disabled with an
explicit reason; the application must not hide the only completion control.
The [GNOME desktop action](desktop-capture-action.md) checks the effective GIO
registration and an actual child-process/primary-IPC round trip. The shared
controls describe that action instead of referring to an unavailable tray or
shortcut. One-shot instances and unregistered AppImages cannot claim it.
Real GNOME menu reachability, shell UI capture, actual stop and complete
stitching remain acceptance gates; saving a command string is insufficient.

## Actual validation and remaining gates

`tests/scroll_capture_model_tests.cpp` generates one deterministic BGRA page with
a fixed header and samples viewports from it. The stitched output is compared
against the actual contiguous source page, rather than against another copy of
the matching implementation. The test also exercises stationary retries,
animation rechecks, progress resetting retries, the original height threshold,
bounded capacity growth and retained output after memory refusal.

`tests/scroll_accumulator_tests.cpp` checks the same model through the controller
facade: exact initial working bytes, refusal one byte below that bound, retained
pixels after a rejected append, the complete append crossing the height limit,
and ownership transfer without copying the accumulated allocation. These pure
model/facade tests have run locally with C++20 and strict GCC warnings.
`qt_scroll_progress` runs real worker threads while GUI dispatch is blocked,
counts actual QImage cleanup and QObject MetaCall delivery, then checks the
single latest slot, one pending callback, stop and owner destruction. It does
not replace the native readback or stitching tests.

`scroll_capture_tests --unavailable` uses the final `ScrollController` and real
`MediaWorkspace`. It checks invalid/unbound-source admission, no invented busy
state or output, and synchronous owner deletion from the shutdown completion.
It does not substitute a fake successful native source.

The positive mode of `tests/qt/scroll_capture_tests.cpp` paints a deterministic
page in a real native QWidget and obtains the initial snapshot from the actual
`DesktopCapture`. The start click is a component input event; subsequent page
movement must come from actual SendInput/XTest wheel events, including a checked
spontaneous flag. It checks 44/24 controls and order, stops the actual worker,
cancels a real shared QFileDialog, retries Save As, then compares every archived
and exported pixel against the independent source page. Screenshots and bounded
readback diagnostics are described in [scroll-test-evidence.md](scroll-test-evidence.md).

The integrated sources and probe have compiled with the fixed Qt 6.12.1 SDK.
This merged native positive probe has not run in the local workspace; an earlier
implementation's native result cannot validate the changed controller. Windows
and X11 execution, compositor freshness, actual manual Wayland scrolling,
physical mixed DPI and performance remain separate gates. Component file-dialog
interaction does not prove a platform-native file picker or external clipboard
consumer.

Wayland live frames must originate from one ScreenCast authorization and its
PipeWire remote. A previous Screenshot rectangle does not prove the identity or
pixel mapping of that stream. The first actual stream frame must be confirmed
through the existing shared selection UI. Manual scrolling uses that same live
source; automatic input is offered only with real RemoteDesktop/libei permission
and capability. These are implementation requirements, not completed validation.
