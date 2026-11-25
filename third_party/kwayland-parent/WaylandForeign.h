/*
    SPDX-FileCopyrightText: 2023 Kai Uwe Broulik <kde@broulik.de>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Adapted from KWindowSystem 6.32.0, commit
    870fed584663656db8b5e3d4ec1a7048b5eff25f.
    Only the exported-surface classes are retained. See upstream.md.
*/
#pragma once

#include "qwayland-xdg-foreign-unstable-v2.h"
#include <QObject>
#include <QtWaylandClient/QWaylandClientExtension>

namespace HuhuCapture::Platform::Linux::WaylandParent {

class Exported final : public QObject, public QtWayland::zxdg_exported_v2 {
    Q_OBJECT
public:
    explicit Exported(::zxdg_exported_v2* object);
    ~Exported() override;
    QString handle() const;

Q_SIGNALS:
    void handleReceived(const QString& handle);

protected:
    void zxdg_exported_v2_handle(const QString& handle) override;

private:
    QString m_handle;
};

class Exporter final : public QWaylandClientExtensionTemplate<Exporter>, public QtWayland::zxdg_exporter_v2 {
public:
    Exporter();
    ~Exporter() override;
    Exported* exportToplevel(::wl_surface* surface);
};

} // namespace HuhuCapture::Platform::Linux::WaylandParent
