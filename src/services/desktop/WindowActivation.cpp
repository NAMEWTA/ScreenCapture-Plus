#include "services/desktop/WindowActivation.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <atomic>
#include <new>
#include <vector>
#ifdef Q_OS_LINUX
#include "platform/linux/WindowActivation.h"
#endif

namespace HuhuCapture::Desktop {
struct WindowActivation::State {
    std::atomic<quint64> sequence{};
    quint64 active{};
    QPointer<QWindow> window;
    QString token;
    QTimer deadline;
    bool requested{};
    std::vector<QMetaObject::Connection> connections;
#ifdef Q_OS_LINUX
    std::unique_ptr<Platform::Linux::WindowActivation> wayland;
#endif
};
WindowActivation::WindowActivation(QObject* parent) : QObject(parent), state_(std::make_unique<State>())
{
    state_->deadline.setSingleShot(true); state_->deadline.setInterval(3000);
    connect(&state_->deadline, &QTimer::timeout, this, [this] {
        complete({state_->requested ? Error::Timeout : Error::Unavailable, 0,
            state_->requested ? QStringLiteral("The desktop did not focus the requested window") :
                QStringLiteral("The desktop did not provide an activation interface for this window")});
    });
    if (qGuiApp) connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this] { attempt(); });
#ifdef Q_OS_LINUX
    if (qGuiApp && QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        state_->wayland = std::make_unique<Platform::Linux::WindowActivation>();
        connect(state_->wayland.get(), &Platform::Linux::WindowActivation::availabilityChanged, this, [this] { attempt(); });
    }
#endif
}
WindowActivation::~WindowActivation()
{
    state_->deadline.stop();
    for (const auto& connection : state_->connections) disconnect(connection);
    if (state_->window) state_->window->removeEventFilter(this);
}
quint64 WindowActivation::request(QWindow* window, const QString& activationToken)
{
    auto token = state_->sequence.fetch_add(1) + 1;
    if (!token) token = state_->sequence.fetch_add(1) + 1;
    auto reject = [this, token](Failure failure) {
        QTimer::singleShot(0, this, [this, token, failure = std::move(failure)] { Q_EMIT finished(token, {false, false, failure}); });
    };
    if (QThread::currentThread() != thread()) {
        reject({Error::WrongThread, 0, QStringLiteral("Window activation requires the GUI thread")}); return token;
    }
    if (!qGuiApp || thread() != qGuiApp->thread() || !window) {
        reject({Error::Unavailable, 0, QStringLiteral("There is no live application window to activate")}); return token;
    }
    const auto platform = QGuiApplication::platformName();
    if (platform != QStringLiteral("windows") && platform != QStringLiteral("xcb") && !platform.startsWith(QStringLiteral("wayland"))) {
        reject({Error::Unavailable, 0, QStringLiteral("This QPA backend has no supported desktop activation")}); return token;
    }
    if (state_->active) complete({Error::Cancelled, 0, QStringLiteral("A later activation replaced this request")});
    try {
        state_->active = token; state_->window = window; state_->token = activationToken; state_->token.detach();
        state_->requested = false; window->installEventFilter(this);
        state_->connections.push_back(connect(window, &QWindow::activeChanged, this, [this] { attempt(); }));
        state_->connections.push_back(connect(window, &QObject::destroyed, this, [this] {
            complete({Error::Cancelled, 0, QStringLiteral("The activation window was closed")});
        }));
        state_->deadline.start();
        QTimer::singleShot(0, this, [this, token] { if (state_->active == token) attempt(); });
    } catch (const std::bad_alloc&) {
        complete({Error::Allocation, 0, QStringLiteral("Insufficient memory to activate the window")});
    }
    return token;
}
void WindowActivation::attempt()
{
    if (!state_->active || !state_->window || !state_->window->isVisible()) return;
    if (!state_->requested) {
#ifdef Q_OS_LINUX
        if (!state_->token.isEmpty() && QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
            if (!state_->window->isExposed()) return;
            if (!state_->wayland || !state_->wayland->submit(state_->window, state_->token)) return;
            state_->requested = true; state_->token.clear();
        } else
#endif
        {
            state_->requested = true; state_->token.clear();
            const QPointer<WindowActivation> lifetime(this);
            state_->window->requestActivate();
            if (!lifetime || !state_->active || !state_->window) return;
        }
    }
    if (state_->window->isActive() || QGuiApplication::focusWindow() == state_->window) complete();
}
void WindowActivation::complete(Failure failure)
{
    if (!state_->active) return;
    const auto token = state_->active;
    const WindowActivationResult result{state_->requested,
        !failure && state_->window && (state_->window->isActive() || QGuiApplication::focusWindow() == state_->window), std::move(failure)};
    for (const auto& connection : state_->connections) disconnect(connection);
    state_->connections.clear();
    if (state_->window) state_->window->removeEventFilter(this);
    state_->deadline.stop(); state_->window.clear(); state_->token.clear(); state_->active = 0; state_->requested = false;
    QTimer::singleShot(0, this, [this, token, result] { Q_EMIT finished(token, result); });
}
void WindowActivation::cancel(quint64 token)
{
    if (QThread::currentThread() != thread()) {
        QTimer::singleShot(0, this, [this, token] { cancel(token); }); return;
    }
    if (token && state_->active == token) complete({Error::Cancelled, 0, QStringLiteral("Window activation was cancelled")});
}
bool WindowActivation::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == state_->window && (event->type() == QEvent::Show || event->type() == QEvent::Expose || event->type() == QEvent::PlatformSurface)) {
        const auto token = state_->active;
        QTimer::singleShot(0, this, [this, token] { if (state_->active == token) attempt(); });
    }
    return QObject::eventFilter(watched, event);
}
}
