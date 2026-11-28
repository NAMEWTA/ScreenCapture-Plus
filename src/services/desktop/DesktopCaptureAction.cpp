#include "services/desktop/DesktopCaptureAction.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <exception>
#include <functional>
#include <stop_token>
#include <utility>

#ifdef Q_OS_LINUX
#include "platform/linux/DesktopCaptureAction.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#endif

namespace HuhuCapture::Desktop {
namespace {
constexpr char proofEnvironment[] = "HUHUCAPTURE_DESKTOP_CAPTURE_PROBE";
constexpr int verificationDeadlineMs = 15000;
Failure failed(Error code, QString detail) { return {code, 0, std::move(detail)}; }
Failure cancelled() { return failed(Error::Cancelled, QStringLiteral("Desktop capture-action verification was cancelled")); }
Failure unavailable() { return failed(Error::Unavailable, QStringLiteral("This desktop has no verified capture finish action")); }
}

struct DesktopCaptureAction::State : std::enable_shared_from_this<State> {
    struct Work {
        quint64 token{};
        QByteArray challenge;
        std::stop_source stop;
        QElapsedTimer elapsed;
        Failure failure;
        bool received{}, checking{}, checked{}, terminalQueued{};
#ifdef Q_OS_LINUX
        QPointer<QDBusPendingCallWatcher> shellCall;
        QPointer<QFutureWatcher<Platform::Linux::CaptureActionNativeResult>> worker;
        std::optional<Platform::Linux::CaptureActionRegistration> registration;
#endif
    };
    QPointer<DesktopCaptureAction> owner;
    QString executable, profile, label;
    std::shared_ptr<Work> active;
    QTimer deadline, connectionWatch;
    QFileSystemWatcher files;
    quint64 sequence{};
    unsigned deliveries{};
    bool ready{}, closing{}, shutdownQueued{}, destroyed{};
#ifdef Q_OS_LINUX
    QDBusConnection bus{QDBusConnection::sessionBus()};
    QDBusServiceWatcher shell{QStringLiteral("org.gnome.Shell"), bus, QDBusServiceWatcher::WatchForOwnerChange};
    std::unique_ptr<Platform::Linux::CaptureActionMonitor> appInfoMonitor;
#endif

    State(DesktopCaptureAction* value, QString file, QString directory)
        : owner(value), executable(std::move(file)), profile(std::move(directory)) {}

