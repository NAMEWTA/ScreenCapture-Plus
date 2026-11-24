# Repository guide

ScreenCapture-Plus is a native Windows C++20 application using Ling, Win32,
Direct2D/DirectWrite and Windows media APIs. It ships as `ScreenCapture.exe`.
Read `CONTRIBUTING.md`, `docs/ARCHITECTURE.md` and `docs/ANNOTATIONS.md` before
changing editor or export behavior; build details are in `docs/BUILDING.md`.

## Boundaries to preserve

- Keep `src/core` standard-library-only. Windows resources belong in
  `src/platform/windows`; image encoding and annotation rendering belong in
  `src/services`; UI coordination belongs in windows/panels/toolbars.
- Use project includes relative to `src`, such as `"core/Annotation.h"`. Add
  source files to the Windows project and filters. Do not edit restored `.deps`
  as a substitute for updating pinned dependencies reproducibly.
- Captured pixels are tightly packed, top-down BGRA with opaque screen alpha.
  Check dimension/stride products and buffer lifetimes. Keep the original image
  intact for erasing/mosaic and use returned export dimensions for merged data.
- Ordinary number markers and annotations are independent tools. Annotation IDs
  and colors remain stable through undo/redo and deletion. The right rail uses
  one shared note editor; do not add one input control per marker.
- Export every active annotation's complete text. Fit readable text/badges and
  spacing, then grow the canvas/columns within checked limits. Preserve original
  source pixels at the left; report failure instead of silently dropping text.
- OCR/ImageReader integration has been removed; QR decoding remains. Preserve
  existing capture, number, recording, configuration and language behavior unless
  the requested change explicitly includes it. Treat codec/frame-rate changes as
  behavior changes and describe them accordingly.
- Keep `Lang/*.json` UTF-16. Preserve license notices and update
  `third_party/NOTICE.md` when dependencies change.

## Validation

On Windows, use the same entry points as CI:

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

Run Debug when build/dependency settings change. Pure tests in `tests/*.cpp`
each have an independent `main` and can also be compiled with a C++20 compiler
and `-Isrc`. The Windows annotation harness compiles the production renderer
without Ling or the application PCH and writes PNG evidence under
`artifacts/tests/windows`.

The separate `-Gui` harness links the already-built Release application objects
and drives the real windows and shared note editor. It has a hard timeout and
reports missing graphics/input-desktop capabilities explicitly. Keep this test
entry point outside the shipping executable.

Do not equate core tests, WARP offscreen rendering or tray smoke checks with
interactive GUI validation. Scripted GUI checks cover their named scenarios,
not physical input, IME composition, mixed DPI or audio. Record which checks actually ran; use
`docs/VALIDATION.md` for Windows input, DPI, focus, capture and audio checks.
Keep generated artifacts, `.deps` and personal IDE/configuration files out of
commits.
