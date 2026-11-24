[简体中文](../README.md) | English | [Русский](ReadMe.ru-RU.md) | [Bahasa Indonesia](ReadMe.id-ID.md)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/brand/huhucapture-logo-dark.svg">
  <img src="../assets/brand/huhucapture-logo-light.svg" alt="huhucapture" width="560">
</picture>

# huhucapture

**huhucapture** is a native Windows tool for screenshots, annotation, and screen recording, maintained by [NAMEWTA](https://github.com/NAMEWTA). It combines numbered regions and written explanations into one image, alongside pinned images, scrolling capture, GIF / MP4 recording, and QR decoding.

It is part of the [huhu](https://github.com/NAMEWTA/huhu) brand family. The Huhu mascot and capture-corner identity, asset variants, and usage rules are documented in the [brand guide](BRAND.md).

[Download v0.0.1](https://github.com/NAMEWTA/huhucapture/releases/tag/v0.0.1) · [All releases](https://github.com/NAMEWTA/huhucapture/releases) · [Report an issue](https://github.com/NAMEWTA/huhucapture/issues)

## Explain a region, save one image

![Example of a composed image with numbered regions and notes](banner.png)

This example was copied from the actual editor. Each region border, small upper-left badge, and corresponding note use the same automatic color. Deleting an annotation does not renumber or recolor the others, so gaps in the sequence are expected.

1. Select a screenshot area, then open image editing, or hold `Ctrl` while completing the selection.
2. Choose **Annotation** beside the ordinary **Number** tool.
3. Drag a rectangle in any direction. Release to enter a multiline note in the right panel.
4. Click a badge, border, or list entry to edit its note. Drag the badge or border to move the region; use its handles to resize it.
5. Save or copy to export the image, drawing, numbered regions, and complete notes together.

Tiny accidental drags do not create annotations. Region interiors remain transparent and available for nested annotations. Collapsing the panel leaves all notes in the export. Numbers and colors remain stable through undo, redo, and deletion of other items.

Export wraps text and reduces font size and spacing when needed, then expands the canvas height or adds note columns. The source image retains its original pixels. If the full result exceeds device or memory limits, export reports failure and keeps the editing session; it never silently truncates notes. See the detailed [annotation guide](ANNOTATIONS.md).

## Features

- Area screenshots, color picker, pinned images with zoom, and scrolling capture.
- GIF / MP4 recording, with system-audio and microphone options for MP4.
- Rectangle, ellipse, arrow, ordinary number, line, text, mosaic, and eraser tools.
- Rectangular annotations with automatic colors, stable IDs, and a shared note editor.
- QR decoding and copying of decoded content.
- Tray operation, one-time capture mode, portable settings, and multiple languages.

Toolbars use **44 × 44 logical-pixel hit areas and 24 logical-pixel icons**, scale with DPI, and wrap in narrow windows. OCR is not included.

## Download and run

Requires **Windows 10 version 1803 or later, x64**. Download `huhucapture.exe` from [Releases](https://github.com/NAMEWTA/huhucapture/releases) and run it directly. No installer or separately installed Visual C++ runtime is required.

The release also includes `huhucapture-v0.0.1-windows-x64.zip` with extra languages and licenses, a `-symbols.zip` archive, `build-info.json`, and `SHA256SUMS.txt`. Exit the running application before replacing it manually. The built-in updater uses this repository's stable releases and asks before installing.

The version sequence starts at `v0.0.1`. If you used an earlier development build, disable startup in the old application and exit it, then download and replace the program manually. Settings are not migrated automatically; copy your existing `config.json` to `%appdata%\huhucapture\config.json` or beside the new EXE to reuse them.

### Shortcuts

| Action | Shortcut |
| --- | --- |
| Save the complete image | `Ctrl+S` |
| Copy the complete image | `Ctrl+C`, `Enter`, or double-click while not editing text |
| Undo / redo drawing | `Ctrl+Z` / `Ctrl+Y` while not editing text |
| Constrain ordinary shapes | Hold `Shift` while drawing |
| Zoom a pinned image | `Ctrl` + mouse wheel |
| Copy RGB / HEX / CMYK | `Ctrl+R` / `Ctrl+H` / `Ctrl+K` during color picking |

While a text field has focus, typing, selection, deletion, and `Ctrl+C` belong to the field. `Ctrl+S` still saves the full image; press `Esc` to end text editing before copying the image. The shared annotation editor currently has no independent text undo stack; `Ctrl+Z` / `Ctrl+Y` do not remove whole annotations while typing.

## Command line and settings

```powershell
huhucapture.exe --auto-quit=true
huhucapture.exe --enter=pin
huhucapture.exe --enter=long
huhucapture.exe --enter=video
huhucapture.exe --enter=qr
huhucapture.exe --enter=tray
huhucapture.exe --enter=long --auto-quit=true
```

`--enter=qrcode` is an alias for `--enter=qr`. These options select desktop workflows.

Settings default to `%appdata%\huhucapture\config.json`. An existing `config.json` beside the EXE takes priority; create an empty file there to enable portable settings. Additional languages load from `Lang` beside the EXE, or, only if that directory is absent, `%appdata%\huhucapture\Lang`. The locations are not merged. Simplified Chinese and English are embedded. See [language files](../Lang/ReadMe.md).

## Build and contribute

Use Windows x64, Visual Studio 2026 with the C++ desktop workload, MSVC v145, ATL and Windows SDK, PowerShell 7, Git, and Rustup.

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

The binary is `artifacts/bin/x64/Release/huhucapture.exe`; the solution is `huhucapture.slnx`. Documentation covers [build and release](BUILDING.md), [architecture](ARCHITECTURE.md), [validation](VALIDATION.md), and [contributing](../CONTRIBUTING.md). The detailed engineering guides are currently in Chinese. Automated checks do not replace manual input, IME, mixed-DPI, or audio verification.

## License

See the [MIT license](../LICENSE) and [third-party notices](../third_party/NOTICE.md). Preserve the applicable license and copyright notices when distributing the application.
