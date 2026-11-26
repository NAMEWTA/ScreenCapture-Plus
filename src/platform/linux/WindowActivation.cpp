#include "platform/WindowActivation.h"

#include <QGuiApplication>
#include <QThread>
#include <QWindow>
#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

struct xdg_toplevel;

namespace HuhuCapture::Platform {

ActivationTokenResult applyActivationToken(QWindow* window, const QString& token)
{
    // This narrowly scoped adapter is source-reviewed against the dependency
    // lock. Private Qt APIs must be reviewed again when that lock is updated.
    static_assert(QT_VERSION == QT_VERSION_CHECK(6, 12, 1));
    if (token.isEmpty()) return ActivationTokenResult::NotNeeded;
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread())
        return ActivationTokenResult::WindowUnavailable;
    // Native lookup cannot instantiate a Wayland integration on X11/offscreen.
    if (!qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        return ActivationTokenResult::NotNeeded;
    if (!token.isValidUtf16() || token.contains(QChar(u'\0')))
        return ActivationTokenResult::InvalidToken;
    if (!window || window->thread() != QThread::currentThread() || !window->isVisible() ||
        !window->handle() || !window->isTopLevel() || window->type() == Qt::Popup ||
        window->type() == Qt::ToolTip || window->flags().testFlag(Qt::WindowDoesNotAcceptFocus))
        return ActivationTokenResult::WindowUnavailable;
    auto* native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    auto* wayland = qobject_cast<QtWaylandClient::QWaylandWindow*>(native);
    if (!wayland || !wayland->surface() || !wayland->shellSurface() ||
        !native->surfaceRole<xdg_toplevel>())
        return ActivationTokenResult::WindowUnavailable;
    if (!wayland->display()->hasRegistryGlobal(u"xdg_activation_v1"))
        return ActivationTokenResult::Unsupported;
    // Qt's XDG shell stores this exact token, then consumes and clears it in
    // requestActivate(). Without an existing shell role, Qt discards the token.
    wayland->setXdgActivationToken(token);
    return ActivationTokenResult::Attached;
}

} // namespace HuhuCapture::Platform
