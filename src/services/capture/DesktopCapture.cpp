#include "services/capture/DesktopCapture.h"

#include "platform/DesktopSnapshot.h"
#include "services/media/MediaArchiveService.h"
#ifdef HUHUCAPTURE_WITH_PORTAL
#include "platform/linux/portal/PortalCapture.h"
#endif
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#ifdef Q_OS_WIN
#include <QtGui/qscreen_platform.h>
#endif
#include <algorithm>
#include <stop_token>

namespace HuhuCapture::Capture {
namespace {
struct ScreenAtRequest {
    QPointer<QScreen> screen;
    QString name;
    QRect geometry, available;
    qreal dpr{};
    quintptr nativeHandle{};
};
QVector<ScreenAtRequest> currentScreens()
{
    QVector<ScreenAtRequest> result;
    for (auto* screen : QGuiApplication::screens()) {
        ScreenAtRequest value{screen, screen->name(), screen->geometry(), screen->availableGeometry(), screen->devicePixelRatio()};
#ifdef Q_OS_WIN
        if (auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>())
            value.nativeHandle = reinterpret_cast<quintptr>(native->handle());
#endif
        result.push_back(std::move(value));
    }
    return result;
}
Failure bindNative(const Platform::NativeSnapshot& native, const QVector<ScreenAtRequest>& requested, Snapshot& output)
{
    if (native.failure) return native.failure;
    if (native.image.isNull() || native.image.size() != native.desktop.size() || requested.isEmpty() ||
        requested.size() != QGuiApplication::screens().size() || native.screens.size() != requested.size())
        return {Failure::Code::SourceChanged, 0, QStringLiteral("The desktop capture and screen layout do not agree")};
    for (const auto& old : requested) {
        if (!old.screen || old.screen->geometry() != old.geometry || old.screen->availableGeometry() != old.available ||
            old.screen->name() != old.name || old.screen->devicePixelRatio() != old.dpr)
            return {Failure::Code::SourceChanged, 0, QStringLiteral("The display layout changed while the screenshot was being prepared")};
#ifdef Q_OS_WIN
        const auto* current = old.screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (!current || !old.nativeHandle || reinterpret_cast<quintptr>(current->handle()) != old.nativeHandle)
            return {Failure::Code::SourceChanged, 0, QStringLiteral("The native Windows monitor identity changed during capture")};
#endif
    }
    Snapshot result;
    result.image = native.image; result.coordinates = CoordinateSpace::DesktopPixels;
    result.desktopOrigin = native.desktop.topLeft();
    result.cursor = native.cursor - native.desktop.topLeft();
    result.sourceName = QStringLiteral("Desktop");
    QSet<quintptr> bound;
    for (const auto& screen : requested) {
        const auto found = std::find_if(native.screens.begin(), native.screens.end(), [&](const auto& candidate) {
#ifdef Q_OS_WIN
            return candidate.nativeHandle && candidate.nativeHandle == screen.nativeHandle;
#else
            return candidate.id == screen.name;
#endif
        });
        if (found == native.screens.end() || screen.geometry.isEmpty() || found->pixels.isEmpty() ||
            !found->nativeHandle || bound.contains(found->nativeHandle))
            return {Failure::Code::SourceChanged, 0, QStringLiteral("A native monitor cannot be verified against its Qt screen")};
        bound.insert(found->nativeHandle);
        ScreenBinding binding;
        binding.screenId = static_cast<quint64>(result.screens.size()) + 1;
        binding.screen = screen.screen; binding.nativeId = found->id;
        binding.sourcePixels = found->pixels.translated(-native.desktop.topLeft());
        binding.logicalGeometry = screen.geometry; binding.logicalWorkArea = screen.available;
        binding.pixelsPerDipX = static_cast<qreal>(found->pixels.width()) / screen.geometry.width();
        binding.pixelsPerDipY = static_cast<qreal>(found->pixels.height()) / screen.geometry.height();
        binding.nativeRotation = found->rotation;
        result.screens.push_back(std::move(binding));
    }
    for (const auto& window : native.windows) {
        const auto source = window.intersected(native.desktop).translated(-native.desktop.topLeft());
        if (!source.isEmpty()) result.windows.push_back(source);
    }
    output = std::move(result); return {};
}
struct ReadResult { Snapshot snapshot; Failure failure; };
}

struct DesktopCapture::State {
    DesktopCapture* owner{};
    Capability capability;
    bool initialized{}, initializing{}, shuttingDown{}, portalMode{}, wasBusy{}, shutdownReported{};
    bool portalStopping{}, portalDrained{true};
    unsigned workers{}, terminals{};
    quint64 sequence{}, active{}, portalRequest{};
    std::stop_source stop;
    QThreadPool pool;
#ifdef HUHUCAPTURE_WITH_PORTAL
    Platform::Linux::PortalCapture* portal{};
#endif
    explicit State(DesktopCapture* value) : owner(value)
    { pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1); }
    bool busy() const
    { return initializing || active || portalRequest || workers || terminals || (portalStopping && !portalDrained); }
    void notifyBusy()
    {
        const bool now = busy(); const QPointer<DesktopCapture> lifetime(owner);
        if (now != wasBusy) { wasBusy = now; Q_EMIT owner->busyChanged(now); }
        if (lifetime && shuttingDown && !now && !shutdownReported) {
            shutdownReported = true; Q_EMIT owner->shutdownFinished();
        }
    }
    void finish(quint64 token, Snapshot snapshot, Failure failure)
    {
        if (!token || active != token) return;
        active = 0;
        if (!failure) { snapshot.sourceId = token; snapshot.generation = token; }
        ++terminals;
        // Even rejection/cancellation inside request() returns its accepted
        // token to the host before the terminal callback. Teardown also waits
        // for this queued delivery, not just the native worker's return.
        QTimer::singleShot(0, owner, [this, token, snapshot = std::move(snapshot), failure = std::move(failure)] {
            --terminals;
            const QPointer<DesktopCapture> lifetime(owner);
            Q_EMIT owner->finished(token, snapshot, failure);
            if (lifetime) notifyBusy();
        });
        notifyBusy();
    }
};

