/*
    SPDX-FileCopyrightText: 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-License-Identifier: LGPL-2.0-or-later
    See upstream.md for the pinned source and downstream changes.
*/
#include "WaylandActivation.h"
#include <QGuiApplication>

namespace HuhuCapture::Platform::Linux::WaylandActivation {
Manager::Manager() : QWaylandClientExtensionTemplate<Manager>(1)
{
    QObject::connect(this, &QWaylandClientExtension::activeChanged, this, [this] {
        if (!isActive() && qGuiApp && isInitialized()) destroy();
    });
    initialize();
}
Manager::~Manager()
{
    if (qGuiApp && isInitialized()) destroy();
}
}
