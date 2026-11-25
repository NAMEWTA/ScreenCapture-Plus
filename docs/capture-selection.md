# Capture selection

The shared `capture/SelectionModel.h` extracts the existing `CutMask` behavior
from the fixed Windows baseline. The retained Windows `CutMask` calls it for
every hover, creation, hit test, move and resize; its Direct2D rectangle is a
presentation snapshot. The Qt `CaptureSession` uses the same model. This extraction does not
establish desktop capture, mixed-DPI input or a complete Qt capture workflow.

## Source pixels and gestures

The model reuses the core point/bounds types and has no Qt, Win32, image or
window ownership. Its extent is the original image's `[0,width] × [0,height]`
boundary. Platform adapters translate native window rectangles to this source
space in front-to-back order. Candidate clipping occurs once; hovering neither
allocates nor invalidates a label when the selected rectangle stays unchanged.

Window interiors use strict comparisons. A pointer exactly on an edge can
therefore select a lower window containing that point. A press without movement
keeps the existing highlight. A manual drag can run in all four directions, and
a one-pixel initial region remains valid; the four-pixel minimum belongs to
adjusting an existing region, as in the baseline.

The selected rectangle's four extended edges divide the complete canvas into
nine hit regions. The inclusive interior and exact border move the whole
selection. The eight outside regions resize their corresponding sides or
corners, immediately snapping that edge on press. This behavior is intentionally
different from editor shape handles and annotation transparent interiors.

Every adjustment derives from its press rectangle and pointer position, avoiding
incremental drift. Moves retain their width and height and stop at the image
boundary. Resizing can cross the opposite edge. The original minimum-size
side convention after crossing is retained, including its small crossing
transition; replacing it with annotation resize rules would change interaction.

## Explicit boundary correction

In the baseline, applying the minimum size near an image edge could produce a
negative left/top or a right/bottom beyond the source. That result was later
passed to image-copy APIs. The shared model fits the same minimum-sized region
back inside the source. Images smaller than four pixels use their available
extent. Creation also clips a pointer dragged outside the canvas while mouse
capture remains active. These corrections preserve valid source pixels rather
than relying on a later rasterizer to accept an invalid crop.

The Windows candidate enumerator now skips a failed
`DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` result instead of inspecting
an uninitialized `RECT`. Window visibility, cloaking, order and the existing
greater-than-six-pixel candidate filter remain in the Windows adapter.

## Evidence and remaining integration

`CaptureSession` owns one immutable source, one selection and one outstanding
action. Window views use explicit per-screen source rectangles and X/Y scale
values; the logical virtual desktop is never multiplied by one DPR. A normal
authorized-image source has neither a desktop origin nor native window
candidates. It initially selects the complete authorized image so a click can
accept that source, while dragging performs an application crop. Position copy
uses `source(x,y)` unless a verified desktop origin exists.

Native color sampling and the Qt session share `PixelSample.h` for the original
CMYK percentage rounding. HEX, RGB and CMYK copy strings retain the old format.
One queued worker creates an independent selection image before dispatching
the real host action. Preparation and host output have distinct lifetimes;
failed output and cancelled Save As retain the selected source. Request tokens
remain unique when a closed session loads a new source, so cancelled worker
results cannot complete a later selection. An explicit CLI entry takes
precedence over Ctrl-release pinning. Dismissing a QR result closes capture,
matching the old QR dialog journey.

The capture toolbar's existing mark action uses `Action::Edit` to open the same
shared `EditorWindow`/`EditorCanvas` with all nine tools and range annotations.
It passes the actual prepared source crop to the host, without making the user
pin first or creating another editor implementation. `Action::Pin`, Ctrl-release
and the explicit pin CLI entry retain the pinned-window policy. The host accepts
the real editor before completing capture; failed admission retains the source.

`tests/capture_selection_tests.cpp` checks candidate order/clipping, exact edge
hits, unchanged hover, click acceptance, four drag directions, one-pixel regions,
all nine hit regions, immediate snapping, press-based movement, crossed edges
and minimum sizes at every source edge. It has been compiled and executed with
g++ 14, C++20, `-O2 -Wall -Wextra -Wpedantic -Werror` on the local Linux executor.
The retained Windows adapter is validated by its separate native regression build.
`tests/qt/capture_session_tests.cpp` exercises real QImage worker crops/alpha,
output completion/cancellation, CLI/Ctrl behavior, text and source-coordinate
formatting, per-screen conversion unit data and synchronous host deletion. These
Qt tests have now been compiled and executed with the fixed local Qt 6.12.1 SDK
using offscreen QPA; that component run is not a native desktop or physical DPI pass. The pure test additionally covers shared
CMYK rounding and has been executed with the same strict C++20 flags.

The model does not infer a desktop origin, screen identity, display scale or
Portal authorization. The capture service/UI must explicitly carry and verify
those mappings; a source-local coordinate is not a desktop coordinate. Physical
multi-monitor input and mixed-DPI acceptance remain separate required evidence.

Source: [huhucapture baseline CutMask.cpp](https://github.com/NAMEWTA/huhucapture/blob/0efaf45aae1062436da8238814bba17c325fe4d3/src/capture/CutMask.cpp).
Existing project and inherited ScreenCapture attribution remain applicable;
this extraction adds no external runtime dependency.

## Shared selection presentation

`ui/qt/CaptureCanvas` is the only capture selection view on both systems. Native
desktops instantiate that view for each verified Qt screen, sharing one
`CaptureSession`; an authorized image without global mapping uses the same view
in a normal application window. Qt pointer positions pass through the explicit
source transform. The 50-by-30 source-pixel magnifier retains nearest-neighbour
sampling at five physical display pixels per source pixel; its four text rows
and selection caption use ten logical-pixel text. Labels avoid the passive
toolbar, error message and authorized-image hint.

`CaptureWindow` keeps the original mark, scrolling, recording, QR, history,
close, save and clipboard action order. Its 44-pixel controls and 24-pixel icons
come from `ToolbarControls`, extracted directly from the existing shared editor;
the editor and capture do not maintain separate wrapping or icon renderers.
The old history glyph was in Windows-only Segoe MDL2 Assets and absent from the
project's icon font, so the common toolbar uses Qt's real file-list icon for
history on both platforms. A failed icon-font load keeps readable localized
text controls. Missing live capabilities are disabled with the host-provided
reason, never connected to placeholder success.

Enter and double-click copy an adjusted selection. Escape/right-click cancel.
Ctrl+H/R/K/P use the shared source sample and actual text output service; arrows
move one original-image pixel. Ctrl+C/S keep the baseline stage rules and do
not gain an unrelated adjustment-mode meaning. Successful actions close the
capture only after the host reports acceptance; failed output or cancelled Save
As retains the selected image. The same application-owned presentation can open
a later source generation, retiring its old windows without replacing editor
state. Window activation is handled by the [shared desktop adapter](window-activation.md).

The local fixed Qt 6.12.1 SDK has actually compiled MOC/resources and executed
`capture_window_tests`, the full existing `qt_editor_components` suite after
toolbar extraction, and `qt_capture_session`. The new component tests send real
Qt widget events, hand an actual worker crop directly to the existing nine-tool
editor, verify complete toolbar targets at a 240-pixel width and exercise
failure/cancel/reentrant owner destruction. Three PNG renders were inspected;
caption natural width, failure wrapping and toolbar separation were corrected.
These runs use offscreen QPA and validate component behavior and rendering only.
They do not validate real native mouse capture, actual display-coordinate
bindings, physical DPI, Portal authorization, IME or native source acquisition.
