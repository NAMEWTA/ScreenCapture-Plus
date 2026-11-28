/*
    SPDX-FileCopyrightText: 2023 Kai Uwe Broulik <kde@broulik.de>
    SPDX-License-Identifier: LGPL-2.0-or-later
    See upstream.md for the pinned source and downstream changes.
*/
#include "WaylandForeign.h"
#include <QGuiApplication>

namespace HuhuCapture::Platform::Linux::WaylandParent {

Exported::Exported(::zxdg_exported_v2* object)
    : QObject(), QtWayland::zxdg_exported_v2(object) {}

Exported::~Exported()
{
    if (qGuiApp && isInitialized()) destroy();
}

QString Exported::handle() const { return m_handle; }

void Exported::zxdg_exported_v2_handle(const QString& handle)
{
    m_handle = handle;
    Q_EMIT handleReceived(handle);
}

Exporter::Exporter() : QWaylandClientExtensionTemplate<Exporter>(1)
{
    // Global removal must also release the proxy. The upstream destructor's
    // isActive() check alone would skip it after activeChanged(false).
    QObject::connect(this, &QWaylandClientExtension::activeChanged, this, [this] {
        if (!isActive() && qGuiApp && isInitialized()) destroy();
    });
    initialize();
}

Exporter::~Exporter()
{
    if (qGuiApp && isInitialized()) destroy();
}

Exported* Exporter::exportToplevel(::wl_surface* surface)
{
    return new Exported(export_toplevel(surface));
}

} // namespace HuhuCapture::Platform::Linux::WaylandParent
