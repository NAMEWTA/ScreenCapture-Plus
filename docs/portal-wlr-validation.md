# Portal-wlr used by the isolated validation desktop

The application uses the user's actual xdg-desktop-portal service. It does not
install or replace Portal-wlr, Sway or wlroots. The separately locked
`headlessPortalValidation` toolchain exists only to exercise real Wayland,
Portal and PipeWire behavior in an owned CI desktop. Its local patches are
applied by `scripts/build-validation-desktop.py` after verifying their hashes.

## The observed reconnect failure

At application commit `24f94d403acfe82212c955729e6e34de16bce5b4`, Linux Qt job
`139288150686` passed the 79 ordinary tests and the three direct PipeWire
capture/resize/daemon-loss scenarios. In the shared live-source scenario, the
original 800 × 600 preview matched the actual painted surface. The first
consumer frame after reconnecting to the same authorized source did not match.
The owner still drained fully, and no image or stream success was reported.

The producer log recorded `ext: started frame without buffer` between its
PAUSED/remove-buffer transition and replacement buffers. The original probe
did not preserve the failing pixels. The log and source review identify
relevant producer defects; they do not yet prove which bytes caused this
particular assertion. The subsequent diagnostic probe saves the original
preview and first mismatching normalized image, its four sampled ARGB values,
native and normalized layouts, crop/transform, source PTS/sequence and real
stream activity. It still fails on that first mismatch. The isolated wrapper
records TRACE output so the producer's capture, ready and enqueue ordering can
be compared directly with those bytes.

The independently implemented `live_capture_refresh_probe` retains its
stronger check of every pixel in the exact replacement seed after a real
control window is hidden. Neither probe skips a first frame to obtain a pass.

## Selected upstream fixes

The base stays pinned to Portal-wlr 0.8.1 commit
`e1d5d16f0e064a0b788c5d70c11461b15ee7a4af`. Following the existing POSIX header
patch, two controlled backports reuse the upstream implementation:

