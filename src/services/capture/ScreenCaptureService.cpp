#include "services/capture/ScreenCaptureService.h"
#include "platform/ScreenCaptureBackend.h"
#include "services/media/MediaArchiveService.h"

#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#ifdef Q_OS_WIN
#include <QtGui/qscreen_platform.h>
#endif
#ifdef Q_OS_LINUX
#include "platform/linux/portal/PortalCapture.h"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <new>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace HuhuCapture::Capture {
namespace {
std::atomic<quint64> sourceSequence{};
quint64 next(std::atomic<quint64>& sequence)
{ auto value = sequence.fetch_add(1) + 1; return value ? value : sequence.fetch_add(1) + 1; }
Failure failure(Failure::Code code, QString detail) { return {code, 0, std::move(detail)}; }
Failure cancelled() { return failure(Failure::Code::Cancelled, QStringLiteral("Screen capture was cancelled")); }
Failure allocation() { return failure(Failure::Code::Allocation, QStringLiteral("Screen capture exceeded available memory")); }
Failure changed() { return failure(Failure::Code::SourceChanged, QStringLiteral("The captured display configuration is no longer current")); }

struct ScreenState {
    QPointer<QScreen> screen;
    QString name;
    QRect geometry, available;
    qreal dpr{};
    Qt::ScreenOrientation orientation{};
    quintptr nativeId{};
};
QVector<ScreenState> screens()
{
    QVector<ScreenState> result;
    for (auto* screen : QGuiApplication::screens()) {
        ScreenState value{screen, screen->name(), screen->geometry(), screen->availableGeometry(),
            screen->devicePixelRatio(), screen->orientation(), 0};
#ifdef Q_OS_WIN
        auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native) value.nativeId = reinterpret_cast<quintptr>(native->handle());
#endif
        result.push_back(std::move(value));
    }
    return result;
}
bool sameScreens(const QVector<ScreenState>& before)
{
    const auto current = QGuiApplication::screens();
    if (current.size() != before.size()) return false;
    for (const auto& screen : before) {
        if (!screen.screen || !current.contains(screen.screen) || screen.screen->name() != screen.name ||
            screen.screen->geometry() != screen.geometry ||
            !qFuzzyCompare(screen.screen->devicePixelRatio(), screen.dpr) || screen.screen->orientation() != screen.orientation) return false;
#ifdef Q_OS_WIN
        auto* native = screen.screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (!native || reinterpret_cast<quintptr>(native->handle()) != screen.nativeId) return false;
#endif
    }
    return true;
}
Failure bindScreens(const QVector<ScreenState>& qt, const Platform::CaptureDesktop& native,
    QVector<ScreenBinding>& output)
{
    if (qt.isEmpty() || qt.size() != native.displays.size() || !sameScreens(qt)) return changed();
    QVector<ScreenBinding> result;
    QVector<quintptr> used;
    for (const auto& screen : qt) {
        const Platform::CaptureDisplay* match{};
        for (const auto& display : native.displays) {
#ifdef Q_OS_WIN
            const bool equal = screen.nativeId && screen.nativeId == display.nativeId;
#else
            const bool equal = screen.name == display.name;
#endif
            if (equal) { if (match) return changed(); match = &display; }
        }
        if (!match || used.contains(match->nativeId) || !screen.screen || !std::isfinite(screen.dpr) || screen.dpr <= 0 ||
            screen.geometry.isEmpty() || !native.bounds.contains(match->pixels)) return changed();
        // Qt rounds logical screen dimensions to integer DIPs. Bind by actual
        // native identity first; tolerate only that one-DIP rounding envelope.
        if (std::abs(screen.geometry.width() * screen.dpr - match->pixels.width()) > (std::max)(qreal(1), screen.dpr) ||
            std::abs(screen.geometry.height() * screen.dpr - match->pixels.height()) > (std::max)(qreal(1), screen.dpr)) return changed();
        used.push_back(match->nativeId);
        result.push_back({static_cast<quint64>(result.size()) + 1, screen.screen, match->name,
            match->pixels.translated(-native.bounds.topLeft()), screen.geometry,
            screen.available.isEmpty() ? QRectF(screen.geometry) : QRectF(screen.available),
            screen.dpr, screen.dpr, match->rotation});
    }
    output = std::move(result); return {};
}
#ifdef Q_OS_LINUX
Failure fromPortal(const Platform::Linux::PortalFailure& value)
{
    using Code = Platform::Linux::PortalFailure::Code;
    if (!value) return {};
    const auto code = value.code == Code::Cancelled || value.code == Code::ParentDestroyed ? Failure::Code::Cancelled :
        value.code == Code::Denied ? Failure::Code::Denied : value.code == Code::Busy ? Failure::Code::Busy :
        value.code == Code::Unavailable || value.code == Code::Unsupported ? Failure::Code::Unavailable : Failure::Code::Platform;
    return {code, value.nativeCode, value.message};
}
#endif
}

