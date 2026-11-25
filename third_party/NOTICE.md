# Licenses and third-party components

huhucapture is derived from [xland/ScreenCapture](https://github.com/xland/ScreenCapture).
The recorded inherited application snapshot is
[`5e4713e2fbe333873abff911424a212923489ccd`](https://github.com/xland/ScreenCapture/commit/5e4713e2fbe333873abff911424a212923489ccd)
(`2.5.15`). We thank xland and the upstream contributors for the native capture,
editing, recording, and desktop application foundation. See
[upstream acknowledgements](https://github.com/NAMEWTA/huhucapture/blob/main/docs/upstream.md)
for provenance and maintenance boundaries.

huhucapture is maintained by NAMEWTA. Product branding does not replace the
copyright notices of inherited code. The inherited application resource declared
`Copyright (C) LiuXiaoLun 2023-2026`; that attribution is retained here. The inherited
root MIT license also contains `Copyright (c) 2020 - 2026 ThorVG Project`, which
remains in [LICENSE](https://github.com/NAMEWTA/huhucapture/blob/main/LICENSE). New huhucapture work is credited to NAMEWTA.

The application source retains the license in the repository root. The following
components retain their own copyright notices and license terms. The release ZIP
includes the license texts; installing only `huhucapture.exe` does not require
these text files at runtime.

| Component | Source and revision | License | Usage |
| --- | --- | --- | --- |
| Ling | https://github.com/xland/Ling/tree/2ae45005b2863b004b4432bfe85d6e63d7ee44ca | MIT | Windows GUI, statically linked |
| Yoga | `yoga/` in the pinned Ling checkout; original project https://github.com/react/yoga | MIT | Layout engine, statically linked |
| gifski | https://github.com/ImageOptim/gifski/tree/1060eab4500a20f27e2fa3ab7e85473d0e921cbd (1.34.0) | AGPL-3.0-or-later | Existing active GIF encoder, statically linked |
| quirc | https://github.com/dlbeer/quirc; sources inherited from ScreenCapture commit `5e4713e2fbe333873abff911424a212923489ccd` | ISC | QR-code recognition |
| cgif | https://github.com/dloebl/cgif; sources inherited from the same ScreenCapture commit | MIT | Existing alternate GIF encoder |

The inherited upstream revisions of `cgif` and `quirc` were not recorded, so the
table names the exact inherited snapshot rather than inventing a version.
`cgif` remains unchanged. The retained `quirc` decoder has two narrowly replayable
upstream fixes and one local coordinate-arithmetic fix, recorded with hashes and
author attribution in [quirc/upstream.json](quirc/upstream.json). Its ISC notices
remain intact. Real QR fixtures are generated with pinned Segno 1.6.6, a
development-only BSD-3-Clause dependency; see [segno/upstream.json](segno/upstream.json).

gifski has Rust dependencies recorded in its pinned `Cargo.lock`; restoring and
building with `--locked` preserves that dependency graph. Its AGPL license is
included separately and is not replaced by this repository's MIT license.
Sources and build instructions for this release are available at
https://github.com/NAMEWTA/huhucapture and at the pinned links above.

## Qt migration components (not yet a released product)

The additional CMake and platform validation targets do not replace the released
Windows application yet. Their exact revisions are in
[`build/dependencies.lock.json`](../build/dependencies.lock.json). Dependency
licenses apply independently of the root application license.

| Component | Source and revision | License and provenance |
| --- | --- | --- |
| Qt 6.12.1 | QtBase `5af1a37e31a869cd6eb8a73d82e2c71a450e6634`; matching Core / Gui / Widgets / Network / Svg / platform SDK | Qt's module-specific commercial or open-source terms; preserve the actual SDK license set when packaging. Validation is not a completed deployment license audit. |
| LocalSend core | `localsend/localsend` `44f48f18a5503c43873570ff489d27ff1983e198`, core crate 0.1.0 | Apache-2.0; original [LICENSE](localsend/LICENSE), [file provenance](localsend/upstream-files.json) and [adaptations](localsend/patches/native-core.patch) retained. |
| infer 0.22.0 | `bojand/infer` `cb05400c5f43e07200fbc1c5753e15946e9b1de8` | MIT, Bojan; [license](localsend/infer.LICENSE). The crate checksum is in the resolved Cargo.lock. |
| gif 0.13.3 | `image-rs/image-gif` `acf2f6f7e6195962a9f455dab50a278b7dabc884` | MIT OR Apache-2.0, image-rs Developers / nwin; [sources and checksums](gif/upstream.json), [MIT](gif/LICENSE-MIT), [Apache](gif/LICENSE-APACHE). Reuses the existing gifski dependency for strict complete-file decoding; no new GIF parser. |
| windows-sys 0.52.0 | `microsoft/windows-rs` `3a605cba064b26f2a198ac58085f8c8836f47c38` | MIT OR Apache-2.0, Microsoft Corporation; [source and checksum](windows-rs/upstream.json), [MIT](windows-rs/LICENSE-MIT), [Apache](windows-rs/LICENSE-APACHE). Reuses the locked official bindings to identify the actual open Windows file handle. |
| libportal 0.11.0 | `flatpak/libportal` `fc4d62cfe131e8494c82f597f92b2234d584c367` | LGPL-3.0-only; [COPYING](patches/libportal/COPYING) and [GPL text](patches/libportal/COPYING.GPL3) retained with the [reviewed patch](patches/libportal/0001-capture-request-lifetime-and-async-fd.patch). |
| PipeWire 1.6.11 client | `PipeWire/pipewire` `4287f31510771fc999f8a58a024c81ca10b8d55f` | MIT for the selected client/examples, Wim Taymans and PipeWire contributors; [original COPYING](patches/pipewire/COPYING), [exact source blobs and controlled patches](../docs/linux-pipewire.md). The host daemon is not an application deployment dependency. |
| Portal-wlr 0.8.1, validation desktop only | `emersion/xdg-desktop-portal-wlr` `e1d5d16f0e064a0b788c5d70c11461b15ee7a4af` | MIT, emersion and source-file contributors. The original LICENSE is preserved by the private build and in CI evidence. This real desktop backend is not shipped with the application. |
| Wayland protocols 1.41, validation build input | `wayland-mirror/wayland-protocols` `71da8bd7f9fd3ea7c5d87ad19ca087eeca137673`; upstream `gitlab.freedesktop.org/wayland/wayland-protocols` | MIT / Expat, with original COPYING and per-XML copyright notices retained. Only the CI desktop uses this separate protocol definition prefix; [scope](../docs/validation-desktop.md). |
| Sway 1.11 and Wayland 1.23.1, validation desktop only | `swaywm/sway` `990b23c0e584114c3694447344d6bbf44b2d748f`; `wayland-mirror/wayland` `a9fec8dd65977c57f4039ced34327204d9b9d779` | MIT / Expat, the Sway and Wayland contributors. Exact LICENSE/COPYING retained in the private desktop prefix and CI evidence. These services/libraries are not bundled as application runtime replacements. |
| wlroots 0.19.0, libdrm 2.4.124 and pixman 0.44.0, validation desktop only | Deepin source imports `826be6494c201ca887cddd8deb07d70c13595dd2`, `2c5b3b1dd9c5ba09900dc36f5ad7f921ac9ac732`, `02227c183c4e2f35501544d0dd1fa456e36837eb`; canonical upstream and repositories in the dependency lock | MIT and retained per-file kernel-header notices; wlroots authors include Drew DeVault, Jari Vetoniemi and contributors; libdrm/pixman retain their original multi-author notices. The import SHAs are not claimed as upstream Git commits. Original LICENSE/COPYING/copyright records are copied by the private build; [rationale and scope](../docs/validation-desktop.md). |
| FFmpeg 9.0.2 | `FFmpeg/FFmpeg` `946fcce07b6dcd0331c8cc609192aeff5e1924f8` | Linux software validation enables GPL / version3 / libx264, without nonfree; [GPL v3](ffmpeg-examples/COPYING.FFMPEG.GPL3). The Windows inspection-only profile disables encoders, x264 and network and retains LGPL-2.1-or-later. Both apply the recorded [strict MOV patch](patches/ffmpeg/upstream.json); the mux example retains Fabrice Bellard's MIT notice and frame-duration / finalization-error fixes. |
| FFmpeg MOV box traversal | Same FFmpeg 9.0.2 commit, `libavformat/mov.c` | LGPL-2.1-or-later, Fabrice Bellard, Baptiste Coudurier and the named original contributors; [exact source and adaptation](ffmpeg-mov/upstream.json), [full LGPL text](ffmpeg-mov/COPYING.LGPLv2.1). Only top-level physical bounds are reused in the shared MP4 preflight. |
| x264 API build 164 | VideoLAN x264 commit `3a21e97bf23676a0bf4616df8bc2207c9fd7b1d3`, fetched from `mirror/x264` | GPL-2.0-or-later, x264 project; [license text](ffmpeg-examples/COPYING.x264). Runtime use through FFmpeg's existing libx264 adapter. |
| x265 4.1, validation fixtures only | Official VideoLAN archive with the recorded SHA-512 and reviewed source blobs in [hevc-fixtures/upstream.json](hevc-fixtures/upstream.json) | GPL-2.0-or-later; [original COPYING](hevc-fixtures/COPYING.x265). Two independent upstream CLI builds produce real Main / Main10 inputs; x265 is not linked into or distributed with the product. |
| KWindowSystem 6.32.0 | `KDE/kwindowsystem` `870fed584663656db8b5e3d4ec1a7048b5eff25f` | LGPL-2.0-or-later, Kai Uwe Broulik; only the two asynchronous xdg-foreign export classes are adapted. [Exact source blobs and changes](kwayland-parent/upstream.md), [license](kwayland-parent/LICENSES/LGPL-2.0-or-later.txt). |
| QHotkey X11 implementation, source reference/adaptation | `Skycoder42/QHotkey` `4e3a244d87f1f7e741e1395f2ffe825f3a8ada45`, project version 1.5.0 | BSD-3-Clause, Copyright (c) 2016 Felix Barz; [unmodified license](qhotkey/LICENSE), [exact source blobs and adaptation](qhotkey/upstream.md). Passive-grab and lock-modifier behavior is adapted to the shared settings service's checked XCB backend; the QHotkey library is not linked. |
| xdg-foreign unstable v2 protocol | QtBase `5af1a37e31a869cd6eb8a73d82e2c71a450e6634`, source blob `cc3271dca4d6211bf6d04c34c0ca973c52f9b552` | MIT, Red Hat Inc. 2015–2016; the complete notice is retained in the unmodified [protocol XML](wayland-protocols/xdg-foreign-unstable-v2.xml). |
| KWindowSystem 6.32.0 activation manager | `KDE/kwindowsystem` `870fed584663656db8b5e3d4ec1a7048b5eff25f` | LGPL-2.0-or-later, Aleix Pol Gonzalez (2021); only the existing-token `xdg_activation_v1` manager is adapted. [Exact source and scope](kwayland-activation/upstream.md), [full license](kwayland-activation/LICENSES/LGPL-2.0-or-later.txt). |
| xdg-activation v1 protocol | QtBase `5af1a37e31a869cd6eb8a73d82e2c71a450e6634`, source blob `d87e633745b870ab479701048416edd14692afde` | MIT, Aleix Pol Gonzalez and Carlos Garnacho (2020); complete original notice in the unmodified [protocol XML](wayland-protocols/xdg-activation-v1.xml). |
| Windows SDK FileStream adaptation | `microsoft/Windows-classic-samples` `434f6002bdf9cf9829406c3ff2b33387982d6168` | MIT, Microsoft Corporation; [full license](windows-sdk-stream/LICENSE), [original file/blob and changes](windows-sdk-stream/upstream.json). Wraps an existing file object for the retained MF encoder; not a replacement codec or container. |

The shared clipboard, recycle and file-reveal services reuse the existing native
Windows implementation and public Qt/GIO APIs. Their exact audited source files,
license identities and distinction between reference and copied code are in
[desktop/upstream.json](desktop/upstream.json). The old Windows application also
uses the extracted native helpers; it does not link the new Qt service library.

The Qt migration aggregate in `src/platform/rust` links the same fixed gifski
source with the controlled LocalSend core into one Rust static library. It does
not copy or replace gifski's encoder or original C API. Exact header, license and
upstream lock-file provenance are retained in [gifski/upstream.json](gifski/upstream.json).
The independent upstream lock is reference material; the aggregate's actual
production resolution belongs to `third_party/localsend/Cargo.lock`. The aggregate lock was recovered byte-for-byte from both platforms
and normal builds now use `--locked`; matching-CRT executable validation remains
an explicit CI gate.

The software-codec probe also adapts the decode/drain API flow from FFmpeg's
`doc/examples/demux_decode.c`, Copyright (c) 2012 Stefano Sabatini; the
[full MIT notice](ffmpeg-examples/demux-decode-license.txt) and exact source blob
are retained. The complete-file inspection and fixture remux flow additionally retain Stefano Sabatini's [remux notice](ffmpeg-examples/remux-license.txt) and [exact source records](ffmpeg-examples/video-inspection-upstream.json). These media service candidates are subject to the actual [malformed-file gates](../docs/video-inspection.md). This is dependency validation, not a production Linux recorder;
see [scope and reproducible inputs](../docs/software-codecs.md).

LocalSend's application UI and Flutter/Dart runtime are not included. Unselected
upstream Rust APIs remain in the audited source to keep the fork reviewable; the
strict native v2 entry point and its limits are documented in
[`docs/localsend-core.md`](../docs/localsend-core.md). Rust dependencies retain
their own Cargo.lock identities and licenses; their final aggregated binary
distribution requirements remain a release gate.