DesktopCapture::DesktopCapture(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
DesktopCapture::~DesktopCapture() { state_->stop.request_stop(); }
void DesktopCapture::initialize()
{
    if (QThread::currentThread() != thread() || state_->initialized || state_->initializing || state_->shuttingDown) return;
    state_->initializing = true;
    const auto qpa = QGuiApplication::platformName();
#ifdef Q_OS_WIN
    state_->capability = qpa == QStringLiteral("windows") ? Capability{CapabilityState::Available, {}} :
        Capability{CapabilityState::Unavailable, QStringLiteral("Windows desktop capture requires the native Windows platform plugin")};
#else
    const bool wayland = qpa.startsWith(QStringLiteral("wayland")) ||
        qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), ::Qt::CaseInsensitive) == 0 ||
        !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    state_->portalMode = wayland;
    if (wayland) {
#ifdef HUHUCAPTURE_WITH_PORTAL
        state_->portal = new Platform::Linux::PortalCapture(this);
        state_->portalDrained = false;
        connect(state_->portal, &Platform::Linux::PortalCapture::shutdownFinished, this, [this] {
            state_->portalDrained = true; state_->notifyBusy();
        });
        connect(state_->portal, &Platform::Linux::PortalCapture::initialized, this,
            [this](const Platform::Linux::PortalCapabilities& capabilities, const Platform::Linux::PortalFailure& failure) {
                state_->initializing = false; state_->initialized = true;
                if (state_->shuttingDown) { state_->notifyBusy(); return; }
                state_->capability = !failure && capabilities.screenshot.available() ?
                    Capability{CapabilityState::RequiresPermission, QStringLiteral("The desktop will ask which image may be shared")} :
                    Capability{CapabilityState::Unavailable, failure ? failure.message : capabilities.screenshot.failure.message};
                const QPointer<DesktopCapture> lifetime(this);
                state_->notifyBusy();
                if (!lifetime) return;
                Q_EMIT capabilityChanged(state_->capability);
                if (lifetime) Q_EMIT initialized(state_->capability);
            });
        connect(state_->portal, &Platform::Linux::PortalCapture::screenshotFinished, this,
            [this](quint64 request, const QUrl& uri, const Platform::Linux::PortalFailure& failure) {
                if (state_->portalRequest != request) return;
                const auto token = state_->active; state_->portalRequest = 0;
                if (!token) { state_->notifyBusy(); return; }
                if (failure) {
                    using Code = Platform::Linux::PortalFailure::Code;
                    const auto code = failure.code == Code::Cancelled || failure.code == Code::ParentDestroyed ? Failure::Code::Cancelled :
                        failure.code == Code::Denied ? Failure::Code::Denied : Failure::Code::Platform;
                    state_->finish(token, {}, {code, failure.nativeCode, failure.message}); return;
                }
                if (!uri.isLocalFile() || !uri.host().isEmpty() || uri.hasQuery() || uri.hasFragment() || uri.toLocalFile().isEmpty()) {
                    state_->finish(token, {}, {Failure::Code::InvalidSnapshot, 0, QStringLiteral("The Portal returned an unsupported screenshot URI")}); return;
                }
                auto* watcher = new QFutureWatcher<ReadResult>(this);
                ++state_->workers;
                connect(watcher, &QFutureWatcher<ReadResult>::finished, this, [this, watcher, token] {
                    ReadResult result;
                    try { result = watcher->result(); }
                    catch (...) { result.failure = {Failure::Code::Allocation, 0, QStringLiteral("The screenshot decoder could not complete its owned image")}; }
                    watcher->deleteLater();
                    --state_->workers;
                    if (state_->active != token) { state_->notifyBusy(); return; }
                    state_->finish(token, std::move(result.snapshot), std::move(result.failure));
                });
                watcher->setFuture(QtConcurrent::run(&state_->pool, [path = uri.toLocalFile(), stop = state_->stop.get_token()] {
                    ReadResult result;
                    const auto inspected = Media::inspectPng(path, {}, stop, &result.snapshot.image);
                    if (inspected.failure) {
                        result.failure = {inspected.failure.code == Media::Error::Cancelled ? Failure::Code::Cancelled : Failure::Code::InvalidSnapshot,
                            0, inspected.failure.detail};
                    } else {
                        result.snapshot.coordinates = CoordinateSpace::AuthorizedSourcePixels;
                        result.snapshot.cursor = {result.snapshot.image.width() / 2, result.snapshot.image.height() / 2};
                        result.snapshot.sourceName = QStringLiteral("Authorized screenshot");
                    }
                    // Portal owns this file. Decoding never removes it.
                    return result;
                }));
            });
        const QPointer<DesktopCapture> lifetime(this);
        state_->notifyBusy();
        if (lifetime) state_->portal->initialize();
        return;
#else
        state_->capability = {CapabilityState::Unavailable, QStringLiteral("This build has no Portal screenshot adapter")};
#endif
    } else state_->capability = qpa == QStringLiteral("xcb") ? Capability{CapabilityState::Available, {}} :
        Capability{CapabilityState::Unavailable, QStringLiteral("Desktop capture requires an X11 or Wayland desktop session")};
