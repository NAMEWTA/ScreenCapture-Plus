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

## Verification and support boundary

The fixed source blobs and license have been checked by their Git object IDs.
All four patches apply in order with `git apply --check`; the resulting source
bytes match the reviewed backports and local ownership correction. The new diagnostic client
compiled with the fixed Qt 6.12.1 SDK and strict GCC 14 warnings. This workspace
lacks the compositor's host development headers and cannot create the private
Unix sockets required by the native desktop. Patched producer compilation and
actual positive first-frame behavior therefore remain CI work, not local
passes.

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
