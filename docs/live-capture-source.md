# One authorized live source

`Capture::LiveCaptureSource` owns one Portal authorization and one PipeWire
producer for the shared scrolling and recording workflows. The native adapter,
its remote FD and the Portal session stay under this owner. Consumers receive
CPU frame leases, a bounded queue, observed native activity and a stop source;
they do not acquire another Portal session or expose a native buffer to Qt.

## Selection and admission

An earlier Screenshot result does not establish the coordinates of a new
ScreenCast source. The first actual stream frame is normalized once on a worker
and delivered as an `AuthorizedSourcePixels` snapshot. The existing
`CaptureSession`/`CaptureWindow` must select that actual image. It has no invented
desktop origin, monitor bindings or global window positions.

`claim(sourceId, generation, selection)` runs on the GUI thread and succeeds
once for a current source and a nonempty in-bounds selection. It transfers the
original native first-frame lease as well as the queue. This is necessary for
a quiet, still-streaming compositor that produces no additional damage after
the user sees the selection preview. A consumer prefers a newer queued frame;
when the queue is open and empty it can use `firstFrame`. The owner releases
its own reference to that slot at claim. `claimedAt` records the time of this
admission without rewriting the frame's original source timestamp.

The queue holds at most two frames and the capture pool has at least four
slots: two queued frames, one consumer-held frame and one producer write slot.
Slow consumers discard old queued leases through `FrameQueue`, with exact
statistics. No per-frame GUI signal or unbounded image list is used. The
consumer applies the confirmed selection through the shared `FrameNormalizer`.

Source format, format generation, native crop or orientation changes invalidate
the current coordinate contract and close the queue. They require a new
authorization/selection flow. A static Streaming source is different from a
native Paused source: actual PipeWire activity is observable, and the native
adapter applies its bounded pause-recovery deadline. Time without damage alone
does not imply source loss.

## Completion and lifetime

Every nonzero request token receives one queued `prepared` result. Requests
after shutdown return zero and cannot add callbacks behind `shutdownFinished`.
`stop` preserves queued complete frames for drain. `cancel` abandons queued
work and still waits for the native producer to return. The first actual queue
error is never replaced by a later cancellation.

The consumer calls `consumerFinished` on the GUI thread only after its worker
has returned and released all source leases. `stopped` waits for this handshake,
the native producer, the preview worker, and Portal callback cleanup. Normal
shutdown is asynchronous. Abrupt QObject destruction requests stop and joins
the private workers before destroying the Portal owner; it is not the normal
application exit path. Signal observers may request shutdown or destroy the
owner without subsequent widget or state dispatch.

The current live adapter requires a native Linux desktop, an actual visible
parent, a supported screen/window source and Hidden cursor capability. Missing
Portal or source capabilities are explicit failures. Native Windows and X11
desktop sources continue to use their existing adapters. This service does
not claim RemoteDesktop input injection, arbitrary global Wayland coordinates
or a working ScreenCast source on offscreen/minimal QPA.

## Actual validation and remaining gates

`Application::CaptureWorkspace` owns the application's single live source beside
its native `ScreenCaptureService`. Recording and scrolling borrow these owners
through `liveSource()` / `nativeSource()`, and source-currentness checks use the
same globally unique source identities. No consumer workspace creates its own
Portal controller. A direct Wayland recording/scrolling entry authorizes the
stream first; a live action chosen from an ordinary Screenshot starts a separate
authorization. Both use a real modeless parent window and wait for its actual
`QWindow::isExposed()` event before requesting ScreenCast. The wait is bounded;
there is no invented parent handle or GUI-thread round trip.

After the real first frame is prepared, the same `CaptureSession` and
`CaptureWindow` replace the previous source and require selection in that new
image. The old Screenshot rectangle is never transferred. Canceling permission
before replacement restores an existing screenshot selection. A repeated entry
activates the existing parent, picker or live-action controls. If a source ends,
its already-owned image remains usable for image actions; another live action
must authorize/select again. Normal producer completion has no failure and is
not translated into a source-invalidated error. Actual source failures retain
their typed error and notify the consumer.

Capture workspace shutdown waits for both source owners' real drain signals,
in addition to its QR/text and clipboard operations. The application must stop
and await recording/scrolling consumers while the GUI dispatcher is alive, so
each worker can deliver its `consumerFinished` handshake. The existing real
`capture_workspace_tests --unavailable` was rebuilt and run locally with this
composition; it passed unavailable-desktop propagation, no invented pixels or
history, and ordered shutdown. Positive authorization/reselection remains a
native-compositor test, not an offscreen result.

The fixed Qt 6.12.1 / libportal / PipeWire SDK compiles the owner, MOC output,
shared normalizer and real source probe with strict GCC warnings. The local
offscreen test actually executes unavailable-capability handling, no fabricated
snapshot/claim, queued preparation and terminal ordering, closed admission,
reentrant shutdown, and destruction from completion signals.

`tests/qt/live_capture_source_probe.cpp` is a real compositor probe for the
existing `scripts/test-pipewire-capture.py` harness. It paints a real Qt surface,
requests the actual Portal source, checks source-local first-frame pixels,
single consumer admission, native lease normalization, timestamps, bounded
queue statistics and full producer/consumer/Portal drain. Its resize and daemon
loss modes require the wrapper to change its owned Sway output or stop its own
PipeWire process. The probe saves an actual final frame as PNG and never
substitutes generated frames for received pixels.

At this batch's freeze, that positive compositor probe is compiled but **has
not run locally**: this workspace has no Xvfb/Sway session. Physical Windows,
GNOME and Plasma/Wayland selection, rotation, mixed DPI and pause recovery
remain required environment checks. The offscreen tests do not satisfy them.
