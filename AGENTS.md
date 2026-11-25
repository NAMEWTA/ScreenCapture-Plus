# huhucapture repository guide

## Active Qt migration

[Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5) is the sole P00–P14
implementation plan. Issue #4 supplements Linux and LocalSend details. Read
[the current migration boundaries](docs/qt-migration.md) before editing the new
CMake, Qt, Linux, or Rust paths. The target product uses one C++20 / Qt 6 Widgets
UI, editor, and service implementation; only actual OS capabilities belong in
platform adapters. The shipping Windows build described below remains the
regression baseline until P12 passes; P13 then removes Ling and the old build.
Do not describe component validation as a complete Qt/Linux product or weaken
release gates to remove the old architecture early.

Reuse existing production behavior first, pinned cited upstream second, then
mature libraries. Keep pure editor models free of Qt/Win32 and use them in the
existing adapters while migrating. Register shared targets and tests in CMake;
register files used by the old Windows product in its project and filters too.
Pinned external changes need exact provenance, licenses and replayable patches.
Do not edit restored dependencies as an undocumented build fix. LocalSend and
gifski must ultimately share one aggregated Rust static library and the Qt CRT.

Qt/native/Portal tests and old Windows regression checks have distinct scopes.
Portal fault tests use a real bus, while desktop authorization and PipeWire
frames require separate real desktop evidence. Preserve unknown configuration
fields and original migration bytes; a stale writer or failed backup must not
overwrite user data. See the migration guide for atomicity and durability limits.

## Shipping Windows baseline

huhucapture is a native Windows x64 C++20 application using Ling, Win32,
Direct2D/DirectWrite, and Windows media APIs. It ships as `huhucapture.exe`.
The canonical repository is `NAMEWTA/huhucapture`; the maintained branch is
`main`. Start with [CONTRIBUTING.md](CONTRIBUTING.md), then read the relevant
[architecture](docs/architecture.md), [annotation](docs/annotations.md), and
[build](docs/building.md) guides before editing those paths. Directory, coding,
and documentation rules live in [conventions](docs/conventions.md).

## Module and ownership boundaries

- Keep `src/core` standard-library-only. Put Win32/COM resource ownership in
  `src/platform/windows`, encoding and image composition in `src/services`, and
  window/panel/toolbar coordination in the presentation layer.
- Use includes relative to `src`, such as `"core/Annotation.h"`. Register new
  production sources in `build/windows/huhucapture.vcxproj` and its filters.
  Add independent pure C++ test programs under `tests` when behavior needs one.
- Keep `AnnotationRenderer.cpp` independent of Ling, App, global settings, and
  the application PCH. The Windows renderer harness compiles it directly.
- UI objects and configuration/language WinRT objects belong to the UI thread.
  Background tasks use copied data and explicit lifetime checks. Stop and join
  workers before releasing any resource they can access.
- Captured pixels are tightly packed, top-down BGRA; screen alpha is opaque.
  Check dimensions, stride multiplication, and borrowed-buffer lifetimes.
  Preserve the original image for erasing/mosaic. Use returned composition
  dimensions when reading exported pixels.
- Reuse capture resources and buffers when dimensions are unchanged. Do not
  introduce repeated full-image copies into frame loops. Codec, frame rate,
  sample rules, and matching thresholds are observable behavior changes.

## Editor and annotation contracts

- Ordinary number markers and annotations are independent tools. An annotation
  is one history-owned rectangle, upper-left badge, stable ID, and complete note.
  Deletion, undo, redo, cancellation, and a cleared redo branch never reuse IDs
  or recolor surviving annotations.
- Keep region and badge colors identical. Clamp geometry to the source image;
  discard newly drawn degenerate regions. Geometry rules belong in `src/core`.
  Only badges, borders, and resize handles hit-test the region; its transparent
  interior remains available for nested marks.
- Focus note input after completing a new region or clicking an existing item,
  not during pointer capture or an existing move/resize. On capture/focus loss,
  discard an unfinished new region and stop existing geometry at its last valid
  position. Defer panel-induced view movement until a drag ends.
- The right panel uses one shared note editor and cached list layouts, not one
  input control per annotation. Shapes own the full text; panel rows are display
  snapshots. Text input must not accidentally undo or delete an entire shape.
- Passive toolbar layout uses `SWP_NOACTIVATE`; it must not steal focus or mouse
  capture. Filter Ling's mouse-leave sentinel before image hit-testing/dragging.
