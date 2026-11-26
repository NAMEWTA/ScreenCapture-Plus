# Shared capture and recording coordination

[`DesktopApplication`](../src/app/DesktopApplication.cpp) is the one application
owner. It connects `DesktopCapture`, `CaptureSession` and `CaptureWindow` to the
same `MediaWorkspace` used by editors, history and received media. Recording and
scrolling have one `RecordingController` and one `ScrollController`. There is no
second Qt application, capture workspace, selection canvas or encoding loop.

## Selection and output

Native snapshots contain owned pixels, a source identity, generation and verified
Qt/native screen bindings. Frozen image actions remain usable without asserting
that those pixels are a current recording source. A Portal Screenshot does not
authorize global desktop coordinates or a later ScreenCast. An authorized live
stream establishes its own source selection and frame contract.

Ordinary annotation and Ctrl-release pinning are distinct actions which open the
same editor/document with the appropriate window behavior. Save As and clipboard
output retain the selection until the real media/desktop receipt completes.
Cancellation restores the adjustable selection; failure retains pixels for retry.
QR recognition and text preparation use the bounded application worker and keep
their origin and cancellation token through the actual clipboard receipt.

## Recording and scrolling

The recording controller connects the common `RecordingWindow`, `RecordingSession`,
platform frame sources and shared GIF/MP4 encoders. Windows uses native GDI/MF/WASAPI;
Linux X11 uses the native XCB source, and Wayland uses `LiveCaptureSource` through
the single `ScreenCastSource` facade and `LiveFrameReader`. A preview cannot become
recording input until controls have been hidden/excluded and the same authorized
source has supplied a validated replacement frame.

The scroll controller uses the common `ScrollCaptureModel` through its retained
`ScrollAccumulator` interface. Native wheel input and capture remain platform
adapters. A changed identity, source failure, pause or unsafe control surface
stops capture and retains usable output instead of appending invented frames.
Hidden controls require an actually available shortcut, tray or verified desktop
action; losing that route stops the active operation safely.

The first accepted recording stop intention remains fixed. Finalization and
archive publication precede Save As or clipboard output. The exact archived
entry and file identity go to `MediaWorkspace`; a cancelled export does not
remove or duplicate the completed recording.

## Shutdown ordering

The application closes new admission and stops recording/scrolling. Their actual
worker completion precedes media shutdown, which in turn precedes final
clipboard/file-manager shutdown. Portal callbacks and native producers retain
ownership until their real completion barriers. The Qt/GLib dispatcher stays
alive throughout. Errors already returned by a worker remain visible and must
be acknowledged before exit; cancellation does not rewrite them as success.

Linux clipboard publication proves current ownership. After all service work
drains, a visible owner window offers Keep and Exit until another owner replaces
the selection or the user explicitly releases it. This does not claim clipboard
persistence after process termination.

## Validation boundary

`capture_workspace_tests` now drives the actual `DesktopApplication` and its
one capture/session/media pipeline. Its native mode reads a painted OS window,
checks physical pixels, copies and archives them, opens ordinary and pinned
editors, cancels the real Save As dialog, and drains the host. Its offscreen mode
checks explicit capture failure with no synthetic image or history.

`live_recording_tests` uses real bounded frame pools/queues, the production live
reader, shared encoders, complete media inspection and session archive. Its
controlled source is not a physical PipeWire desktop. Separate X11, Portal,
Windows media, clipboard-owner and installation probes retain native assertions.
Compilation or offscreen success does not satisfy mixed DPI, IME, physical audio,
long-running synchronization or the complete P12 acceptance matrix.
