# Wayland capture refresh: ownership and evidence

This is one shared control/data handoff for P08 scrolling and P09 recording.
It retains the original ScreenCast authorization; it is not a new capture
implementation or a second Portal owner. It is also **not a universal Wayland
presentation guarantee**. The actual compositor pixel test and the limits
below are part of the contract.

## Why the preview cannot become the first recording frame

The original selected frame can contain the permission parent, selection view
or controls. A static compositor may keep that frame in the queue indefinitely.
Arrival time is not capture time, source PTS is not implicitly the application's
steady epoch, and dropping a fixed count or waiting a fixed delay cannot establish
when the compositor painted hidden surfaces.

Both workspaces now keep only the selected coordinates during setup. Actual
Start requires reachable stop controls, hides the relevant native windows and
tooltips, and observes Qt unexposure within a bounded deadline. The Wayland
refresh then performs the following operations, in order:

1. Close claim admission and stop the old unclaimed preview producer. Await the
   actual producer and preview-worker returns; discard their queue and seed
   leases before allocating the replacement data connection.
2. Submit `wl_display.sync` on Qt's own native display without a blocking
   round trip or another dispatcher. Its asynchronous callback only confirms
   prior requests were handled. Cancellation destroys the proxy and queued
   terminal observers never run on a native listener's stack.
3. Use the existing patched libportal asynchronous OpenPipeWireRemote API on
   the same active session. A late/canceled/closed-session FD is closed, and
   an accepted FD has close-on-exec. The active Portal session remains owned
   by the original controller; no new permission request occurs.
4. Start the existing PipeWire backend against that session's original node
   ID/serial. The actual new format, native crop and transform must equal the
   selected contract. A connection-local frame-pool generation is not substituted
   for the public source identity.
5. Normalize the replacement connection's actual first native lease once for
   verification/extent. Deliver one queued `captureReady`, then permit one
   in-bounds `claim`. The first lease retains its original metadata and timestamp.

This order proves the old application-side preview resources cannot be
reused as the new seed. It does **not** by itself prove that a compositor never
replays an already-composed texture.

## Cancellation and callers

A refresh token remains cancellable until its queued terminal is delivered.
If shutdown or a native failure wins after a success was queued, claim admission
stays closed and the error terminal waits for actual producer/preview/Portal
drain. `prepared` is not emitted a second time. Parent disappearance after the
original authorization does not invalidate an otherwise active session.

`RecordingWorkspace` includes pending unmap/refresh preparation in its real Stop
route before creating a recording worker or output staging file. It handles
failure, cancellation, capability loss and application exit before claiming a
consumer. `ScrollCaptureSession::prepareLive` stores the selection only;
`start` refreshes and then adopts the single consumer. Closing during refresh
waits for that terminal. Once a worker runs, the existing source-consumer drain
and `consumerFinished` handshake remain unchanged.

Manual scrolling hides its complete window, rather than warning users to move
it outside an unmapped source coordinate system. Both workspaces use actual
tray/registered-shortcut capability or a verified installed desktop action
provided by the host. Without a reachable
stop route they refuse hidden Start and retain visible instructions. A saved
shortcut preference or desktop command is not a registered stop action.
The [GNOME desktop action](desktop-capture-action.md) uses real GIO registration
and a one-use round trip through the existing primary-instance IPC. Controls
show the available completion route in the selected language before hiding.
An unregistered AppImage or independent one-shot instance does not acquire
this capability. Actual GNOME menu reachability, shell UI in captured pixels,
and the complete Issue #5 §8.4 journey remain release gates.

Native X11 continues through the existing guarded XCB implementation and
Windows continues through its native affinity/capture adapters. An XWayland
window is not promoted to a native Wayland display by this refresh helper.

## Fixed upstream evidence

Exact source identities and review scope are also recorded in
[`third_party/recording-controls/upstream.json`](../third_party/recording-controls/upstream.json).
These compositor implementations were read, not copied into the application.