struct ScreenCaptureService::State {
    struct Worker { std::unique_ptr<Platform::ScreenCaptureBackend> backend; };
    struct WorkResult {
        Failure failure;
        Platform::NativeScreenshot capture;
        QImage authorized;
        QPoint cursor;
        bool requiresPortal{};
    };
    struct Work {
        std::stop_source stop;
        QPointer<QFutureWatcher<WorkResult>> watcher;
    };
    struct Operation {
        quint64 token{}, epoch{}, portalRequest{};
        std::stop_source stop;
        Failure cancellation;
        QVector<ScreenState> screens;
    };
    struct Current {
        quint64 source{}, generation{};
        Platform::CaptureDesktop desktop;
        QVector<ScreenState> screens;
    };
    struct Move { quint64 source{}, generation{}; QPoint target; };
    ScreenCaptureService* owner;
    QThreadPool pool;
    std::shared_ptr<Worker> worker{std::make_shared<Worker>()};
    std::vector<std::shared_ptr<Work>> work;
    std::shared_ptr<Operation> operation;
    std::optional<Current> current;
    std::optional<Move> pendingMove;
    std::shared_ptr<Work> cursorWork;
    CaptureCapabilities capability;
    std::atomic<quint64> sequence{};
    quint64 epoch{1};
    bool initializing{}, ready{}, stopping{}, resetting{}, reset{}, stopped{};
#ifdef Q_OS_LINUX
    std::unique_ptr<Platform::Linux::PortalCapture> portal;
    bool portalStopped{true};
#endif
    explicit State(ScreenCaptureService* value) : owner(value)
    { pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1); }
    bool ownerThread() const { return qGuiApp && owner->thread() == qGuiApp->thread() && QThread::currentThread() == owner->thread(); }
    void initialized(Failure reason = {})
    {
        initializing = false; ready = true;
        const auto copy = capability;
        QTimer::singleShot(0, owner, [target = owner, copy, reason] { Q_EMIT target->initialized(copy, reason); });
    }
    void changedCapabilities()
    {
        const auto copy = capability;
        QTimer::singleShot(0, owner, [target = owner, copy] { Q_EMIT target->capabilitiesChanged(copy); });
    }
    void invalidated()
    {
        if (++epoch == 0) ++epoch;
        if (operation && !operation->screens.isEmpty()) { operation->cancellation = changed(); operation->stop.request_stop(); }
        pendingMove.reset();
        if (cursorWork) cursorWork->stop.request_stop();
        if (!current) return;
        const auto source = current->source, generation = current->generation;
        current.reset();
        QTimer::singleShot(0, owner, [target = owner, source, generation] { Q_EMIT target->sourceInvalidated(source, generation, changed()); });
    }
    void screenChanged()
    {
        // Work-area or physical-DPI metadata alone does not move source pixels.
        // Do not abort a recording merely because a taskbar changes its strut.
        if ((current && !sameScreens(current->screens)) ||
            (operation && !operation->screens.isEmpty() && !sameScreens(operation->screens))) invalidated();
    }
    void watch(QScreen* screen)
    {
        QObject::connect(screen, &QScreen::geometryChanged, owner, [this] { screenChanged(); });
        QObject::connect(screen, &QScreen::physicalDotsPerInchChanged, owner, [this] { screenChanged(); });
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, owner, [this] { screenChanged(); });
        QObject::connect(screen, &QScreen::orientationChanged, owner, [this] { screenChanged(); });
        QObject::connect(screen, &QObject::destroyed, owner, [this] { screenChanged(); });
    }
    void finish(const std::shared_ptr<Operation>& pending, Snapshot snapshot, Failure reason)
    {
        if (operation != pending) return;
        if (pending->cancellation) reason = pending->cancellation;
        else if (stopping || pending->stop.stop_requested()) reason = cancelled();
        if (reason) snapshot = {};
        const auto token = pending->token;
        operation.reset();
        QTimer::singleShot(0, owner, [target = owner, token, snapshot = std::move(snapshot), reason] {
            Q_EMIT target->finished(token, snapshot, reason);
        });
        tryShutdown();
    }
    std::shared_ptr<Work> run(std::stop_source stop,
        std::function<WorkResult(Worker&, std::stop_token)> function,
        std::function<void(WorkResult)> complete)
    {
        auto pending = std::make_shared<Work>(); pending->stop = std::move(stop);
        auto watcher = std::make_unique<QFutureWatcher<WorkResult>>(owner);
        pending->watcher = watcher.get();
        work.push_back(pending);
        QObject::connect(watcher.get(), &QFutureWatcher<WorkResult>::finished, owner,
            [this, pending, complete = std::move(complete)]() mutable {
                auto* watcher = pending->watcher.data();
                WorkResult result;
                try { if (watcher) result = watcher->result(); else result.failure = cancelled(); }
                catch (const std::bad_alloc&) { result.failure = allocation(); }
                catch (const std::exception&) { result.failure = failure(Failure::Code::Platform, QStringLiteral("The native screenshot worker could not complete")); }
                if (watcher) watcher->deleteLater();
                std::erase(work, pending);
                complete(std::move(result));
                tryShutdown();
            });
        try {
            watcher->setFuture(QtConcurrent::run(&pool, [backend = worker, stop = pending->stop.get_token(), function = std::move(function)]() mutable {
                try { return function(*backend, stop); }
                catch (const std::bad_alloc&) { WorkResult result; result.failure = allocation(); return result; }
                catch (const std::exception&) { WorkResult result; result.failure = failure(Failure::Code::Platform, QStringLiteral("The native screenshot worker could not complete")); return result; }
            }));
        } catch (...) { std::erase(work, pending); throw; }
        watcher.release(); return pending;
    }
    void tryShutdown()
    {
        if (!stopping || stopped || resetting || !work.empty() || operation) return;
#ifdef Q_OS_LINUX
        if (!portalStopped) return;
#endif
        if (!reset) {
            resetting = true;
            // Last native resource release runs on the same permanent worker
            // that created its GDI DC / XCB connection, never the GUI thread.
            run({}, [](Worker& worker, std::stop_token) { worker.backend.reset(); return WorkResult{}; },
                [this](WorkResult) { resetting = false; reset = true; });
            return;
        }
        stopped = true;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
    }
    void nativeInitialized(WorkResult result)
    {
        if (stopping) { initializing = false; return; }
#ifdef Q_OS_LINUX
        if (result.requiresPortal) { initializePortal(); return; }
#endif
        if (result.failure) {
            capability.screenshot = {CapabilityState::Unavailable, result.failure.detail}; initialized(result.failure); return;
        }
#ifdef Q_OS_WIN
        capability.backend = QStringLiteral("windows-gdi");
#else
        capability.backend = QStringLiteral("x11-xcb");
#endif
        const Capability available{CapabilityState::Available, {}};
        capability.screenshot = capability.windowSelection = capability.desktopCoordinates = capability.cursorWarp = available;
        initialized();
    }
