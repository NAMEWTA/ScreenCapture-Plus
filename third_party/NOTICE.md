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

The `cgif` and `quirc` C sources have only moved directories. Their original
upstream revision was not recorded by the inherited project, so the table names
the exact inherited snapshot rather than inventing an upstream version.

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
| libportal 0.11.0 | `flatpak/libportal` `fc4d62cfe131e8494c82f597f92b2234d584c367` | LGPL-3.0-only; [COPYING](patches/libportal/COPYING) and [GPL text](patches/libportal/COPYING.GPL3) retained with the [reviewed patch](patches/libportal/0001-capture-request-lifetime-and-async-fd.patch). |
| FFmpeg 9.0.2 | `FFmpeg/FFmpeg` `946fcce07b6dcd0331c8cc609192aeff5e1924f8` | Software validation configured with GPL / version3 / libx264, without nonfree; [GPL v3](ffmpeg-examples/COPYING.FFMPEG.GPL3). The unmodified mux example separately retains Fabrice Bellard's MIT notice. |
| x264 API build 164 | VideoLAN x264 commit `3a21e97bf23676a0bf4616df8bc2207c9fd7b1d3`, fetched from `mirror/x264` | GPL-2.0-or-later, x264 project; [license text](ffmpeg-examples/COPYING.x264). Runtime use through FFmpeg's existing libx264 adapter. |

The software-codec probe also adapts the decode/drain API flow from FFmpeg's
`doc/examples/demux_decode.c`, Copyright (c) 2012 Stefano Sabatini; the
[full MIT notice](ffmpeg-examples/demux-decode-license.txt) and exact source blob
are retained. This is dependency validation, not a production Linux recorder;
see [scope and reproducible inputs](../docs/software-codecs.md).

LocalSend's application UI and Flutter/Dart runtime are not included. Unselected
upstream Rust APIs remain in the audited source to keep the fork reviewable; the
strict native v2 entry point and its limits are documented in
[`docs/localsend-core.md`](../docs/localsend-core.md). Rust dependencies retain
their own Cargo.lock identities and licenses; their final aggregated binary
distribution requirements remain a release gate.
