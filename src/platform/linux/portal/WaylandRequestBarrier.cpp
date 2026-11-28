#include "platform/linux/portal/WaylandRequestBarrier.h"

#include <QGuiApplication>
#include <QThread>
#include <QTimer>
#include <qpa/qplatformwindow_p.h>
#include <wayland-client.h>
#include <cerrno>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
PortalFailure failure(PortalFailure::Code code, QString message)
{ PortalFailure result; result.code = code; result.message = std::move(message); return result; }
}

struct WaylandRequestBarrier::Private {
    wl_callback* callback{};
    QTimer deadline;
    bool started{}, done{};
    ~Private() { if (callback) wl_callback_destroy(callback); }
};

WaylandRequestBarrier::WaylandRequestBarrier(QObject* parent)
    : QObject(parent), d(std::make_unique<Private>())
{
    d->deadline.setSingleShot(true);
    connect(&d->deadline, &QTimer::timeout, this, [this] {
        complete(failure(PortalFailure::Code::Failed,
            QStringLiteral("The compositor did not acknowledge the pending window requests")));
    });
}
WaylandRequestBarrier::~WaylandRequestBarrier() = default;

void WaylandRequestBarrier::complete(PortalFailure error)
{
    if (d->done) return;
    d->done = true; d->deadline.stop();
    if (d->callback) wl_callback_destroy(std::exchange(d->callback, nullptr));
    // Consumers can destroy the barrier from this signal. Never emit from the
    // native Wayland listener stack, including cancellation/timeout paths.
    QTimer::singleShot(0, this, [this, error = std::move(error)] { Q_EMIT finished(error); });
}

void WaylandRequestBarrier::start()
{
    Q_ASSERT(qGuiApp && QThread::currentThread() == qGuiApp->thread());
    if (d->started || d->done) return;
    d->started = true;
    auto* application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto* display = application ? application->display() : nullptr;
    if (!display || !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        complete(failure(PortalFailure::Code::Unsupported,
            QStringLiteral("This Qt platform has no native Wayland display")));
        return;
    }
    d->callback = wl_display_sync(display);
    if (!d->callback) {
        complete(failure(PortalFailure::Code::Failed, QStringLiteral("The Wayland request barrier could not be created")));
        return;
    }
    static const wl_callback_listener listener{
        [](void* value, wl_callback*, std::uint32_t) {
            auto* self = static_cast<WaylandRequestBarrier*>(value);
            Q_ASSERT(QThread::currentThread() == self->thread());
            self->complete({});
        }
    };
    if (wl_callback_add_listener(d->callback, &listener, this) < 0) {
        complete(failure(PortalFailure::Code::Failed, QStringLiteral("The Wayland request barrier listener could not be installed")));
        return;
    }
    // Flush is nonblocking. EAGAIN means Qt's existing display dispatcher will
    // flush its writable socket; no private read loop or blocking poll exists.
    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        auto error = failure(PortalFailure::Code::Unavailable, QStringLiteral("The Wayland display disconnected before acknowledging window requests"));
        error.domain = QStringLiteral("errno"); error.nativeCode = errno;
        complete(std::move(error)); return;
    }
    d->deadline.start(5000);
}

void WaylandRequestBarrier::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    complete(failure(PortalFailure::Code::Cancelled, QStringLiteral("The Wayland request barrier was cancelled")));
}

} // namespace HuhuCapture::Platform::Linux