#ifdef Q_OS_LINUX
    void initializePortal()
    {
        capability.backend = QStringLiteral("xdg-desktop-portal");
        const Capability unmapped{CapabilityState::Unavailable,
            QStringLiteral("The authorized screenshot does not establish desktop coordinates")};
        capability.windowSelection = capability.desktopCoordinates = capability.cursorWarp = unmapped;
        portal = std::make_unique<Platform::Linux::PortalCapture>(); portalStopped = false;
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::initialized, owner,
            [this](const Platform::Linux::PortalCapabilities& capabilities, const Platform::Linux::PortalFailure& error) {
                if (stopping) return;
                auto reason = fromPortal(capabilities.screenshot.failure);
                if (!capabilities.screenshot.available() && !reason) reason = fromPortal(error);
                if (!capabilities.screenshot.available() && !reason)
                    reason = failure(Failure::Code::Unavailable, QStringLiteral("The desktop does not provide screenshot authorization"));
                capability.screenshot = {reason ? CapabilityState::Unavailable : CapabilityState::RequiresPermission, reason.detail};
                initialized(reason);
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::capabilitiesChanged, owner,
            [this](const Platform::Linux::PortalCapabilities& value) {
                if (stopping) return;
                const auto reason = fromPortal(value.screenshot.failure);
                capability.screenshot = {value.screenshot.available() ? CapabilityState::RequiresPermission : CapabilityState::Unavailable,
                    reason.detail};
                changedCapabilities();
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::screenshotFinished, owner,
            [this](quint64 id, const QUrl& uri, const Platform::Linux::PortalFailure& error) {
                const auto pending = operation;
                if (!pending || pending->portalRequest != id) return;
                pending->portalRequest = 0;
                if (error || pending->cancellation || stopping) { finish(pending, {}, fromPortal(error)); return; }
                if (!uri.isLocalFile() || uri.toLocalFile().isEmpty()) {
                    finish(pending, {}, failure(Failure::Code::InvalidSnapshot, QStringLiteral("The authorized screenshot has no readable local image"))); return;
                }
                try {
                    run(pending->stop, [path = uri.toLocalFile()](Worker&, std::stop_token stop) {
                        WorkResult result;
                        Media::Limits limits; limits.maximumFileBytes = limits.maximumImageBytes;
                        const auto checked = Media::inspectPng(path, limits, stop, &result.authorized);
                        if (checked.failure) result.failure = failure(checked.failure.code == Media::Error::Cancelled ? Failure::Code::Cancelled :
                            checked.failure.code == Media::Error::Budget ? Failure::Code::Allocation : Failure::Code::Platform, checked.failure.detail);
                        if (!result.failure) result.authorized.setDevicePixelRatio(1);
                        return result;
                    }, [this, pending](WorkResult result) {
                        Snapshot snapshot;
                        if (!result.failure) {
                            snapshot.image = std::move(result.authorized); snapshot.sourceId = next(sourceSequence); snapshot.generation = epoch;
                            snapshot.coordinates = CoordinateSpace::AuthorizedSourcePixels;
                            snapshot.cursor = {snapshot.image.width() / 2, snapshot.image.height() / 2};
                            snapshot.sourceName = QStringLiteral("xdg-desktop-portal");
                        }
                        finish(pending, std::move(snapshot), result.failure);
                    });
                } catch (const std::bad_alloc&) { finish(pending, {}, allocation()); }
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::shutdownFinished, owner, [this] {
            portalStopped = true; tryShutdown();
        });
        portal->initialize();
    }
#endif
    void startCursor()
    {
        if (cursorWork || !pendingMove || stopping) return;
        const auto move = *pendingMove; pendingMove.reset();
        if (!owner->isCurrent(move.source, move.generation)) {
            postCursor(move, {}, changed()); return;
        }
        const auto desktop = current->desktop;
        try {
            cursorWork = run({}, [desktop, target = move.target + desktop.bounds.topLeft()](Worker& worker, std::stop_token stop) {
                WorkResult result;
                if (!worker.backend) result.failure = failure(Failure::Code::Unavailable, QStringLiteral("There is no active native cursor backend"));
                else result.failure = worker.backend->moveCursor(desktop, target, result.cursor, stop);
                return result;
            }, [this, move, origin = desktop.bounds.topLeft()](WorkResult result) {
                cursorWork.reset();
                if (!stopping) {
                    if (!owner->isCurrent(move.source, move.generation)) result.failure = changed();
                    postCursor(move, result.cursor - origin, result.failure);
                    startCursor();
                }
            });
        } catch (const std::bad_alloc&) { postCursor(move, {}, allocation()); }
    }
    void postCursor(const Move& move, QPoint point, Failure error)
    {
        QTimer::singleShot(0, owner, [target = owner, move, point, error] {
            Q_EMIT target->cursorMoved(move.source, move.generation, move.target, point, error);
        });
    }
};

ScreenCaptureService::ScreenCaptureService(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this))
{
    if (qGuiApp && thread() == qGuiApp->thread()) {
        for (auto* screen : QGuiApplication::screens()) state_->watch(screen);
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) { state_->watch(screen); state_->screenChanged(); });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { state_->screenChanged(); });
    }
}
ScreenCaptureService::~ScreenCaptureService()
{
    state_->stopping = true;
    for (const auto& work : state_->work) work->stop.request_stop();
    if (state_->operation) state_->operation->stop.request_stop();
#ifdef Q_OS_LINUX
    state_->portal.reset();
#endif
    // Normal application shutdown awaits shutdownFinished. An immediate owner
    // destruction still cannot free a backend while its worker is reading it.
    if (!state_->reset) {
        state_->pool.start([worker = state_->worker] { worker->backend.reset(); });
    }
    state_->pool.waitForDone();
}
void ScreenCaptureService::initialize()
{
    if (!state_->ownerThread()) {
        const auto error = failure(Failure::Code::Unavailable, QStringLiteral("Screen capture initialization requires the application GUI thread"));
        QTimer::singleShot(0, this, [this, error] { Q_EMIT initialized({}, error); }); return;
    }
    if (state_->stopping || state_->initializing) return;
    if (state_->ready) { state_->initialized(); return; }
    state_->initializing = true;
    const auto platform = QGuiApplication::platformName();
#ifdef Q_OS_LINUX
    if (platform.startsWith(QStringLiteral("wayland")) || (platform == QStringLiteral("xcb") &&
            (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || qEnvironmentVariable("XDG_SESSION_TYPE") == QStringLiteral("wayland")))) {
        state_->initializePortal(); return;
    }
    const bool native = platform == QStringLiteral("xcb");
#else
    const bool native = platform == QStringLiteral("windows");
#endif
    if (!native) {
        const auto error = failure(Failure::Code::Unavailable, QStringLiteral("This Qt backend cannot capture a real desktop"));
        state_->capability.screenshot = {CapabilityState::Unavailable, error.detail}; state_->initialized(error); return;
    }
    try {
        state_->run({}, [](State::Worker& worker, std::stop_token stop) {
            State::WorkResult result;
            if (stop.stop_requested()) { result.failure = cancelled(); return result; }
            worker.backend = std::make_unique<Platform::ScreenCaptureBackend>();
            result.failure = worker.backend->initialize(); result.requiresPortal = worker.backend->requiresPortal();
            if (result.requiresPortal || result.failure) worker.backend.reset();
            return result;
        }, [this](State::WorkResult result) { state_->nativeInitialized(std::move(result)); });
    } catch (const std::bad_alloc&) {
        const auto error = allocation(); state_->capability.screenshot = {CapabilityState::Unavailable, error.detail}; state_->initialized(error);
    }
}
CaptureCapabilities ScreenCaptureService::capabilities() const { return state_->capability; }
bool ScreenCaptureService::busy() const noexcept { return state_->initializing || state_->operation || state_->stopping; }
bool ScreenCaptureService::isCurrent(quint64 sourceId, quint64 generation) const
{
    return state_->ownerThread() && !state_->stopping && state_->current && sourceId && generation &&
        state_->current->source == sourceId && state_->current->generation == generation && generation == state_->epoch &&
        sameScreens(state_->current->screens);
}
quint64 ScreenCaptureService::request(QWindow* parent)
{
    if (state_->ownerThread() && state_->stopping) return 0;
    const auto token = next(state_->sequence);
    auto reject = [this, token](Failure reason) {
        QTimer::singleShot(0, this, [this, token, reason] { Q_EMIT finished(token, {}, reason); });
    };
    if (!state_->ownerThread()) { reject(failure(Failure::Code::Unavailable, QStringLiteral("Desktop capture requires the application GUI thread"))); return token; }
    if (state_->operation || state_->initializing) { reject(failure(Failure::Code::Busy, QStringLiteral("Another screen capture is still finishing"))); return token; }
    if (state_->stopping || !state_->ready || !state_->capability.screenshot.canRequest()) {
        reject(failure(Failure::Code::Unavailable, state_->capability.screenshot.reason.isEmpty() ?
            QStringLiteral("Screen capture is not currently available") : state_->capability.screenshot.reason)); return token;
    }
    try {
        state_->invalidated();
        auto pending = std::make_shared<State::Operation>(); pending->token = token; pending->epoch = state_->epoch;
        state_->operation = pending;
#ifdef Q_OS_LINUX
        if (state_->portal) { pending->portalRequest = state_->portal->requestScreenshot(parent, true); return token; }
#else
        Q_UNUSED(parent);
#endif
        pending->screens = screens();
        state_->run(pending->stop, [](State::Worker& worker, std::stop_token stop) {
            State::WorkResult result;
            if (!worker.backend) result.failure = failure(Failure::Code::Unavailable, QStringLiteral("The native screenshot backend is unavailable"));
            else { result.capture = worker.backend->capture(stop); result.failure = result.capture.failure; }
            return result;
        }, [this, pending](State::WorkResult result) {
            Snapshot snapshot;
            if (!result.failure && (pending->epoch != state_->epoch || !sameScreens(pending->screens))) result.failure = changed();
            if (!result.failure) result.failure = bindScreens(pending->screens, result.capture.desktop, snapshot.screens);
            if (!result.failure && !pending->cancellation && !pending->stop.stop_requested() && !state_->stopping) {
                snapshot.image = std::move(result.capture.image); snapshot.sourceId = next(sourceSequence); snapshot.generation = state_->epoch;
                snapshot.coordinates = CoordinateSpace::DesktopPixels; snapshot.desktopOrigin = result.capture.desktop.bounds.topLeft();
                snapshot.cursor = result.capture.desktop.cursor - *snapshot.desktopOrigin; snapshot.sourceName = state_->capability.backend;
                snapshot.windows.reserve(result.capture.desktop.windows.size());
                for (const auto& window : result.capture.desktop.windows) snapshot.windows.push_back(window.translated(-*snapshot.desktopOrigin));
                state_->current = State::Current{snapshot.sourceId, snapshot.generation, std::move(result.capture.desktop), pending->screens};
            }
            state_->finish(pending, std::move(snapshot), result.failure);
        });
    } catch (const std::bad_alloc&) {
        if (state_->operation && state_->operation->token == token) state_->finish(state_->operation, {}, allocation());
        else reject(allocation());
    }
    return token;
}
void ScreenCaptureService::cancel(quint64 token)
{
    if (QThread::currentThread() != thread()) { QTimer::singleShot(0, this, [this, token] { cancel(token); }); return; }
    if (!state_->operation || state_->operation->token != token) return;
    state_->operation->cancellation = cancelled(); state_->operation->stop.request_stop();
#ifdef Q_OS_LINUX
    if (state_->portal && state_->operation->portalRequest) state_->portal->cancelRequest(state_->operation->portalRequest);
#endif
}
void ScreenCaptureService::moveCursor(quint64 sourceId, quint64 generation, QPoint source)
{
    if (!state_->ownerThread()) return;
    if (!isCurrent(sourceId, generation)) { state_->postCursor({sourceId, generation, source}, {}, changed()); return; }
    if (!QRect(QPoint(), state_->current->desktop.bounds.size()).contains(source)) {
        state_->postCursor({sourceId, generation, source}, {}, failure(Failure::Code::InvalidSnapshot, QStringLiteral("The cursor target is outside the frozen source"))); return;
    }
    state_->pendingMove = State::Move{sourceId, generation, source}; state_->startCursor();
}
void ScreenCaptureService::shutdown()
{
    if (QThread::currentThread() != thread()) { QTimer::singleShot(0, this, [this] { shutdown(); }); return; }
    if (state_->stopping) return;
    state_->stopping = true; state_->pendingMove.reset(); state_->current.reset();
    for (const auto& work : state_->work) work->stop.request_stop();
    if (state_->operation) { state_->operation->cancellation = cancelled(); state_->operation->stop.request_stop(); }
#ifdef Q_OS_LINUX
    if (state_->portal) state_->portal->shutdown();
#endif
    state_->tryShutdown();
}
} // namespace HuhuCapture::Capture