    bool ownerThread() const { return owner && QThread::currentThread() == owner->thread(); }
    bool setAvailable(bool value)
    {
        if (ready == value) return owner && !destroyed;
        ready = value;
        if (owner && !destroyed) Q_EMIT owner->availabilityChanged(value);
        return owner && !destroyed;
    }
    void clearWatch()
    {
        const auto paths = files.files() + files.directories();
        if (!paths.isEmpty()) files.removePaths(paths);
        label.clear();
    }
    void bind()
    {
        const std::weak_ptr<State> weak = shared_from_this();
        deadline.setSingleShot(true); deadline.setInterval(verificationDeadlineMs);
        QObject::connect(&deadline, &QTimer::timeout, owner, [weak] {
            if (auto state = weak.lock()) state->invalidate(failed(Error::Timeout,
                QStringLiteral("The desktop finish action did not confirm this instance within its deadline")));
        });
        const auto changed = [weak] {
            if (auto state = weak.lock()) state->invalidate(failed(Error::ChangedOnDisk,
                QStringLiteral("The registered desktop action or its executable changed")));
        };
        QObject::connect(&files, &QFileSystemWatcher::fileChanged, owner, changed);
        QObject::connect(&files, &QFileSystemWatcher::directoryChanged, owner, changed);
#ifdef Q_OS_LINUX
        QObject::connect(&shell, &QDBusServiceWatcher::serviceOwnerChanged, owner,
            [weak](const QString&, const QString&, const QString&) {
                if (auto state = weak.lock()) state->invalidate(unavailable());
            });
        connectionWatch.setInterval(1000);
        QObject::connect(&connectionWatch, &QTimer::timeout, owner, [weak] {
            if (auto state = weak.lock(); state && !state->bus.isConnected()) state->invalidate(unavailable());
        });
#endif
    }
    void checkShutdown()
    {
        if (!closing || active || deliveries || shutdownQueued || !owner || destroyed) return;
        shutdownQueued = true;
        const auto state = shared_from_this();
        QTimer::singleShot(0, owner, [state] {
            if (!state->owner || state->destroyed) return;
            if (state->active || state->deliveries) {
                state->shutdownQueued = false; state->checkShutdown(); return;
            }
            Q_EMIT state->owner->shutdownFinished();
        });
    }
    void rejected(quint64 token, Failure error)
    {
        ++deliveries;
        const auto state = shared_from_this();
        QTimer::singleShot(0, owner, [state, token, error = std::move(error)] {
            if (state->owner && !state->destroyed) Q_EMIT state->owner->verified(token, error);
            --state->deliveries; state->checkShutdown();
        });
    }
    void invalidate(Failure error)
    {
        clearWatch();
        connectionWatch.stop();
        if (active) {
            if (!active->failure) active->failure = std::move(error);
            active->challenge.clear(); active->stop.request_stop();
        }
        if (!setAvailable(false)) return;
        consider();
    }
    void consider()
    {
        const auto work = active;
        if (!work || work->terminalQueued || !owner || destroyed) { checkShutdown(); return; }
#ifdef Q_OS_LINUX
        if (work->shellCall || work->worker) return;
        if (!work->failure && (!work->received || !work->registration)) return;
        if (!work->failure && !work->checked) {
            clearWatch();
            // Watch both the effective entry and every earlier XDG search
            // directory: a new override must revoke the previous proof too.
            if (!files.addPaths(work->registration->watchPaths).isEmpty()) {
                work->failure = failed(Error::Unavailable, QStringLiteral("Changes to the desktop finish action cannot be monitored"));
            } else {
                work->checking = true;
                startWorker(work); // close the inspect-to-watch race on a worker
                return;
            }
        }
#endif
        work->terminalQueued = true; work->challenge.clear(); deadline.stop();
        ++deliveries;
        const auto state = shared_from_this();
        QTimer::singleShot(0, owner, [state, work] {
            if (state->active != work) { --state->deliveries; state->checkShutdown(); return; }
            state->active.reset();
            auto error = work->failure;
            if (!error && state->closing) error = cancelled();
#ifdef Q_OS_LINUX
            if (!error && !state->bus.isConnected()) error = unavailable();
            if (!error && work->registration) state->label = work->registration->actionLabel;
#endif
            if (error) { state->clearWatch(); state->connectionWatch.stop(); }
            if (state->setAvailable(!error)) {
                // A host may begin shutdown from availabilityChanged. Such a
                // callback must not turn this pending proof into a success.
                if (!error && state->closing) { error = cancelled(); state->setAvailable(false); }
                if (state->owner && !state->destroyed) Q_EMIT state->owner->verified(work->token, error);
            }
            --state->deliveries; state->checkShutdown();
        });
    }
#ifdef Q_OS_LINUX
    void startWorker(const std::shared_ptr<Work>& work)
    {
        if (work != active || !owner || destroyed) return;
        try {
            auto watcher = std::make_unique<QFutureWatcher<Platform::Linux::CaptureActionNativeResult>>(owner);
            const auto state = shared_from_this();
            QObject::connect(watcher.get(), &QFutureWatcher<Platform::Linux::CaptureActionNativeResult>::finished, owner,
                [state, work, watcher = watcher.get()] {
                    Platform::Linux::CaptureActionNativeResult result;
                    try { result = watcher->result(); }
                    catch (const std::bad_alloc&) { result.failure = failed(Error::Allocation, QStringLiteral("Desktop-action verification ran out of memory")); }
                    catch (const std::exception& error) { result.failure = failed(Error::Unavailable, QString::fromUtf8(error.what())); }
                    catch (...) { result.failure = failed(Error::Unavailable, QStringLiteral("Desktop-action verification did not return a result")); }
                    watcher->deleteLater(); work->worker.clear();
                    if (state->active != work || !state->owner || state->destroyed) return;
                    if (result.failure && !work->failure) work->failure = std::move(result.failure);
                    if (!work->failure && work->checking && result.registration != work->registration)
                        work->failure = failed(Error::ChangedOnDisk, QStringLiteral("The desktop action changed before its route could be accepted"));
                    if (!work->failure) {
                        if (work->checking) work->checked = true;
                        else work->registration = std::move(result.registration);
                    }
                    state->consider();
                });
            work->worker = watcher.get();
            const auto file = executable, directory = profile;
            const auto challenge = work->challenge;
            const auto checking = work->checking;
            const auto stop = work->stop.get_token();
            watcher->setFuture(QtConcurrent::run([file, directory, challenge, checking, stop] {
                return checking ? Platform::Linux::inspectCaptureAction(file, directory, stop)
                    : Platform::Linux::launchCaptureActionProbe(file, directory, challenge, stop);
            }));
            watcher.release();
        } catch (const std::bad_alloc&) {
            work->worker.clear(); work->failure = failed(Error::Allocation, QStringLiteral("The desktop-action worker could not be allocated")); consider();
        } catch (const std::exception& error) {
            work->worker.clear(); work->failure = failed(Error::Unavailable, QString::fromUtf8(error.what())); consider();
        }
    }
    void shellReply(const std::shared_ptr<Work>& work, QDBusPendingCallWatcher* watcher)
    {
        if (work->shellCall != watcher) return;
        work->shellCall.clear();
        const QDBusPendingReply<QDBusVariant> result(*watcher);
        watcher->deleteLater();
        if (active != work || !owner || destroyed) return;
        if (!work->failure) {
            const auto version = result.isError() ? QString() : result.value().variant().toString();
            bool numeric{};
            const auto major = version.section(QChar(u'.'), 0, 0).toUInt(&numeric);
            if (result.isError() || !numeric || major < 46 || version.size() > 64)
                work->failure = unavailable();
        }
        if (work->failure) consider();
        else startWorker(work);
    }
#endif
    void shutdown()
    {
        if (closing) return;
        closing = true; deadline.stop(); connectionWatch.stop();
#ifdef Q_OS_LINUX
        appInfoMonitor.reset();
#endif
        invalidate(cancelled()); checkShutdown();
    }
};

DesktopCaptureAction::DesktopCaptureAction(QString executable, QString profile, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>(this, std::move(executable), std::move(profile)))
{ state_->bind(); }
DesktopCaptureAction::~DesktopCaptureAction()
{
    const auto state = state_;
    state->destroyed = true; state->owner.clear(); state->closing = true;
    state->deadline.stop(); state->connectionWatch.stop(); state->clearWatch();
#ifdef Q_OS_LINUX
    state->appInfoMonitor.reset();
#endif
    if (state->active) { state->active->challenge.clear(); state->active->stop.request_stop(); }
    // Workers retain only values and a stop token. Normal host shutdown awaits
    // shutdownFinished; abrupt owner deletion never joins a GUI-dependent job.
}

Failure DesktopCaptureAction::applyProbeEnvironment(Core::StartupRequest& request)
{
    if (!qEnvironmentVariableIsSet(proofEnvironment)) return {};
    const auto encoded = qgetenv(proofEnvironment);
    qunsetenv(proofEnvironment);
    const bool finish = request.action == Core::StartupAction::FinishCapture;
    request.action = Core::StartupAction::Invalid;
    if (!finish || encoded.size() != 32)
        return failed(Error::InvalidArgument, QStringLiteral("The desktop-action verification challenge is invalid"));
    const auto bytes = QByteArray::fromHex(encoded);
    if (bytes.size() != 16 ||
        bytes.toHex() != encoded.toLower() || QUuid::fromRfc4122(bytes).isNull())
        return failed(Error::InvalidArgument, QStringLiteral("The desktop-action verification challenge is invalid"));
    request.action = Core::StartupAction::VerifyCaptureAction;
    std::copy(bytes.begin(), bytes.end(), request.verification.begin());
    return {};
}
quint64 DesktopCaptureAction::verify()
{
    const auto state = state_;
    if (!state->ownerThread() || state->closing || state->destroyed) return 0;
    auto token = ++state->sequence; if (!token) token = ++state->sequence;
    if (state->active) { state->rejected(token, failed(Error::Busy, QStringLiteral("Desktop-action verification is already pending"))); return token; }
    if (!state->setAvailable(false)) return token;
    if (state->closing) return 0;
    state->connectionWatch.stop();
    state->clearWatch();
    if (!qGuiApp || qGuiApp->thread() != thread()) { state->rejected(token, unavailable()); return token; }
#ifdef Q_OS_LINUX
    const auto platform = QGuiApplication::platformName();
    const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QChar(u':'), Qt::SkipEmptyParts);
    if ((!platform.startsWith(QStringLiteral("wayland")) && platform != QStringLiteral("xcb")) ||
        !desktops.contains(QStringLiteral("GNOME"), Qt::CaseInsensitive) || !state->bus.isConnected()) {
        state->rejected(token, unavailable()); return token;
    }
    try {
        if (!state->appInfoMonitor) {
            const std::weak_ptr<State> weak = state;
            state->appInfoMonitor = std::make_unique<Platform::Linux::CaptureActionMonitor>([weak] {
                if (auto current = weak.lock()) current->invalidate(failed(Error::ChangedOnDisk,
                    QStringLiteral("The desktop application registry changed")));
            });
        }
        if (!state->appInfoMonitor->available()) { state->rejected(token, unavailable()); return token; }
        auto work = std::make_shared<State::Work>(); work->token = token;
        work->challenge = QUuid::createUuid().toRfc4122(); work->elapsed.start();
        state->active = work; state->deadline.start(); state->connectionWatch.start();
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.gnome.Shell"), QStringLiteral("/org/gnome/Shell"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        message.setAutoStartService(false);
        message.setArguments({QStringLiteral("org.gnome.Shell"), QStringLiteral("ShellVersion")});
        auto* watcher = new QDBusPendingCallWatcher(state->bus.asyncCall(message, 3000), this);
        work->shellCall = watcher;
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [state, work](QDBusPendingCallWatcher* completed) { state->shellReply(work, completed); });
        if (watcher->isFinished()) QTimer::singleShot(0, watcher, [state, work, watcher] { state->shellReply(work, watcher); });
    } catch (const std::bad_alloc&) {
        const auto error = failed(Error::Allocation, QStringLiteral("Desktop-action verification could not be allocated"));
        if (state->active) state->invalidate(error); else state->rejected(token, error);
    }
