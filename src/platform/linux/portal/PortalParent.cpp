// The event-driven exporter is the narrowly adapted KWindowSystem component
// in third_party/kwayland-parent. No compositor roundtrip or second event loop
// is used here. The original libportal Qt helper remains the X11 implementation.
#include <libportal/portal.h>
#include <libportal-qt6/portal-qt6.h>
#include "platform/linux/portal/PortalParent.h"
#include "platform/linux/portal/PortalSupport.h"
#include "WaylandForeign.h"

#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>
#include <utility>

struct xdg_toplevel;

namespace HuhuCapture::Platform::Linux {
using namespace PortalSupport;

struct PortalParent::Private {
    QPointer<QWindow> window;
    QPointer<QNativeInterface::Private::QWaylandWindow> nativeWindow;
    GCancellable* cancellable{};
    gulong cancelledHandler{};
    XdpParent* parent{};
    std::unique_ptr<WaylandParent::Exporter> exporter;
    std::unique_ptr<WaylandParent::Exported> exported;
    QTimer deadline;
    bool requested{};
    bool exportStarted{};
    bool finished{};
    bool succeeded{};
    bool invalid{};

    void release() {
        deadline.stop();
        if (cancellable) g_clear_signal_handler(&cancelledHandler, cancellable);
        g_clear_object(&cancellable);
        if (parent) xdp_parent_free(std::exchange(parent, nullptr));
        exported.reset();
        exporter.reset();
    }
    ~Private() { release(); }
};

PortalParent::PortalParent() : d(std::make_unique<Private>()) {}
PortalParent::~PortalParent() = default;
std::shared_ptr<PortalParent> PortalParent::create() { return std::shared_ptr<PortalParent>(new PortalParent); }

void PortalParent::prepare(QWindow* window, GCancellable* cancellable)
{
    Q_ASSERT(!d->requested);
    Q_ASSERT(qGuiApp && QThread::currentThread() == qGuiApp->thread());
    if (d->requested) return;
    d->requested = true;
    d->window = window;
    if (cancellable) d->cancellable = G_CANCELLABLE(g_object_ref(cancellable));
    // Even the X11 result is delivered after request IDs have been returned.
    QTimer::singleShot(0, this, [this] { start(); });
}

XdpParent* PortalParent::nativeParent() const
{
    return d->finished && d->succeeded && !d->invalid ? d->parent : nullptr;
}

void PortalParent::complete(PortalFailure failure)
{
    const auto keepAlive = shared_from_this();
    if (d->finished) return;
    d->finished = true;
    d->succeeded = !failure;
    d->deadline.stop();
    if (failure) d->release();
    Q_EMIT prepared(failure);
}

void PortalParent::invalidate(PortalFailure failure)
{
    const auto keepAlive = shared_from_this();
    if (!d->finished) { complete(std::move(failure)); return; }
    if (!d->succeeded || d->invalid) return;
    d->invalid = true;
    d->release();
    Q_EMIT invalidated(failure);
}

void PortalParent::start()
{
    const auto keepAlive = shared_from_this();
    if (d->finished) return;
    if (g_cancellable_is_cancelled(d->cancellable)) {
        complete(failure(Code::Cancelled, QStringLiteral("Portal parent export canceled")));
        return;
    }
    if (!d->window || d->window->thread() != thread() || !d->window->isVisible() || !d->window->handle()) {
        complete(failure(Code::ParentDestroyed, QStringLiteral("Portal parent has no live visible native surface")));
        return;
    }
    QObject::connect(d->window, &QObject::destroyed, this, [this] {
        invalidate(failure(Code::ParentDestroyed, QStringLiteral("Portal parent window was destroyed")));
    });
    if (d->cancellable)
        d->cancelledHandler = g_signal_connect(d->cancellable, "cancelled", G_CALLBACK(+[](GCancellable*, gpointer data) {
            static_cast<PortalParent*>(data)->invalidate(failure(Code::Cancelled, QStringLiteral("Portal parent export canceled")));
        }), this);

    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        d->parent = xdp_parent_new_qt(d->window);
        complete(d->parent ? PortalFailure{} : failure(Code::Unsupported, QStringLiteral("Qt could not create an X11 Portal parent")));
        return;
    }
    // Native interface lookup avoids creating a second Wayland integration on
    // other QPAs; Qt's extension constructor may otherwise do exactly that.
    if (!qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        complete(failure(Code::Unsupported, QStringLiteral("The current Qt platform cannot export a Portal parent")));
        return;
    }
    if (!d->window->isTopLevel() || d->window->type() == Qt::Popup || d->window->type() == Qt::ToolTip) {
        complete(failure(Code::Unsupported, QStringLiteral("Wayland Portal parents must be XDG toplevel windows")));
        return;
    }
    d->nativeWindow = d->window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!d->nativeWindow) {
        complete(failure(Code::Unsupported, QStringLiteral("Qt did not provide a native Wayland window")));
        return;
    }
    QObject::connect(d->nativeWindow, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, [this] {
        invalidate(failure(Code::ParentDestroyed, QStringLiteral("The Portal parent's Wayland surface was destroyed")));
    });
    QObject::connect(d->nativeWindow, &QNativeInterface::Private::QWaylandWindow::surfaceRoleDestroyed, this, [this] {
        invalidate(failure(Code::ParentDestroyed, QStringLiteral("The Portal parent's XDG toplevel role was destroyed")));
    });
    QObject::connect(d->nativeWindow, &QNativeInterface::Private::QWaylandWindow::surfaceRoleCreated,
                     this, [this] { tryExport(); });
    d->exporter = std::make_unique<WaylandParent::Exporter>();
    if (!d->exporter->isActive()) {
        complete(failure(Code::Unsupported, QStringLiteral("The compositor does not advertise xdg-foreign-v2")));
        return;
    }
    QObject::connect(d->exporter.get(), &QWaylandClientExtension::activeChanged, this, [this] {
        if (d->exporter && !d->exporter->isActive())
            invalidate(failure(Code::Unavailable, QStringLiteral("The compositor removed xdg-foreign-v2")));
    }, Qt::QueuedConnection);
    d->deadline.setSingleShot(true);
    d->deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(&d->deadline, &QTimer::timeout, this, [this] {
        complete(failure(Code::Unavailable, QStringLiteral("Wayland parent handle export timed out")));
    });
    // This timeout fails an unresponsive native metadata export. It is never
    // applied to the user's Portal permission dialog or reported as success.
    d->deadline.start(5000);
    tryExport();
}

