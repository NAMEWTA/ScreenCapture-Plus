# Native screenshots and source identity

`ScreenCaptureService` is the single QApplication-owned capture entry. It owns
admission, cancellation, the current source identity and verified Qt/native
screen bindings. Selection, editing, QR, clipboard and archive policy remain in
the existing shared capture/session/application layers. Platform code only reads
native displays/windows/pixels and moves a real native cursor.

## Backends and pixel ownership

Windows retains `platform/windows/GdiCapture`: reusable DCs and a top-down BGRA
DIB, a real GDI flush and the existing opaque-alpha normalization. The shipping
`CutMask` visibility/cloaking/extended-frame filter now lives in the pure Win32
`CaptureGeometry` helper and is called by both the retained Windows product and
the Qt backend. The helper cannot unwind a C++ allocation through EnumWindows.
No Qt dependency is introduced into the retained product.

X11 uses one worker-owned XCB connection, RandR 1.5 monitors or older RandR
outputs, a reusable MIT-SHM readback and an owned opaque BGRA buffer. If the
server cannot attach shared memory, the mature ordinary XCB image-reply path is
used. Source byte order, scanline padding and TrueColor masks are read from the
actual server; a requested cursor is blended from the actual XFixes image.
XTest supplies real bounded scroll input for the subsequent scrolling service,
not a fabricated wheel event. The native readback/SHM/cursor flow reuses the
fixed FFmpeg source with [exact provenance and LGPL-2.1-or-later notices](../third_party/ffmpeg-xcbgrab/upstream.md).

Wayland and a detected XWayland session use the existing `PortalCapture`.
Screenshot authorization can genuinely be parent-less. Its returned local PNG
is opened and fully decoded on the worker by the existing `inspectPng` path,
including its owned regular-file handle, complete validation and memory budget.
The portal-owned file is not deleted by this consumer. Authorized pixels keep
their alpha, and are never presented as a reconstructed global desktop.

Only owned images with DPR 1 cross the service boundary. A native DIB, SHM region
or libxcb reply is never wrapped as a persistent UI image. The final native
copy occurs once on the worker. A later capture may reuse the native buffers
without changing an already published screenshot. Native source and decoded
image allocations are bounded to 512 MiB each; the selection/renderer keeps its
existing separate limits. These are allocation limits, not a claim about
measured peak memory or physical display performance.

## Coordinates and currentness

Windows matches each Qt screen to the actual HMONITOR through Qt 6.12.1's public
`QWindowsScreen::handle()`. It does not equate a friendly Qt display name with a
GDI device name. The fixed Qt [screen implementation](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/windows/qwindowsscreen.cpp)
was read in full (blob `aaecc3fbac8e929469940e290d2caae9f917105a`). X11 matches
actual RandR identities/names under the naming rules verified against Qt's
fixed XCB screen implementation. A mismatch fails rather than inventing a
scale or screen origin.

The service records the native rectangle, Qt screen identity, Qt logical
geometry, per-screen DPR and native rotation. Desktop positions can be negative;
the original image has its own zero origin. The logical virtual desktop is
never multiplied by one global scale. Native capture already includes display
rotation, so the image is not rotated again. Logical screen-size rounding is
accepted only within the one-DIP envelope after matching native identity.

The native worker compares the actual display layout before and after readback.
The GUI also verifies that its saved Qt bindings remain current before publishing
pixels. `isCurrent(sourceId,generation)` checks the published native source and
the actual current Qt screen identity/geometry/DPR/orientation. A new capture,
screen removal or relevant configuration change invalidates the old source and
emits `sourceInvalidated`; its already owned image stays editable. A taskbar
work-area change or physical-DPI metadata update without a changed pixel mapping
does not by itself abort recording. This is a currentness check, not a lock that
prevents the user/compositor from changing displays after a check. Live consumers
still observe configuration changes and validate their own active native stream.

Cursor movement is asynchronous on that same native worker. Pending repeats are
coalesced, and each terminal notification identifies the requested and observed
source positions. The desktop may clamp a cursor at a display edge/gap; the host
can correct the shared session only if that requested point still represents
its latest cursor. A late movement must not overwrite newer physical input.
Wayland authorized-image selection has a local cursor and never requests a
global warp without an actual platform capability.

## Lifetime and integration

The worker pool has one persistent thread and no expiry. Native construction,
readback, cursor work and resource destruction all use it. `cancel()` retains
ownership until the native/PNG call returns and publishes no late pixels.
`shutdown()` closes admission (later requests return zero), cancels work and
waits for the [actual Portal drain](portal-capture-lifecycle.md) plus all worker
completions. A final worker task destroys the DC or XCB connection before the
service queues `shutdownFinished()`. Application shutdown must keep the GUI
and shared GLib dispatcher alive through that notification. Immediate object
destruction has a blocking final cleanup fallback; that fallback is not the
normal application shutdown path.

The shared service links Qt Core/Gui/Concurrent, the shared capture types and
media archive/PNG inspector. Windows links the existing GDI implementation,
`CaptureGeometry`, user32, gdi32 and dwmapi. Linux links the existing Portal target
and official `xcb`, `xcb-randr`, `xcb-shm`, `xcb-xfixes`, `xcb-xtest`. The retained
MSBuild product only adds the pure Win32 `CaptureGeometry` pair; it does not link
this Qt service or any Linux dependency.

## Actual verification and remaining gates

The fixed 6d Qt 6.12.1 SDK actually compiled and linked the entire Linux XCB
adapter, backend, service, MOC, existing PNG/archive/Portal dependencies and
`tests/qt/screen_capture_tests.cpp`. The new sources were compiled with g++ 14,
C++20, `-Wall -Wextra -Wpedantic -Werror`.

The SDK inventory contained the exact XTest shared object and development link,
but omitted its runtime SONAME alias `libxcb-xtest.so.0`; an ordinary launch
correctly failed at the loader. With an explicit development-only `LD_PRELOAD`
of that same inventory-verified DSO (SHA-256
`391a0399122d4fce4d5c5e2833b2022b62ada2c8aee891ca751d4661a0b565e7`),
`screen_capture_tests --no-backend` actually passed: offscreen QPA reports
Unavailable, returns no synthetic source, and completes asynchronous shutdown.
The SDK was not modified, and this is not recorded as successful SDK relocation
or native desktop capture. The packaging owner is repairing the missing alias.

The same executable's native mode requires a real Windows or X11 desktop. It
shows an actual Qt color window, reads it back through the real capture backend,
checks verified screen coordinates, repeats a changed visible frame while
retaining old image bytes, moves/restores the real cursor, rejects superseded
source identities and verifies cancellation/shutdown. `--artifacts DIRECTORY`
saves the actual native desktop PNG. Windows and X11 positive execution has not
yet occurred for this batch; no native/DPI pass is inferred from compilation.

Separate required evidence still includes Windows native builds, real X11 and
Portal permission journeys, physical mixed-DPI/rotated monitors, hot plug,
interactive input, real SDR/HDR appearance and performance/resource measurements.
Independent X11 roots without one verified Qt/RandR desktop mapping are rejected
rather than silently merged. These boundaries do not mark P00/P01 or capture's
full acceptance gate complete.
