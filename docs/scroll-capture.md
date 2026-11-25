# Shared scrolling capture

## Preserved matching and timing

`capture/ScrollCaptureModel` extracts the shipping `CapLong` state and calls the
existing `core/ScrollStitch` matchers and incremental append. Both the retained
Windows `CapLong` and the new shared session use this production model. It owns
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
height limit, so the shared session can present the actual reason.

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

The independent [PR #7 implementation](https://github.com/NAMEWTA/huhucapture/pull/7)
was reviewed at commit `251bc0071278c6b711c840bf5188d16870bc7349` without modifying
its branch. Its complete `ScrollController`, `ScrollAccumulator`,
`NativeScrollSource`, `ScreenCastSource` and X11 sources informed the shared
session/view and application composition. Exact reviewed Git blobs and SHA-256
digests are recorded in
[`scroll-capture-upstream.json`](../third_party/project-reuse/scroll-capture-upstream.json).
Those project contributions are MIT, Copyright (c) 2026 NAMEWTA. Existing FFmpeg
XCB readback attribution and LGPL-2.1-or-later obligations remain unchanged.

`NativeScrollSource` keeps the same thin GDI/XCB seam. It does not implement
another screenshot engine. Windows reuses `GdiCapture` and actual `SendInput`;
Linux reuses the single `X11Capture` and its actual XTest support. The source
service exposes that queried support as `automaticScroll`, so an X11 server
without XTest is not advertised as automatic scrolling. XWayland is rejected.

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
vector capacities against a default 512 MiB working budget. Explicit bounded
reserve prevents implicit vector doubling from exceeding that budget. Native
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

## Shared session, controls and output

`ScrollCaptureSession` confines capture, normalization and matching to one
worker. Native capture uses the original timing table above. Manual Wayland
capture stores the same authorized selection during setup, without claiming a
preview lease. Start hides the real controls, waits for native unexposure and
requests the shared source refresh; only `captureReady` can transfer one
`LiveSourceConsumer` and its replacement first-frame lease. The worker uses the
same `FrameNormalizer` as recording. It does not
authorize another stream or reinterpret a Screenshot rectangle. Source identity
changes stop sampling and retain the last complete result.

Preview preparation uses the existing bounded tail sampler. One latest progress
value is delivered at most every 100 ms; there is no per-frame queue of full
images. Output creates a read-only QImage over the completed model allocation
with a shared lifetime keeper. This preserves its full resolution, alpha mode
and color-space metadata without copying the accumulated image. The worker has
already ended before those immutable pixels are exposed.

`ScrollWindow` uses the same 44-DIP controls and 24-DIP icons as the editor.
Native mode retains the 30-DIP, 16-pixel-text start circle inside the selected
region and a pointer-transparent outline outside captured pixels. The manual
authorized-source mode uses a normal Start button, since it has no global
desktop mapping. Status text is 12 logical pixels; the preview and wrapped
instructions can scroll in a constrained work area without hiding the output
controls. Pin/close/save/copy order and keyboard actions remain shared.

Requesting output while sampling first stops and waits for the worker. The
complete image stays available after canceled or failed output. The separate
`Application::ScrollWorkspace` subscribes to the existing capture action flow,
uses its `nativeSource()` / `liveSource()` owners, and sends actual images to
the same `MediaWorkspace` output/editor pipeline. Closing the view cancels only
its own pending output and scrolling session. Closing while a refresh is pending
cancels that token and waits for its actual native/Portal terminal. A live
consumer admitted after refresh but rejected before worker submission releases
its lease immediately; a started consumer drains the producer queue
and returns before `consumerFinished`. The live owner still separately awaits
the actual producer and Portal cleanup.

Before each native readback the controls acknowledge actual hidden/exposed
state or a real Windows capture-exclusion result, within a bounded wait. An old
start surface must be unexposed before its interior mask changes to an outside
outline. Wayland manual capture hides the complete controls and requires a
currently reachable tray, actually registered global stop action, or verified
installed desktop action. The host
uses the same `setExternalStopAvailable` / `canStop` / `stopActive` contract as
recording; losing the last entry requests stop and retains any complete image.
The view returns only after the consumer or canceled refresh has ended. The
former instruction to move controls outside the source is not an admission
check and is no longer presented as a solution.

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

The new model and those tests actually compiled with g++ 14, C++20,
`-O2 -Wall -Wextra -Wpedantic -Werror`, and all six groups passed. Existing
`scroll_stitch_tests.cpp` remains the separate matcher/threshold characterization
suite. The new native X11 adapter, Qt session, UI and workspace also actually
compiled with fixed Qt 6.12.1 and strict GCC 14 warnings. The actual offscreen
component test runs rejected-source admission, disabled empty-image shortcuts,
logical-pixel typography, asynchronous close ordering and immediate deletion
from the close callback. It produces no fake successful capture.

The positive mode of `tests/qt/scroll_capture_tests.cpp` paints a deterministic
page in a real native QWidget. Its start click is a component input event;
all subsequent movement must come from actual Win32 SendInput or XTest wheel
events, including a checked spontaneous flag. The probe compares every stitched
pixel against the independently painted source page, checks retained output
after a canceled component output request, and saves the real controls/result.
This positive probe is compiled but has **not run in this local workspace**.
Windows/X11 native execution, compositor freshness, actual manual Wayland
scrolling, full output dialogs, mixed DPI and performance remain separate gates.

Wayland live frames must originate from one ScreenCast authorization and its
PipeWire remote. A previous Screenshot rectangle does not prove the identity or
pixel mapping of that stream. The first actual stream frame must be confirmed
through the existing shared selection UI. Manual scrolling uses that same live
source; automatic input is offered only with real RemoteDesktop/libei permission
and capability. These are implementation requirements, not completed validation.