#else
    state->rejected(token, unavailable());
#endif
    return token;
}
void DesktopCaptureAction::cancel(quint64 token)
{
    const auto state = state_;
    if (state->ownerThread() && state->active && state->active->token == token) state->invalidate(cancelled());
}
bool DesktopCaptureAction::acceptVerification(const Core::StartupRequest& request)
{
    const auto state = state_;
    const auto work = state->active;
    if (!state->ownerThread() || state->closing || state->destroyed || !work || work->failure || work->received ||
        work->terminalQueued || work->elapsed.hasExpired(verificationDeadlineMs) || work->challenge.size() != 16 ||
        request.action != Core::StartupAction::VerifyCaptureAction || request.entry != Core::CaptureEntry::Capture ||
        request.oneShot || request.background || !std::equal(request.verification.begin(), request.verification.end(),
            reinterpret_cast<const std::uint8_t*>(work->challenge.constData()))) return false;
    work->received = true; work->challenge.clear();
    // Do not finish or emit host signals inside SingleInstance's admission
    // callback. It still has to write the matched UUID acknowledgment.
    QTimer::singleShot(0, this, [state] { state->consider(); });
    return true;
}
bool DesktopCaptureAction::available() const noexcept
{
#ifdef Q_OS_LINUX
    return state_->ready && !state_->closing && state_->bus.isConnected();
#else
    return false;
#endif
}
QString DesktopCaptureAction::actionName() const { return state_->label; }
void DesktopCaptureAction::shutdown()
{
    const auto state = state_;
    if (state->ownerThread()) state->shutdown();
}

} // namespace HuhuCapture::Desktop
