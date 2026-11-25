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

## Actual validation and unfinished integration

`tests/scroll_capture_model_tests.cpp` generates one deterministic BGRA page with
a fixed header and samples viewports from it. The stitched output is compared
against the actual contiguous source page, rather than against another copy of
the matching implementation. The test also exercises stationary retries,
animation rechecks, progress resetting retries, the original height threshold,
bounded capacity growth and retained output after memory refusal.

The new model and those tests actually compiled with g++ 14, C++20,
`-O2 -Wall -Wextra -Wpedantic -Werror`, and all five groups passed. Existing
`scroll_stitch_tests.cpp` remains the separate matcher/threshold characterization
suite. This batch does not claim that the changed retained Windows adapter has
been rebuilt or that native scrolling, Portal consent, mixed DPI or performance
has passed. The shared live-source/session/Qt preview and actual Windows/X11/
Wayland scrolling journeys are still required before P08 is complete.

Wayland live frames must originate from one ScreenCast authorization and its
PipeWire remote. A previous Screenshot rectangle does not prove the identity or
pixel mapping of that stream. The first actual stream frame must be confirmed
through the existing shared selection UI. Manual scrolling uses that same live
source; automatic input is offered only with real RemoteDesktop/libei permission
and capability. These are implementation requirements, not completed validation.
