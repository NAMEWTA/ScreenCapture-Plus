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

Preparing a scrolling or recording view retains the confirmed selection, not
a claimed preview lease. When Start actually hides its controls, the view waits
for native unexposure and calls `refreshForCapture` on this owner. The old
producer, preview and queued/seed leases drain; the owner orders pending
Wayland requests and opens a new data connection to the **same** authorized
session/node. `captureReady` admits only an actual replacement first frame with
unchanged format/crop/transform. See [the refresh contract and evidence
boundaries](wayland-capture-refresh.md); a new connection is not a universal
compositor presentation fence.

`claim(sourceId, generation, selection)` runs on the GUI thread and succeeds
once for a current source, a completed successful refresh and a nonempty
in-bounds selection. It transfers the replacement connection's actual native
first-frame lease as well as the queue. This remains necessary for a quiet,
still-streaming compositor that produces no further damage. A consumer prefers a newer queued frame;
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

A nonzero refresh token similarly has one queued `captureReady` terminal.
`cancelRefresh` closes that operation's admission and waits for native/preview
and Portal cleanup before reporting cancellation. A queued successful result
cannot leave a claimable interval after cancellation or shutdown has won.
Refresh does not emit another `prepared`, change the public source identity or
retain the old first-frame slot. An active claimed consumer cannot be refreshed.

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

## Composition and validation boundary

`ScreenCastSource` is the narrow facade over this one authorized source owner.
RecordingController or ScrollController owns it for an active live journey;
DesktopApplication prevents competing capture journeys. Screenshot actions use
`DesktopCapture`, without a second Portal/PipeWire live implementation. The
consumer uses the single `LiveFrameReader` and the source's actual frame queue,
stop token and producer-completion future.

A Portal Screenshot never supplies a recording rectangle. The controller opens
a real permission window, accepts the authorized stream's first frame, and uses
the existing CaptureSession/CaptureWindow to select that actual source. Before
encoding or scrolling, it hides/excludes controls and refreshes the same grant.
The replacement frame must retain the expected source identity, generation,
format, dimensions and color contract. A queued event turn separates the borrowed
refresh-frame signal from transfer to its media worker.

Normal requested stop is distinguished from an unexpected terminal. Paused,
changed or failed sources stop safely instead of reusing a stale frame forever.
The worker stops and waits for the real producer, returns its leases, and signals
consumerFinished for the exact source/generation. The facade waits for real
producer and Portal shutdown before emitting its own stopped signal.

The offscreen source tests check actual unavailable capability, rejected claims,
queued cancellation, refresh admission, reentrant shutdown and owner destruction.
`live_recording_tests` feeds real bounded queues through the production reader and
shared GIF/H264 codecs; it is controlled-source coverage, not Portal authorization.
The real compositor source and refresh probes verify actual frames and source
replacement separately. Physical GNOME/Plasma authorization, display behavior,
long-running capture and audio remain distinct acceptance requirements.