| Source | Relevant observed behavior | What remains to prove |
| --- | --- | --- |
| [Wayland 1.23.1 protocol](https://github.com/wayland-mirror/wayland/blob/a9fec8dd65977c57f4039ced34327204d9b9d779/protocol/wayland.xml) | `sync` acknowledges ordered requests and resulting events; it does not promise presentation of an output image | A request callback cannot serve as a display presentation receipt |
| [Qt 6.12.1 display dispatch](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/wayland/qwaylanddisplay.cpp) | Default-queue dispatch is scheduled to the GUI-thread display owner; the frame-specific queue is a separate path | No worker access to QWindow or private dispatch loop is used |
| [Portal-wlr 0.8.1 ext capture](https://github.com/emersion/xdg-desktop-portal-wlr/blob/e1d5d16f0e064a0b788c5d70c11461b15ee7a4af/src/screencast/ext_image_copy.c) and [wlroots 0.19.0](https://github.com/deepin-community/wlroots/blob/826be6494c201ca887cddd8deb07d70c13595dd2/types/wlr_ext_image_copy_capture_v1.c) | An ext capture request waits for a source-frame event and copies actual output storage; Portal-wlr queues a completed frame and marks unfinished frames corrupt | New-consumer first-frame content, old producer drain and any in-flight backend frame must still be checked in the exact Sway environment |
| [Mutter 48.0 monitor source](https://github.com/GNOME/mutter/blob/48.0/src/backends/meta-screen-cast-monitor-stream-src.c) | Enabling capture reattaches stage watches and queues a redraw | Actual GNOME acceptance/restart and first pixels after hidden UI are not verified here |
| [KWin 6.3.5 output source](https://github.com/KDE/kwin/blob/v6.3.5/src/plugins/screencast/outputscreencastsource.cpp) | Resume immediately reports output damage; rendering reads the scene's existing output texture and uses its last presentation timestamp | A new streaming connection alone does not exclude a cached texture; actual Plasma evidence or a stronger producer-supported boundary is still required |

The wlroots build identity above is the exact Deepin source-import commit used
by the controlled CI desktop, not a claim that it is an upstream Git commit.
Its canonical upstream and license are recorded by the existing validation
desktop lock. No compositor runtime is added to the application.

## Real validation entry points

`live_capture_refresh_probe` is independent of the existing dynamic
capture/resize/daemon-loss probe. It first displays a separate conspicuous
magenta/black control and verifies at least 1,000 actual magenta pixels in the
authorized preview. After hiding it, the test checks **every pixel of the exact
first replacement lease** against the displayed background. It does not select
a later queue frame to conceal stale controls.

The probe also records native socket device/inode identities: all old preview
producer sockets must have closed before claim, a replacement connection must
exist, and all producer sockets must be gone after real consumer/Portal drain.
It requires one preparation, one refresh terminal, the same public source
identity, continuing GUI heartbeat and actual before/after PNGs. These are real
received pixels; neither the input painting nor generated frames are exported
as capture evidence.

Use the existing isolated wrapper and fixed desktop receipt:

```sh
python3 scripts/test-pipewire-capture.py artifacts/portal/live_capture_refresh_probe \
  --pipewire-prefix .deps/pipewire-validation/install \
  --portal-backend .deps/validation-desktop/install/libexec/xdg-desktop-portal-wlr \
  --backend-build-info .deps/validation-desktop/build-info.json \
  --modes capture --artifacts artifacts/tests/live-refresh
```

Omitting `--modes` still runs all three original capture, resize and daemon-loss
scenarios for the original probes. The refresh probe supports only its explicit
capture scenario; it does not report resize or daemon loss coverage.

Local fixed-SDK strict compilation covers the Portal helper, live owner, both
workspaces, scrolling session/window and the real probes. Actual local tests
cover unauthorized remote/refresh rejection, non-Wayland display failure,
cancel-before-start, queued terminal destruction and closed admission; the
offscreen scrolling controls still pass their no-source/no-output/drain checks.
Local positive compositor execution is **not run**. Sway pixel acceptance must
come from the dedicated CI result, and that result cannot replace GNOME,
Plasma, physical displays, mixed DPI, remote desktop policy or long-run gates.

The first integrated shared-owner CI run (`24f94d4`, job `139288150686`) failed
the actual pattern assertion on its first consumed frame after refresh. The
initial preview passed and all owners drained; fresh-frame acceptance is still
open. The subsequent `659cc9d7` Linux Debug run compiled the controlled producer
backports and passed direct capture, but stopped earlier at resize: new buffers
were negotiated without another process callback. The refresh checks therefore
did not execute. [The validation backend record](portal-wlr-validation.md)
preserves both failures, the actual-pixel diagnostics and the minimal retry
correction after current-buffer removal. These changes belong only to the owned
validation desktop and cannot establish that the user's system Portal has
equivalent behavior.

