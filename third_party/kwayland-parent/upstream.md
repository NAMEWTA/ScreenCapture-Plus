# Wayland parent exporter provenance

This directory contains the two exported-surface classes adapted from
**KWindowSystem 6.32.0**, commit
`870fed584663656db8b5e3d4ec1a7048b5eff25f`. The original author is
**Kai Uwe Broulik**, and both source headers retain **LGPL-2.0-or-later**.
The project-wide MIT license does not replace that license.

| Original file | Git blob | Downstream file |
| --- | --- | --- |
| [waylandxdgforeignv2.cpp](https://github.com/KDE/kwindowsystem/blob/870fed584663656db8b5e3d4ec1a7048b5eff25f/src/platforms/wayland/waylandxdgforeignv2.cpp) | `a34b1e653a73f6a61739f9ff024f0458987ac9db` | WaylandForeign.cpp |
| [waylandxdgforeignv2_p.h](https://github.com/KDE/kwindowsystem/blob/870fed584663656db8b5e3d4ec1a7048b5eff25f/src/platforms/wayland/waylandxdgforeignv2_p.h) | `4a53d8f32125703b69db66f244d161486c4d0bfe` | WaylandForeign.h |

The importer classes and process-wide singleton were removed. The retained
exporter is owned by one pending Portal request, uses the existing Qt event
queue, and emits the original `handleReceived` event. Names are scoped to the
platform adapter. A removed global releases its proxy as well as reporting
inactivity. Qt AUTOMOC replaces the original explicit moc include. The original
QPA lifetime guard remains; owners must release these objects before QApplication
destroys the Wayland integration.

The platform's [PortalParent](../../src/platform/linux/portal/PortalParent.cpp)
uses the public Qt Wayland extension and the version-matched private native
window interface. Qt is fixed to **6.12.1**, qtbase commit
`5af1a37e31a869cd6eb8a73d82e2c71a450e6634`:

| Inspected Qt file | Git blob |
| --- | --- |
| [qwaylandclientextension.h](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/wayland/global/qwaylandclientextension.h) | `e5935ca821d2853cd93280566e8ac8d11f7da121` |
| [qwaylandclientextension.cpp](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/wayland/global/qwaylandclientextension.cpp) | `95f266586836e5c1bd445866d0c1aa30e57dc581` |
| [qplatformwindow_p.h](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/gui/kernel/qplatformwindow_p.h) | `73b995a84c5607ca0016f1658e75e36e49b24294` |

These Qt files are linked APIs and inspection references; they are not copied
into this directory. The bindings are generated with Qt's pinned
`qt6_generate_wayland_protocol_client_sources(... PRIVATE_CODE FILES ...)`.
The committed [xdg-foreign-v2 XML](../wayland-protocols/xdg-foreign-unstable-v2.xml)
is copied verbatim from the same qtbase commit at
`src/3rdparty/wayland/protocols/xdg-foreign/xdg-foreign-unstable-v2.xml`, blob
`cc3271dca4d6211bf6d04c34c0ca973c52f9b552`. Its original Red Hat Inc. 2015–2016
MIT notice is retained inside the XML. No protocol implementation is handwritten.

The complete LGPL-2.0-or-later text is preserved in
[LICENSES/LGPL-2.0-or-later.txt](LICENSES/LGPL-2.0-or-later.txt), copied from
SPDX license-list-data commit
[`ab9b0904b43db6f4b67431efd8821feb821f1c8b`](https://github.com/spdx/license-list-data/blob/ab9b0904b43db6f4b67431efd8821feb821f1c8b/text/LGPL-2.0-or-later.txt), blob
`843b00b561a44454249f6554c609b2899251b3fd`.
Release source and notices must include these files and this record. The existing
libportal notices and its separate LGPL-3.0-only patches remain applicable.

See [the Portal engineering guide](../../docs/linux-portal.md) for request
ownership, cancellation, actual validation scope and desktop acceptance gaps.