void PortalParent::tryExport()
{
    const auto keepAlive = shared_from_this();
    if (d->finished || d->exportStarted || !d->nativeWindow || !d->exporter) return;
    if (!d->nativeWindow->surface() || !d->nativeWindow->surfaceRole<xdg_toplevel>()) return;
    d->exportStarted = true;
    d->exported.reset(d->exporter->exportToplevel(d->nativeWindow->surface()));
    QObject::connect(d->exported.get(), &WaylandParent::Exported::handleReceived, this, [this](const QString& handle) {
        if (d->finished) return;
        if (handle.isEmpty() || handle.contains(QChar(u'\0'))) {
            complete(failure(Code::Failed, QStringLiteral("The compositor returned an empty or malformed parent handle")));
            return;
        }
        const auto identifier = (QStringLiteral("wayland:") + handle).toUtf8();
        d->parent = xdp_parent_new_from_handle(identifier.constData());
        complete(d->parent ? PortalFailure{} : failure(Code::Failed, QStringLiteral("The exported parent handle was invalid")));
    }, Qt::QueuedConnection);
    // Qt dispatches the generated protocol callback on its existing Wayland
    // event queue. There is deliberately no wl_display_roundtrip or dispatch.
}

} // namespace HuhuCapture::Platform::Linux
