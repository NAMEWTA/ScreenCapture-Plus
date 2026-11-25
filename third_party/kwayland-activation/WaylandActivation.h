/*
    SPDX-FileCopyrightText: 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Adapted from KWindowSystem 6.32.0; see upstream.md. Only the manager
    accepting an existing activation token is retained, without a singleton.
*/
#pragma once
#include "qwayland-xdg-activation-v1.h"
#include <QtWaylandClient/QWaylandClientExtension>

namespace HuhuCapture::Platform::Linux::WaylandActivation {
class Manager final : public QWaylandClientExtensionTemplate<Manager>, public QtWayland::xdg_activation_v1 {
public:
    Manager();
    ~Manager() override;
};
}
