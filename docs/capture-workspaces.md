# Shared capture and recording workspaces

`CaptureWorkspace` connects the actual `ScreenCaptureService` to the existing
`CaptureSession` and `CaptureWindow`. It sends finished selections to
`MediaWorkspace` for the shared editor, pin, history, archive and clipboard.
This layer owns a user's operation and its asynchronous lifetime; it does not
contain another renderer, filename policy or platform capture implementation.

## Selection and output

Each native snapshot carries its actual source identity, generation and screen
bindings. Frozen image actions remain usable even if the desktop changes.
Recording and scrolling additionally require a current live source; a previous
Portal Screenshot is not evidence that a later ScreenCast has the same pixels
or coordinates. An authorized stream must establish its own selection mapping.

Copy and Save As retain the selected image while the shared media service is
working. Only a completed output receipt ends a successful capture. Canceling
Save As restores the adjustable selection; an output failure retains it for
retry. Pin and edit create the same production editor/document, then close the
capture presentation. History also uses the existing media owner.

QR detection and text preparation use one bounded worker. Recognition retains
complete payloads, including explicit original-byte hex for text that cannot
be decoded. A QR result window stays alive through the real clipboard receipt,
which matters on Wayland. Closing it cancels pending work without publishing a
late result to a destroyed view. Repeated capture activation raises an existing
journey; it cannot create a competing encoder or selection over the same task.

Arrow movement reports the actual native cursor position. A desktop may clamp
the request at a monitor edge or gap. The session accepts that correction only
when the source is still current and the requested point has not been replaced
by newer physical input.

## Recording coordination

`RecordingWorkspace` connects the common selection, `RecordingWindow`,
`RecordingSession` and media output service. The existing native backend owns
capture and encoding. The workspace checks the selected format and audio
capabilities before admission; a missing backend remains unavailable.

The current desktop adapter accepts only verified desktop-pixel rectangles.
It never reinterprets a Portal bitmap rectangle as a desktop coordinate. Its
transparent input-free selection outline lies outside the recorded pixels.
Actual capture-exclusion results determine the control-window guidance; Linux
does not inherit a fictitious Windows exclusion capability.

The first accepted stop intention remains fixed. Finalization and archive
publication precede any Save As or clipboard operation. The exact archived
entry and file identity go to `MediaWorkspace`, without renaming or archiving
the recording again. Canceling Save As preserves the completed recording.
Output failures show the retained path and allow the same output to be retried.
A changed live source requests safe finalization and reports why recording
stopped. It cannot silently switch to different display pixels.

## Shutdown ordering

The application closes recording admission first and waits for the recording
session's real finalization/worker completion. It then shuts down capture and
media outputs. Capture waits for native readback, QR/text work and owned
clipboard receipts, while its screen service drains Portal and worker resources.
Finally the shared clipboard service drains all of its validation, native and
Portal work. The GUI dispatcher remains running through these acknowledgements.

The workspaces do not claim that the Linux clipboard persists after the owning
process exits. Retaining ownership or a verified external handoff is an
application lifecycle decision; a successful in-process publication alone is
not proof of either.

## Verification boundary

`capture_workspace_tests` uses the actual native desktop backend in its positive
mode. It displays a colored window, verifies its captured physical pixels,
then follows copy/archive, shared edit, pin and real Save As cancellation. It
also verifies orderly service drain and that completed history survives exit.
It does not install a fake capture or recorder. Windows and X11 desktop modes
are registered as required CTest cases, independently of their offscreen case.

The unavailable-desktop mode actually runs with Qt's offscreen backend and
checks failure propagation, no invented image/history, unavailable recording
and closed admission after shutdown. The integrated Linux build and this
negative journey have run successfully. Positive desktop execution, complete
recording-window/backend/output journeys, real mixed DPI, IME, audio, Portal
consent and performance remain separate acceptance requirements.
