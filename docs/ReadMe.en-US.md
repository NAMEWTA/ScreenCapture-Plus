[简体中文](../README.md) | English | [Русский](ReadMe.ru-RU.md) | [Bahasa Indonesia](ReadMe.id-ID.md)

![ScreenCapture-Plus](banner.png)

# ScreenCapture-Plus

**ScreenCapture-Plus v0.0.1** is a maintained fork of [xland/ScreenCapture](https://github.com/xland/ScreenCapture), a native Windows screenshot application. It supports drawing, scrolling captures, GIF / MP4 recording, QR decoding, and numbered notes exported alongside the image.

## Toolbars and annotations

- Capture, drawing, recording, scrolling-capture, and style toolbars use **44 × 44 logical-pixel hit targets and 24 logical-pixel icons**. They scale with monitor DPI and wrap on narrow displays without shrinking the buttons.
- The existing **Number** drawing tool remains available. The new **Annotation** tool next to it has a numbered speech-bubble icon.
- Each annotation receives an automatic number and color. Its marker and note retain their identity when moved, undone, or redone; deleting another note does not renumber the remaining notes.
- Saving or copying produces one image containing the screenshot, markers, and a right-side notes area. Text wraps, font size adapts to the available space, and the output can grow taller or use additional right-side columns to preserve complete notes.
- Preview zoom does not reduce the original image resolution in exported files. If the complete image exceeds output resource limits, export reports a failure and keeps the editing session open.

### Add a note

1. Select a screenshot region, then choose the drawing / pin button. Holding `Ctrl` while selecting the region opens the editor directly.
2. Choose **Annotation**, beside **Number**.
3. Click the image to place a marker, then enter a multiline note in the right panel.
4. Click an existing marker or list entry to edit its note; drag the marker to reposition it.
5. Use the toolbar's Save or Copy button to export the whole annotated image. `Ctrl+S` also saves while writing a note. Inside the text field, `Ctrl+C` copies text; press `Esc` before using it to copy the complete image.

The notes list scrolls. Hiding the panel keeps the notes and still includes them in exports. See [the annotation guide](ANNOTATIONS.md) for numbering and layout details.

## Other features

- Screenshots, scrolling screenshots, GIF / MP4 recording; optional system audio and microphone capture for MP4.
- QR decoding with an option to copy the decoded content.
- Color picker: `Ctrl+R` for RGB, `Ctrl+H` for HEX, `Ctrl+K` for CMYK.
- Rectangles, ellipses, arrows, numbered shapes, lines, text, mosaic, and eraser; hold `Shift` for constrained shapes or straight lines.
- Hover a drawing element to adjust or remove it; `Ctrl+Z` undoes and `Ctrl+Y` redoes.
- `Ctrl+S` saves, `Ctrl+C` or a double-click copies; text fields handle their own keyboard shortcuts.
- `Ctrl` + mouse wheel zooms the pinned preview; tray, one-time execution, and multiple languages are supported.
- Run a single `ScreenCapture.exe` without installation. Application dependencies use static linking, so no separate Visual C++ runtime installation is required.

## Download and requirements

[Download v0.0.1](https://github.com/NAMEWTA/ScreenCapture-Plus/releases/tag/v0.0.1) · [All releases](https://github.com/NAMEWTA/ScreenCapture-Plus/releases)

Release assets include the EXE, portable Windows x64 ZIP, symbols ZIP, `build-info.json`, and `SHA256SUMS.txt`. The portable ZIP includes languages and third-party licenses. Refer to the actual release assets for file sizes.

Requires **Windows 10 1803 or later, x64**. Exit the previous process before replacing the application manually.

## Build and verification

Use Windows x64, Visual Studio 2026 with Desktop development with C++ and ATL, PowerShell 7, Git, and Rustup. The scripts restore pinned Ling / Yoga / gifski dependencies.

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

The executable is `artifacts/bin/x64/Release/ScreenCapture.exe`. Open `ScreenCapture.slnx` after the first build for debugging. Core tests, native rendering / PNG checks, and startup checks cover different boundaries; desktop interactions, mixed DPI, IME, and audio/video still require the [Windows validation checklist](VALIDATION.md).

See [build and release](BUILDING.md), [architecture](ARCHITECTURE.md), [contributing](../CONTRIBUTING.md), and [third-party notices](../third_party/NOTICE.md).

## Command line

```powershell
# Exit after the capture task
ScreenCapture.exe --auto-quit=true
# Open drawing / pin mode after region selection
ScreenCapture.exe --enter=pin
# Scrolling capture
ScreenCapture.exe --enter=long
# Screen recording
ScreenCapture.exe --enter=video
# QR decoding; --enter=qrcode is also accepted
ScreenCapture.exe --enter=qr
# Tray only
ScreenCapture.exe --enter=tray
# Combine options
ScreenCapture.exe --enter=long --auto-quit=true
```

## Portable settings and languages

Settings default to `%appdata%\ScreenCapture\config.json`. An existing `config.json` beside the EXE takes priority. Extra languages are loaded from a `Lang` folder beside the EXE when it exists; otherwise from `%appdata%\ScreenCapture\Lang`. Simplified Chinese and English are built in. See [language setup](../Lang/ReadMe.md).

## Upstream author and sponsorship

The upstream author's attribution, [MIT license](../LICENSE), and sponsorship information are preserved. The following contacts and QR codes belong to the upstream author.

<table>
  <tr>
    <td align="center">
      <img alt="Alipay Sponsor" src="./alipay.jpg" width="160" height="160">
      <p>Alipay Sponsor</p>
    </td>
    <td align="center">
      <img alt="WeChat Sponsor" src="./wechat.png" width="160" height="160">
      <p>WeChat Sponsor</p>
    </td>
    <td align="center">
      <img alt="Author WeChat" src="./author.jpg" width="160" height="160">
      <p>Author WeChat</p>
    </td>
    <td align="center">
      <img alt="Official Account QR Code" src="./gongzhonghao.jpg" width="160" height="160">
      <p>WeChat Blog: Desktop Software</p>
    </td>
  </tr>
</table>