- Export every active annotation's full text, including a localized placeholder
  for an empty note. Fit font, badge, and spacing, then grow the canvas/columns
  within checked limits. Preserve source pixels and alpha at the upper left.
  Report failure and retain editing state instead of truncating text or exporting
  a partial result. Failed planning/rendering must not replace valid outputs.
- Preserve capture, pinning, scrolling, drawing, recording, QR decoding, settings,
  languages, and command-line behavior unless the task explicitly changes them.
  OCR is absent; do not restore obsolete controls, downloads, or plugin discovery.

## Media archive contracts

- Saving or completing a copy archives the finished image or recording under
  the Windows Documents known folder's `huhucapture` directory. Use the shared
  media service for naming, exclusive creation and persistence; UI handlers must
  not implement a second archive path or delete an entire directory.
- Use local `YYYYMMDD-HHMMSS` names and collision suffixes. Only completed
  top-level timestamp-named PNG/GIF/MP4 files belong to history. Keep original
  files independent of rebuildable thumbnails and the Windows clipboard.
- Finalize recording before archiving and copy a stable media path. On archive
  failure retain the temporary recording or editing session and report failure.
  Cancelling Save As must not discard an already completed archive.
- Clear only the listed managed media after confirmation, preferring the Recycle
  Bin. If Windows cannot recycle, retain its additional deletion confirmation;
  do not silently fall back to permanent deletion. Refresh remaining items after
  failure or cancellation. Do not
  touch unrelated files, subdirectories, or copies saved elsewhere.
- Background thumbnail work is bounded and checks UI lifetime on completion.
  See [media history](docs/media-history.md) for the persistence and UX contract.

## Identity, resources, and dependencies

- Use lowercase `huhucapture` for product, executable, solution, configuration,
  and release filenames; use `HuhuCapture` for the C++ namespace. Runtime brand
  values belong in `src/app/Brand.h`; version values belong in
  `build/windows/Version.props`. Keep updater and package metadata consistent.
- Follow [docs/brand.md](docs/brand.md) for SVG, PNG, and ICO assets. Brand family
  association does not imply an unimplemented integration with another product.
- Keep `Lang/*.json` UTF-16 with BOM. Update embedded and external translations
  together, retaining English fallback and short toolbar labels.
- Update the lock file for dependency changes. Do not patch restored `.deps` as
  a substitute for a reproducible upgrade. Preserve `LICENSE`, dependency
  notices, and the notices included in release archives.
- Keep maintenance documents in `docs/` with lowercase kebab-case Markdown
  names, including lowercase locale suffixes. Standard root discovery files and
  upstream license filenames retain their canonical names. Document rationale,
  boundaries and failure semantics; link code rather than duplicating API lists.
- Settings and history follow [design](docs/design.md); mascot palette and
  annotation colors have independent roles and must not recolor all UI chrome.
- Keep generated `artifacts`, `.deps`, IDE state, and personal settings out of
  commits. Documentation must describe current behavior and reproducible steps,
  without obsolete branch, run, or commit narratives.

## Validation and release

Use the same Windows entry points as CI:

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/test-windows.ps1 -Update
pwsh ./scripts/package.ps1
```

Also build Debug when changing dependencies or build settings. Pure tests in
`tests/*.cpp` each have their own `main` and can run with a C++20 compiler and
`-Isrc`. The WARP harness writes native renderer/PNG evidence under
`artifacts/tests/windows`. The separate GUI harness links Release application
objects and drives real windows; keep that entry point outside the shipping EXE.
It has a hard timeout and fails explicitly when graphics/input capabilities are
missing.
The separate `-Update` mode exercises the user-triggered download against the
current published GitHub release. It has its own network budget and removes only
the completed file created by that check; normal GUI checks remain offline.

Core tests, WARP rendering, tray smoke, and scripted GUI checks have different
coverage. None proves physical input, IME composition, mixed DPI, or audio worked.
Record actual commands, results, and untested boundaries according to
[docs/validation.md](docs/validation.md). Do not claim checks that did not run.

Normal stable releases are declared by `releases/publish.json`, matched by
`Version.props` and `releases/<tag>.md`. The main workflow publishes only after
all required checks pass; an already published declaration is left unchanged.
See [docs/building.md](docs/building.md) for asset validation and recovery steps.
