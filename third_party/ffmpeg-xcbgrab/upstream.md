# X11 capture source

The native XCB readback, MIT-SHM allocation/readback and premultiplied cursor
blending flow in `src/platform/linux/X11Capture.cpp` is adapted from
[FFmpeg 9.0.2 `libavdevice/xcbgrab.c`](https://github.com/FFmpeg/FFmpeg/blob/946fcce07b6dcd0331c8cc609192aeff5e1924f8/libavdevice/xcbgrab.c),
commit `946fcce07b6dcd0331c8cc609192aeff5e1924f8`, Git blob
`d0b2c6e0b559242be028102f7cb2478b06858314`.
Copyright **2014 Luca Barbato**, **LGPL-2.1-or-later**. The complete license is
in `COPYING.LGPLv2.1`; the copied/adapted implementation retains its source notice.
The complete upstream file was read, including normal-image fallback, source
format discovery, cursor clipping, SHM ownership and close paths.

The retained scope is `xcbgrab_frame`, SHM allocation/readback, XFixes cursor
blending and native format/setup queries. Codec packets, rate scheduling,
selection overlays and input-device registration are not required: the existing
shared capture session and Qt presentation own those responsibilities. No
libavdevice runtime is added. XCB, RandR, SHM, XFixes and XTest are official linked
libraries rather than copied protocol or ABI declarations.

Downstream changes provide worker-confined C++ RAII, one reusable native SHM
surface and opaque BGRA buffer, a checked server attach with mode `0600`, a
server detach before cleanup, source scanline padding and actual TrueColor masks,
transactional desktop metadata, bounded dimensions, checked reply lengths and
explicit native errors. The normal XCB reply fallback remains available where
MIT-SHM cannot attach. Cursor composition uses upstream's premultiplied blend
formula and includes cursors whose hotspot lies outside the selected image.
XWayland is not treated as a Wayland desktop capture backend.

RandR monitor/output binding was checked against the complete fixed
[Qt 6.12.1 `qxcbscreen.cpp`](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/xcb/qxcbscreen.cpp),
Git blob `2eda466377966028ccdcc6b4835524aa7a3096e6`. In particular, RandR 1.5
monitor names are Latin-1 in Qt, older output names are UTF-8, and grouped
monitors already have a root-coordinate geometry. Qt's code is an inspection
reference, not copied into this adapter.

The Issue-referenced ksnip fixed commit
`059f022cb9c4bdd30d3e8311eb9199ca92fed4f3` was also read: `X11ImageGrabber`,
`BaseX11ImageGrabber`, `ImageGrabberFactory` and `X11Wrapper` (implementation
blob `009647f0a2b63ad4b2069da85788906de5456de4`, header
`549b496591f08e83058e24e8576a6aab0dcd19d1`). Its native root-cursor warning
confirms why a Qt logical cursor position cannot identify physical pixels by a
single virtual-desktop scale. Those GPL-2.0-or-later files remain inspection
references; no ksnip source is copied. The existing Windows `GdiCapture` remains
the production Windows implementation.