#endif
    state_->initializing = false; state_->initialized = true;
    const QPointer<DesktopCapture> lifetime(this);
    Q_EMIT capabilityChanged(state_->capability);
    if (lifetime) Q_EMIT initialized(state_->capability);
}
Capability DesktopCapture::capability() const
{
    if (state_->busy()) return {CapabilityState::Busy, QStringLiteral("A screenshot request is already active")};
    if (state_->initializing) return {CapabilityState::Busy, QStringLiteral("Desktop capabilities are being discovered")};
    return state_->capability;
}
bool DesktopCapture::busy() const noexcept { return state_->busy(); }
quint64 DesktopCapture::request(QWindow* parent)
{
    if (QThread::currentThread() != thread() || state_->shuttingDown || !state_->initialized || !capability().canRequest()) return 0;
    const auto token = ++state_->sequence ? state_->sequence : ++state_->sequence;
    state_->active = token; state_->stop = {};
    const QPointer<DesktopCapture> lifetime(this);
    state_->notifyBusy();
    if (!lifetime || state_->active != token) return token;
    if (state_->portalMode) {
#ifdef HUHUCAPTURE_WITH_PORTAL
        state_->portalRequest = state_->portal->requestScreenshot(parent, true);
        if (!state_->portalRequest) state_->finish(token, {}, {Failure::Code::Busy, 0, QStringLiteral("The Portal cannot accept another screenshot request")});
#else
        Q_UNUSED(parent);
#endif
        return token;
    }
    const auto screens = currentScreens();
    auto* watcher = new QFutureWatcher<Platform::NativeSnapshot>(this);
    ++state_->workers;
    connect(watcher, &QFutureWatcher<Platform::NativeSnapshot>::finished, this, [this, watcher, token, screens] {
        Platform::NativeSnapshot native;
        try { native = watcher->result(); }
        catch (...) { native.failure = {Failure::Code::Platform, 0, QStringLiteral("The native screenshot worker could not complete its owned image")}; }
        watcher->deleteLater();
        --state_->workers;
        if (state_->active != token) { state_->notifyBusy(); return; }
        Snapshot snapshot;
        auto failure = bindNative(native, screens, snapshot);
        state_->finish(token, std::move(snapshot), std::move(failure));
    });
    watcher->setFuture(QtConcurrent::run(&state_->pool, [stop = state_->stop.get_token()] {
        return Platform::captureNativeDesktop(stop);
    }));
    return token;
}
void DesktopCapture::cancel(quint64 token)
{
    if (!token || state_->active != token) return;
    state_->stop.request_stop();
#ifdef HUHUCAPTURE_WITH_PORTAL
    if (state_->portalRequest) state_->portal->cancelRequest(state_->portalRequest);
#endif
    state_->finish(token, {}, {Failure::Code::Cancelled, 0, {}});
}
void DesktopCapture::shutdown()
{
    const QPointer<DesktopCapture> lifetime(this);
    state_->shuttingDown = true;
    cancel(state_->active);
    if (!lifetime) return;
#ifdef HUHUCAPTURE_WITH_PORTAL
    if (state_->portal && !state_->portalStopping) {
        state_->portalStopping = true;
        state_->portal->shutdown();
        if (!lifetime) return;
    }
#endif
    state_->capability = {CapabilityState::Unavailable, QStringLiteral("The application is shutting down")};
    state_->notifyBusy();
}

} // namespace HuhuCapture::Capture