| Patch | Source and behavior |
| --- | --- |
| `0002-backport-frame-buffer-lifetime.patch` | [5724a432](https://github.com/emersion/xdg-desktop-portal-wlr/commit/5724a432fc95690aef19e55e62178d71b60d0cbb) ties an in-flight ext/wlr frame to the buffer it writes. Enqueue/PAUSED cleanup and buffer removal cancel that frame while preserving the capture session. The original author documents removed storage and a second frame object surviving the first one. |
| `0003-backport-producer-driven-frame-scheduling.patch` | [c613a8bc](https://github.com/emersion/xdg-desktop-portal-wlr/commit/c613a8bc7cfbcf5615b59906e247b30b190c8662) makes the producer drive its graph after frame completion. [162463ab](https://github.com/emersion/xdg-desktop-portal-wlr/commit/162463aba4f0e97b7003f5a9b4b6b9080897c134) puts first start and resume through the same trigger/process path. The backport includes the official PAUSED guard, starvation retry/disarm and both legacy wlr completion/trigger dependencies. |

The starvation retry asks the producer to try obtaining an actual available
buffer at the negotiated rate. It does not generate pixels, change application
frame rates, discard a fixed number of received frames, or relax the client's
buffer and metadata checks. The legacy wlr trigger fixes are included because
copying only the initial driver change would otherwise stall that fallback
after one frame, even though this controlled pixman desktop uses the ext path.

These are context adaptations to the locked base. They do not import unrelated
later node-serial or per-frame damage changes. Upstream authors, complete
commit identities, original source blobs, exact patch hashes and the unchanged
MIT license are recorded in
[`third_party/patches/portal-wlr/upstream.json`](../third_party/patches/portal-wlr/upstream.json).
The patch descriptions retain the original attribution and applicable commit
trailers. No new application or validation runtime dependency is introduced.

## Local ownership corrections

`0004-close-retry-and-frame-ownership-gaps.patch` is a project-local correction
after reviewing the complete backports against the pinned instance destructor,
main loop and native PipeWire callbacks. It is not an upstream commit. The
destructor first enters terminal state, removes its timers and clears the retry
member before disconnect can notify the stream synchronously. A legacy frame
proxy is owned immediately after creation, including failure before a shm
buffer event and dmabuf-only negotiation. Failed initial dispatch, roundtrip or
missing constraints releases the frame and constraints and returns failure.

The existing unsupported `Y_INVERT` path may be reached from a PipeWire state
callback. Returning from that callback alone is insufficient: the native
caller still uses its stream. Enqueue now marks the instance terminal and
cancels capture/retry, returning that outcome to all continuing callers. The
existing main loop frees terminal instances after its native callbacks return.
The two add-buffer failure branches use the same deferred cleanup. Later
add-buffer callbacks leave their user data empty and allocate nothing. State,
format, process, retry and FPS callbacks cannot restart terminal work; buffer
removal still runs its normal cleanup. This uses the existing instance list
without a new thread, deferred timer allocation or ownership framework.
The exact reviewed source identities are included in `upstream.json`.

## Buffer replacement without a state change

The [subsequent Linux Debug run](https://github.com/NAMEWTA/huhucapture/actions/runs/45649036886/job/139328100702)
at `659cc9d7f882e3fd1d92cdb6f2360ed2c31ab735` compiled patches 0001–0004,
passed 83 ordinary tests and direct capture, then failed the resize scenario.
Its TRACE shows six matching 800 × 600 frames, a held capture frame, two
constraints updates, removal of the four old buffers, successful 1024 × 768
negotiation and four new buffers. No new process callback followed before the
client's five-second format deadline. The stream remained STREAMING through
replacement; no BUFFER_CONSTRAINTS frame failure was observed. The client's
geometry notification therefore did not prove that a new-size frame arrived.

Removing the current buffer correctly cancels its capture frame, but also
removes the frame-ready callback that drives the next graph iteration.
`0005-retry-after-current-buffer-replacement.patch` restores that missing
schedule through the existing upstream starvation timer, only for the held
buffer of a live, nonterminal stream. The native removal callback returns
before the timer fires; missing buffers follow the existing bounded-rate retry,
and successful dequeue, pause and teardown retain their existing disarm paths.
The patch adds no new capture loop and changes neither pixels nor deadlines.
It is a local integration correction, attributed separately from the upstream
backports. The subsequent native run passed direct capture, resize and daemon
loss, but exposed the separate delayed-capture lifetime problem below.

## Delayed capture across pause and resume

[Qt run 45784600351](https://github.com/NAMEWTA/huhucapture/actions/runs/45784600351/job/139788227903)
at `bb416cbea78f9eb9660bbb54db201a223cb433f0` compiled patches 0001–0005 and
passed the three direct PipeWire scenarios. The shared-source probe prepared a
matching 800 × 600 image, then refreshed its data connection on the same
authorized source. The producer paused while an FPS delay was pending. After
resume, its old and new timers each created an ext frame before either frame
completed. The compositor rejected the second object with
`session already has a frame object`; Portal-wlr exited and the client correctly
reported `target not found`. All source owners still drained.

`0006-cancel-delayed-capture-with-current-frame.patch` owns the existing FPS
timer in the instance and cancels it whenever the current native frame is
canceled. A delayed frame belongs to its original buffer even before the native
proxy has been created. The callback clears its timer member before capture,
because the original event loop frees the timer before dispatch; normal
instance destruction also clears it after removing its timers. Deferred capture
requires a live streaming instance and a held buffer. The negotiated FPS delay,
graph trigger and every application pixel/identity assertion are unchanged.

This is a local correction to the controlled producer, not an upstream release
or a repair to the user's Portal service. Native compilation and the three
direct scenarios subsequently passed with this timer correction. The dedicated
first-frame probe exposed the separate stationary-source problem below. The
existing shared-source and independent first-frame probes remain the acceptance
checks; neither discards an invalid first image or grants a second authorization
to hide reconnect failures.

## A stationary source after consumer replacement

The [native refresh result](https://github.com/NAMEWTA/huhucapture/actions/runs/45811133095/job/139879170949)
records 7,680 visible control pixels in the initial witness. After its consumer
disconnects, the producer pauses, cancels the in-flight frame and removes the
four old buffers. The replacement consumer negotiates four buffers and starts
one ext frame. The source is now stationary: no damage or ready event follows
before the unchanged five-second format deadline. The probe accepts no seed and
still drains its source owners. The previous duplicate-frame protocol error is
absent. Exact original evidence digests are retained in `upstream.json`.

This is consistent with the locked protocol's
[`capture` contract](https://github.com/wayland-mirror/wayland-protocols/blob/71da8bd7f9fd3ea7c5d87ad19ca087eeca137673/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml):
after the first successful frame of an ext copy session, a compositor may wait
indefinitely for the source content to change. Declaring a client buffer fully
damaged specifies which pixels need copying; it does not require new source
content or force completion of that later frame.

`0007-renew-copy-session-on-consumer-resume.patch` closes that backend copy
session when a streaming PipeWire producer pauses, including when no buffer is
held. It retains the selected target, Portal authorization and PipeWire node.
The first resumed process creates a fresh backend session, then waits for the
initial buffer constraints before it dequeues storage. Only the asynchronous
`done` callback, after updating the constraints and checking the instance is
still live, resumes the existing producer graph. This gives the new consumer
the protocol's first-frame behavior without another authorization or synthetic
screen damage.

The new session's first batch is compared with the retained dimensions, formats,
strides, device and modifiers. Identical constraints keep the existing PipeWire
negotiation, avoiding a repeated pause/recreate cycle. Actual changes still use
the original parameter-update path; no format or size restriction is relaxed.

Pause cancels the prior frame and FPS timer; close clears partial constraints
from the old session. Each temporary image-source proxy is released after
session creation, as permitted by the locked
[`destroy` contract](https://github.com/wayland-mirror/wayland-protocols/blob/71da8bd7f9fd3ea7c5d87ad19ca087eeca137673/staging/ext-image-capture-source/ext-image-capture-source-v1.xml).
Initial setup also requires a complete constraint batch. Terminal or superseded
callbacks cannot schedule capture. The client pixel, authorization-identity,
socket-lifetime and first-frame assertions remain unchanged.

## Verification and support boundary

The fixed source blobs and license have been checked by their Git object IDs.
All seven patches apply in order with `git apply --check`; the resulting source
bytes match the reviewed backports and local corrections. The diagnostic client
compiled with the fixed Qt 6.12.1 SDK and strict GCC 14 warnings. This workspace
lacks the compositor's host development headers and cannot create the private
Unix sockets required by the native desktop. The six-patch producer compiled
and passed direct capture, resize and daemon loss in the run above. Native
compilation with 0007, the dedicated stationary first-frame check and Release
remain CI work; earlier results cannot cover these new changes.

A previously executed native PipeWire client check confirmed that disconnect
can emit `UNCONNECTED` synchronously. It did not use a compositor or validate
capture. The legacy pre-buffer close, dmabuf-only start, unsupported inversion
and retry termination paths still need actual native compositor validation;
source review and patch replay do not establish those passes.

A successful isolated run would establish only the tested source versions and
backend configuration. The user's installed Portal is a separate producer;
an old or differently patched backend may still have these defects. GNOME,
Plasma, physical outputs, mixed DPI, external system Portal packages and the
complete manual-scroll/recording user flows require their own actual results.
This controlled backport does not satisfy those release gates or change an
external desktop silently.
