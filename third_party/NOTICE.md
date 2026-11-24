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
