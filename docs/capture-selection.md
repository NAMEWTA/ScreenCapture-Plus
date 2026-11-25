# Capture selection

The shared `capture/SelectionModel.h` extracts the existing `CutMask` behavior
from the fixed Windows baseline. The retained Windows `CutMask` calls it for
every hover, creation, hit test, move and resize; its Direct2D rectangle is a
presentation snapshot. The Qt capture migration must use the same model. This extraction does not
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

`tests/capture_selection_tests.cpp` checks candidate order/clipping, exact edge
hits, unchanged hover, click acceptance, four drag directions, one-pixel regions,
all nine hit regions, immediate snapping, press-based movement, crossed edges
and minimum sizes at every source edge. It has been compiled and executed with
g++ 14, C++20, `-O2 -Wall -Wextra -Wpedantic -Werror` on the local Linux executor.
The retained Windows adapter still needs its actual Windows regression build.

The model does not infer a desktop origin, screen identity, display scale or
Portal authorization. The capture service/UI must explicitly carry and verify
those mappings; a source-local coordinate is not a desktop coordinate. Physical
multi-monitor input and mixed-DPI acceptance remain separate required evidence.

Source: [huhucapture baseline CutMask.cpp](https://github.com/NAMEWTA/huhucapture/blob/0efaf45aae1062436da8238814bba17c325fe4d3/src/capture/CutMask.cpp).
Existing project and inherited ScreenCapture attribution remain applicable;
this extraction adds no external runtime dependency.
