# huhucapture repository guide

huhucapture is a C++20 and Qt 6 Widgets application for Windows x64 and Linux
amd64. The maintained branch is `main` in `NAMEWTA/huhucapture`. Start with
[CONTRIBUTING.md](CONTRIBUTING.md) and the relevant [architecture](docs/architecture.md),
[annotation](docs/annotations.md), [build](docs/building.md), and
[conventions](docs/conventions.md) guides.

The application shares its UI, editor and service logic. Keep actual OS
capabilities in platform adapters. Reuse existing behavior first and pin external
sources, patches and licenses. LocalSend and gifski share one Rust static
library and the Qt CRT. The v0.0.6 tag retains the Windows update bridge source.

## Module and ownership boundaries

- Keep `src/core` standard-library-only. Put Win32/COM resource ownership in
  `src/platform/windows`, encoding and image composition in `src/services`, and
  window/panel/toolbar coordination in the presentation layer.
- Use includes relative to `src`, such as `"core/Annotation.h"`. Register new
  production sources in the applicable CMake target.
  Add independent pure C++ test programs under `tests` when behavior needs one.
- UI objects and application-owned configuration and language state belong to the UI thread.
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
- Passive toolbar layout must preserve focus and pointer capture.
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
  the operating system's Documents folder in a `huhucapture` directory. Use the shared
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
  values belong in `src/app/Brand.h`; `build/version.json` is the sole
  hand-maintained version source. Run `python scripts/version_source.py --write`
  after changing it; root `VERSION` and `build/windows/Version.props` are
  generated compatibility files. CMake, MSBuild and packaging reject stale
  generated values; tag arguments verify the source and must not relabel it.
  Keep updater and package metadata consistent.
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

Use the same CMake presets and fixed dependencies as [CI](docs/building.md).
Run the `core` preset and the affected production and Qt contract tests.
Run Debug and Release when changing dependencies or build settings. Keep
test-only entry points out of the shipping application.

Core tests, platform probes, GUI tests and package checks have distinct
coverage. Record actual commands, results and untested boundaries in accordance
with [docs/validation.md](docs/validation.md).

Stable Qt releases are declared by `releases/publish.json`, matched by
`build/version.json` and `releases/<tag>.md`. The explicit complete-release workflow
publishes only the verified same-source aggregate after all required gates pass.
The Qt release validates the complete package with the published P11a bridge
from its fixed source tag. Published versions are immutable. See [docs/building.md](docs/building.md) for admission and recovery.
