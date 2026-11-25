#include "services/capture/LiveCaptureSource.h"

#include "capture/SourceIdentity.h"
#include "services/media/FrameNormalizer.h"
#include <QColorSpace>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#ifdef Q_OS_LINUX
#include "platform/linux/portal/PortalCapture.h"
#include "platform/linux/pipewire/PipeWireCapture.h"
#endif
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace HuhuCapture::Capture {
namespace {
using Activity = Core::LiveSourceActivity;
using End = Core::FrameSourceEnd;
using Terminal = Core::FrameSourceTerminal;
Failure fail(Failure::Code code, QString detail, qint64 nativeCode = 0)
{ return {code, nativeCode, std::move(detail)}; }
Failure unavailable()
{ return fail(Failure::Code::Unavailable, QStringLiteral("This desktop cannot provide an authorized live source")); }
Failure cancelled()
{ return fail(Failure::Code::Cancelled, QStringLiteral("Live capture was cancelled")); }
Failure allocation()
{ return fail(Failure::Code::Allocation, QStringLiteral("The live source could not be prepared within available memory")); }
Failure fromTerminal(const Terminal& value)
{
    Failure::Code code;
    switch (value.reason) {
    case End::Stopped: return {};
    case End::Cancelled: code = Failure::Code::Cancelled; break;
    case End::PermissionDenied: code = Failure::Code::Denied; break;
    case End::SourceClosed: code = Failure::Code::SourceChanged; break;
    case End::UnsupportedFormat: code = Failure::Code::Unavailable; break;
    case End::BudgetExceeded: code = Failure::Code::Allocation; break;
    case End::PlatformFailure: code = Failure::Code::Platform; break;
    }
    return {code, value.nativeCode, QString::fromStdString(value.detail)};
}
#ifdef Q_OS_LINUX
Failure fromPortal(const Platform::Linux::PortalFailure& value)
{
    using Code = Platform::Linux::PortalFailure::Code;
    if (!value) return {};
    const auto code = value.code == Code::Cancelled || value.code == Code::ParentDestroyed ? Failure::Code::Cancelled :
        value.code == Code::Denied ? Failure::Code::Denied : value.code == Code::Unavailable || value.code == Code::Unsupported ?
        Failure::Code::Unavailable : value.code == Code::Busy ? Failure::Code::Busy : Failure::Code::Platform;
    return fail(code, value.message, value.nativeCode);
}
Terminal fromPipeWire(const Platform::Linux::PipeWireFailure& value)
{
    using Code = Platform::Linux::PipeWireFailure::Code;
    const auto code = value.code == Code::Budget || value.code == Code::Allocation ? End::BudgetExceeded :
        value.code == Code::UnsupportedFormat || value.code == Code::UnsupportedMemory ? End::UnsupportedFormat :
        value.code == Code::PauseTimeout ? End::SourceClosed : End::PlatformFailure;
    return {code, value.nativeCode, value.message};
}
#endif

QColorSpace imageColorSpace(const Core::FrameColor& color)
{
    // Metadata describes the source, not a conversion. Leave unspecified
    // primaries/transfer untagged; do not relabel them as measured sRGB.
    QColorSpace::Primaries primaries;
    switch (color.primaries) {
    case Core::ColorPrimaries::Bt709: primaries = QColorSpace::Primaries::SRgb; break;
    case Core::ColorPrimaries::DisplayP3: primaries = QColorSpace::Primaries::DciP3D65; break;
    case Core::ColorPrimaries::AdobeRgb: primaries = QColorSpace::Primaries::AdobeRgb; break;
    case Core::ColorPrimaries::Bt2020: primaries = QColorSpace::Primaries::Bt2020; break;
    default: return {};
    }
    switch (color.transfer) {
    case Core::ColorTransfer::Srgb: return {primaries, QColorSpace::TransferFunction::SRgb};
    case Core::ColorTransfer::Linear: return {primaries, QColorSpace::TransferFunction::Linear};
    case Core::ColorTransfer::Gamma18: return {primaries, 1.8f};
    case Core::ColorTransfer::Gamma20: return {primaries, 2.0f};
    case Core::ColorTransfer::Gamma22: return {primaries, 2.2f};
    case Core::ColorTransfer::Gamma28: return {primaries, 2.8f};
    case Core::ColorTransfer::Bt2020_10:
    case Core::ColorTransfer::Bt2020_12: return {primaries, QColorSpace::TransferFunction::Bt2020};
    default: return {};
    }
}
}

struct LiveCaptureSource::State {
    struct Identity {
        Core::FrameFormat format;
        quint64 generation{};
        Core::FrameCrop crop;
        Core::FrameTransform transform{};
        bool operator==(const Identity&) const = default;
    };
    struct Data {
        std::shared_ptr<Core::FrameQueue> queue{std::make_shared<Core::FrameQueue>(2)};
        std::shared_ptr<std::atomic<Activity>> activity{std::make_shared<std::atomic<Activity>>(Activity::Starting)};
        std::stop_source stop;
        std::atomic_bool valid{true};
        std::mutex mutex;
        std::optional<Core::FrameFormat> format;
        quint64 formatGeneration{};
        std::optional<Identity> identity;
        std::optional<Terminal> terminal;
        Core::FrameLease firstFrame;

        void close(Terminal value, bool discard = false)
        {
            {
                std::lock_guard lock(mutex);
                if (!terminal) terminal = value;
                value = *terminal;
            }
            valid.store(false, std::memory_order_release);
            queue->close(std::move(value), discard);
        }
        Terminal outcome()
        {
            std::lock_guard lock(mutex);
            return terminal.value_or(Terminal{});
        }
        bool changedFormat(const Core::FrameFormat& value, quint64 generation)
        {
            bool changed;
            {
                std::lock_guard lock(mutex);
                changed = format && (*format != value || formatGeneration != generation);
                if (!format) { format = value; formatGeneration = generation; }
            }
            if (changed) {
                close({End::SourceClosed, 0, "The authorized source changed its negotiated pixel format"});
                return false;
            }
            return true;
        }
        bool frame(Core::FrameLease value)
        {
            if (!value || stop.stop_requested() || !valid.load(std::memory_order_acquire)) return false;
            const auto& format = value.format();
            const auto& metadata = value.metadata();
            const Identity next{format, value.formatGeneration(), metadata.crop.value_or(Core::FrameCrop{0, 0,
                static_cast<std::uint32_t>(format.width), static_cast<std::uint32_t>(format.height)}), metadata.transform};
            bool changed;
            {
                std::lock_guard lock(mutex);
                changed = identity && *identity != next;
                if (!identity) identity = next;
            }
            if (changed) {
                close({End::SourceClosed, 0, "The authorized source changed its crop or orientation"});
                return false;
            }
            return queue->push(std::move(value));
        }
    };
    struct Operation {
        quint64 token{}, sourceId{}, generation{1}, portalRequest{};
        LiveCaptureOptions options;
        QPointer<QWindow> parent;
        std::shared_ptr<Data> data;
        QSize extent;
        bool producerActive{}, previewActive{}, consumer{}, claimed{};
        bool preparationSent{}, sourceReady{}, closureSent{}, ending{}, abandon{};
        Failure failure;
        Activity observed{Activity::Starting};
    };
    struct Preview { QImage image; Failure failure; };

    LiveCaptureSource* owner;
    QThreadPool pool;
    QTimer monitor;
    Capability available;
    std::shared_ptr<Operation> operation;
    quint64 sequence{};
    unsigned deliveries{};
    bool initializing{}, ready{}, shuttingDown{}, shutdownReported{};
#ifdef Q_OS_LINUX
    std::unique_ptr<Platform::Linux::PortalCapture> portal;
    quint32 sourceTypes{}, cursorModes{};
    bool portalClosing{}, portalStopped{true};
#endif

    explicit State(LiveCaptureSource* value) : owner(value)
    {
        pool.setMaxThreadCount(2); pool.setExpiryTimeout(-1);
        monitor.setInterval(50);
        QObject::connect(&monitor, &QTimer::timeout, owner, [this] {
            const auto pending = operation;
            if (!pending || !pending->data || !pending->sourceReady) return;
            const auto activity = pending->data->activity->load(std::memory_order_acquire);
            if (pending->observed == activity) return;
            pending->observed = activity;
            // No frame traverses the GUI. Only actual native activity changes
            // are observed, and a quiet Streaming source remains Streaming.
            post([id = pending->sourceId, generation = pending->generation, activity](LiveCaptureSource* target) {
                Q_EMIT target->activityChanged(id, generation, activity);
            });
        });
    }
    bool ownerThread() const
    { return qGuiApp && owner->thread() == qGuiApp->thread() && QThread::currentThread() == owner->thread(); }
    void post(std::function<void(LiveCaptureSource*)> function)
    {
        ++deliveries;
        QTimer::singleShot(0, owner, [this, function = std::move(function)] {
            const QPointer<LiveCaptureSource> alive(owner);
            function(owner);
            if (!alive) return;
            --deliveries;
            checkDone();
        });
    }
    void prepared(const std::shared_ptr<Operation>& pending, QImage image, Failure error)
    {
        if (pending->preparationSent) return;
        pending->preparationSent = true;
        Snapshot snapshot;
        if (!error && !pending->ending && !shuttingDown) {
            snapshot.image = std::move(image); snapshot.sourceId = pending->sourceId; snapshot.generation = pending->generation;
            snapshot.coordinates = CoordinateSpace::AuthorizedSourcePixels;
            snapshot.cursor = {snapshot.image.width() / 2, snapshot.image.height() / 2};
            snapshot.sourceName = QStringLiteral("xdg-desktop-portal-screencast");
            pending->extent = snapshot.image.size(); pending->sourceReady = true;
        } else {
            if (!error) error = pending->failure ? pending->failure : cancelled();
            if (!pending->failure) pending->failure = error;
            pending->ending = true;
        }
        post([token = pending->token, snapshot = std::move(snapshot), error](LiveCaptureSource* target) {
            Q_EMIT target->prepared(token, snapshot, error);
        });
    }
    void checkDone()
    {
        const auto pending = operation;
        if (!pending && !shuttingDown) return;
        if (pending && !pending->ending) return;
        if (pending && (pending->producerActive || pending->previewActive || pending->consumer)) return;
        if (pending && !pending->preparationSent) prepared(pending, {}, pending->failure ? pending->failure : cancelled());
#ifdef Q_OS_LINUX
        if (portal && !portalStopped) {
            if (!portalClosing) { portalClosing = true; portal->shutdown(); }
            return;
        }
#endif
        if (pending) {
            if (pending->data) pending->data->queue->close(pending->data->outcome(), true);
            monitor.stop();
            operation.reset();
            post([token = pending->token, error = pending->failure](LiveCaptureSource* target) {
                Q_EMIT target->stopped(token, error);
            });
        }
        if (shuttingDown && !initializing && deliveries == 0 && !shutdownReported) {
            shutdownReported = true;
            QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
        }
    }
    void stopOperation(const std::shared_ptr<Operation>& pending, bool abandon, Failure error = {})
    {
        pending->ending = true;
        pending->abandon = pending->abandon || abandon;
        if (error && !pending->failure) pending->failure = error;
        if (pending->data) {
            if (abandon) pending->data->close({End::Cancelled, 0, "Live capture was cancelled"}, true);
            pending->data->stop.request_stop();
        }
#ifdef Q_OS_LINUX
        if (portal && pending->portalRequest) portal->cancelRequest(pending->portalRequest);
#endif
        checkDone();
    }
    void initialized(Failure error)
    {
        initializing = false; ready = true;
        const auto capability = available;
        post([capability, error](LiveCaptureSource* target) { Q_EMIT target->initialized(capability, error); });
        if (operation && !operation->ending) {
            if (error || !available.canRequest()) {
                operation->failure = error ? error : unavailable();
                prepared(operation, {}, operation->failure);
            } else begin();
        }
        if (shuttingDown || (operation && operation->ending)) checkDone();
    }
    void initialize()
    {
        if (shuttingDown || initializing) return;
        if (ready) {
            const auto capability = available;
            post([capability](LiveCaptureSource* target) { Q_EMIT target->initialized(capability, {}); });
            if (operation && !operation->ending) begin();
            return;
        }
        initializing = true;
#ifdef Q_OS_LINUX
        const auto qpa = QGuiApplication::platformName();
        if (qpa != QStringLiteral("xcb") && !qpa.startsWith(QStringLiteral("wayland"))) {
            available = {CapabilityState::Unavailable, unavailable().detail}; initialized(unavailable()); return;
        }
        portal = std::make_unique<Platform::Linux::PortalCapture>(); portalStopped = false; portalClosing = false;
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::initialized, owner,
            [this](const Platform::Linux::PortalCapabilities& caps, const Platform::Linux::PortalFailure& error) {
                sourceTypes = caps.sourceTypes; cursorModes = caps.cursorModes;
                auto reason = fromPortal(caps.screenCast.failure);
                if (!caps.screenCast.available() && !reason) reason = fromPortal(error);
                if (!caps.screenCast.available() && !reason) reason = unavailable();
                if (!reason && (!(sourceTypes & 3u) || !(cursorModes & 1u)))
                    reason = fail(Failure::Code::Unavailable,
                        QStringLiteral("The portal cannot provide a screen or window without an embedded cursor"));
                available = {reason ? CapabilityState::Unavailable : CapabilityState::RequiresPermission, reason.detail};
                initialized(reason);
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::capabilitiesChanged, owner,
            [this](const Platform::Linux::PortalCapabilities& caps) {
                sourceTypes = caps.sourceTypes; cursorModes = caps.cursorModes;
                auto error = fromPortal(caps.screenCast.failure);
                if ((!caps.screenCast.available() || !(sourceTypes & 3u) || !(cursorModes & 1u)) && !error) error = unavailable();
                available = {error ? CapabilityState::Unavailable : CapabilityState::RequiresPermission, error.detail};
                const auto capability = available;
                post([capability](LiveCaptureSource* target) { Q_EMIT target->capabilityChanged(capability); });
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::screenCastFinished, owner,
            [this](quint64 request, const QVector<Platform::Linux::PortalStream>& streams, const QString&,
                const Platform::Linux::PortalFailure& error) {
                const auto pending = operation;
                if (!pending || pending->portalRequest != request) return;
                pending->portalRequest = 0;
                if (pending->ending || shuttingDown || error) {
                    if (!pending->failure) pending->failure = error ? fromPortal(error) : cancelled();
                    prepared(pending, {}, pending->failure); checkDone(); return;
                }
                if (streams.size() != 1) {
                    prepared(pending, {}, fail(Failure::Code::SourceChanged, QStringLiteral("Exactly one authorized live source is required")));
                    checkDone(); return;
                }
                Platform::Linux::UniqueFd remote(portal->takePipeWireFd());
                if (!remote) {
                    prepared(pending, {}, fail(Failure::Code::Platform, QStringLiteral("The authorized source has no PipeWire remote")));
                    checkDone(); return;
                }
                try { startProducer(pending, std::move(remote), streams.front()); }
                catch (const std::bad_alloc&) { stopOperation(pending, true, allocation()); }
                catch (const std::exception&) {
                    stopOperation(pending, true, fail(Failure::Code::Platform, QStringLiteral("The authorized source worker could not start")));
                }
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::screenCastClosed, owner,
            [this](const Platform::Linux::PortalFailure& error) {
                const auto pending = operation;
                if (!pending || pending->ending) return;
                const auto reason = error ? fromPortal(error) : fail(Failure::Code::SourceChanged, QStringLiteral("The authorized source was closed"));
                if (pending->data) pending->data->close({reason.code == Failure::Code::Denied ? End::PermissionDenied : End::SourceClosed,
                    reason.nativeCode, reason.detail.toStdString()});
                stopOperation(pending, false, reason);
            });
        QObject::connect(portal.get(), &Platform::Linux::PortalCapture::shutdownFinished, owner, [this] {
            portalStopped = true; initializing = false; ready = false;
            // Release the already-drained controller on a subsequent Qt turn,
            // never delete a sender during its shutdown signal dispatch.
            QTimer::singleShot(0, owner, [this] {
                if (portal && portalStopped) portal.reset();
                checkDone();
            });
        });
        portal->initialize();
#else
        available = {CapabilityState::Unavailable, unavailable().detail}; initialized(unavailable());
#endif
    }
    void begin()
    {
        const auto pending = operation;
        if (!pending || pending->ending || pending->portalRequest || shuttingDown) return;
        if (!available.canRequest()) { prepared(pending, {}, unavailable()); checkDone(); return; }
#ifdef Q_OS_LINUX
        if (!portal || portalClosing || !ready) return;
        Platform::Linux::ScreenCastOptions options;
        // The portal probe already read these official bitmasks. Request only
        // source kinds it can actually offer; an explicit unsupported-only
        // choice fails rather than silently selecting another kind.
        options.monitors = pending->options.monitors && (sourceTypes & 1u);
        options.windows = pending->options.windows && (sourceTypes & 2u);
        if ((!options.monitors && !options.windows) || !(cursorModes & 1u)) {
            prepared(pending, {}, fail(Failure::Code::Unavailable,
                QStringLiteral("The portal cannot provide the requested live source without an embedded cursor")));
            checkDone(); return;
        }
        options.multiple = false; options.cursor = Platform::Linux::ScreenCastOptions::Cursor::Hidden;
        options.persistence = Platform::Linux::ScreenCastOptions::Persistence::None;
        pending->portalRequest = portal->requestScreenCast(pending->parent, options);
        if (!pending->portalRequest) { prepared(pending, {}, unavailable()); checkDone(); }
#else
        prepared(pending, {}, unavailable()); checkDone();
#endif
    }
#ifdef Q_OS_LINUX
    void startProducer(const std::shared_ptr<Operation>& pending, Platform::Linux::UniqueFd remote,
        const Platform::Linux::PortalStream& stream)
    {
        pending->data = std::make_shared<Data>();
        auto watcher = std::make_unique<QFutureWatcher<Terminal>>(owner);
        QObject::connect(watcher.get(), &QFutureWatcher<Terminal>::finished, owner,
            [this, pending, watcher = watcher.get()] {
                watcher->deleteLater(); pending->producerActive = false;
                Terminal terminal;
                try { terminal = watcher->result(); }
                catch (const std::bad_alloc&) { terminal = {End::BudgetExceeded, 0, "The authorized source worker ran out of memory"}; }
                catch (const std::exception&) { terminal = {End::PlatformFailure, 0, "The authorized source worker could not complete"}; }
                pending->data->activity->store(Activity::Closed, std::memory_order_release);
                pending->data->close(terminal, pending->abandon);
                pending->ending = true;
                if (!pending->failure) pending->failure = fromTerminal(pending->data->outcome());
                if (pending->sourceReady && !pending->closureSent) {
                    pending->closureSent = true;
                    post([id = pending->sourceId, generation = pending->generation, error = pending->failure](LiveCaptureSource* target) {
                        Q_EMIT target->sourceClosed(id, generation, error);
                    });
                }
                checkDone();
            });
        Platform::Linux::PipeWireCaptureOptions options;
        options.pool = pending->options.pool; options.maximumFrameRate = pending->options.maximumFrameRate;
        options.sessionId = pending->token; options.sourceId = pending->sourceId;
        const Platform::Linux::PipeWireTarget target{stream.nodeId, stream.pipeWireSerial};
        pending->producerActive = true;
        try {
            watcher->setFuture(QtConcurrent::run(&pool, [data = pending->data, remote = std::move(remote), options, target]() mutable {
                Platform::Linux::PipeWireCaptureCallbacks callbacks;
                callbacks.formatChanged = [data](const Core::FrameFormat& format, std::uint64_t generation) {
                    return data->changedFormat(format, generation);
                };
                callbacks.frame = [data](Core::FrameLease frame) { return data->frame(std::move(frame)); };
                callbacks.activityChanged = [data](bool active) {
                    data->activity->store(active ? Activity::Streaming : Activity::Paused, std::memory_order_release);
                };
                const auto result = Platform::Linux::runPipeWireCapture(std::move(remote), target, options,
                    std::move(callbacks), data->stop.get_token());
                // Only here has the private native loop actually torn down.
                data->activity->store(Activity::Closed, std::memory_order_release);
                data->close(result.failure ? fromPipeWire(*result.failure) : Terminal{});
                return data->outcome();
            }));
        } catch (...) { pending->producerActive = false; throw; }
        watcher.release();
        startPreview(pending);
        monitor.start();
    }
#endif
    void startPreview(const std::shared_ptr<Operation>& pending)
    {
        auto watcher = std::make_unique<QFutureWatcher<Preview>>(owner);
        QObject::connect(watcher.get(), &QFutureWatcher<Preview>::finished, owner,
            [this, pending, watcher = watcher.get()] {
                watcher->deleteLater(); pending->previewActive = false;
                Preview preview;
                try { preview = watcher->result(); }
                catch (const std::bad_alloc&) { preview.failure = allocation(); }
                catch (const std::exception&) {
                    preview.failure = fail(Failure::Code::Platform, QStringLiteral("The first authorized frame could not be prepared"));
                }
                if (!pending->data->valid.load(std::memory_order_acquire) && !preview.failure)
                    preview.failure = pending->failure ? pending->failure : fromTerminal(pending->data->outcome());
                if (!preview.failure && (preview.image.isNull() || pending->ending || shuttingDown)) preview.failure = cancelled();
                prepared(pending, std::move(preview.image), preview.failure);
                if (preview.failure) stopOperation(pending, false, preview.failure);
                checkDone();
            });
        pending->previewActive = true;
        try {
            watcher->setFuture(QtConcurrent::run(&pool, [data = pending->data, maximumBytes = pending->options.pool.maximumBytes] {
                Preview result;
                auto read = data->queue->waitNext(data->stop.get_token());
                if (read.interrupted) { result.failure = cancelled(); return result; }
                if (!read.frame) {
                    result.failure = read.terminal ? fromTerminal(*read.terminal) : unavailable();
                    if (!result.failure) result.failure = fail(Failure::Code::Platform, QStringLiteral("The authorized source ended without a complete first frame"));
                    return result;
                }
                Media::FrameNormalizer normalizer({1, maximumBytes, 16384});
                auto normalized = normalizer.normalize(read.frame);
                if (!normalized.frame) {
                    result.failure = normalized.error == Media::FrameNormalizationError::Budget ||
                        normalized.error == Media::FrameNormalizationError::Allocation ? allocation() :
                        fail(Failure::Code::Unavailable, QStringLiteral("The authorized frame cannot be displayed with its actual crop and orientation"));
                    return result;
                }
                const auto& frame = normalized.frame;
                const auto& format = frame.format();
                const auto qtFormat = format.alpha == Core::FrameAlpha::Premultiplied ? QImage::Format_ARGB32_Premultiplied :
                    format.alpha == Core::FrameAlpha::Opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32;
                const QImage view(frame.row(0, 0).data(), format.width, format.height, format.planes[0].stride, qtFormat);
                result.image = view.copy(); // one owned selection image, never a per-frame GUI copy
                if (result.image.isNull()) { result.failure = allocation(); return result; }
                result.image.setDevicePixelRatio(1);
                result.image.setColorSpace(imageColorSpace(format.color));
                {
                    std::lock_guard lock(data->mutex);
                    // Keep the original native lease, not the QImage copy.
                    // Static compositors may have no further damage frames
                    // after this preview; claim transfers this slot once.
                    data->firstFrame = std::move(read.frame);
                }
                return result;
            }));
        } catch (...) { pending->previewActive = false; throw; }
        watcher.release();
    }
};

LiveCaptureSource::LiveCaptureSource(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
LiveCaptureSource::~LiveCaptureSource()
{
    state_->shuttingDown = true; state_->monitor.stop();
    if (state_->operation && state_->operation->data) {
        state_->operation->data->close({End::Cancelled, 0, "The live source owner was destroyed"}, true);
        state_->operation->data->stop.request_stop();
    }
    // Normal shutdown is asynchronous and awaits shutdownFinished. Abrupt
    // owner destruction still keeps the Portal alive until the private native
    // loop and preview worker have returned; retained leases own only CPU bytes.
    state_->pool.waitForDone();
#ifdef Q_OS_LINUX
    state_->portal.reset();
#endif
}
void LiveCaptureSource::initialize()
{
    if (!state_->ownerThread()) return;
    try { state_->initialize(); }
    catch (const std::bad_alloc&) { state_->available = {CapabilityState::Unavailable, allocation().detail}; state_->initialized(allocation()); }
}
Capability LiveCaptureSource::capability() const { return state_->available; }
bool LiveCaptureSource::busy() const noexcept { return state_->initializing || state_->operation || state_->shuttingDown; }
quint64 LiveCaptureSource::request(QWindow* parent, LiveCaptureOptions options)
{
    if (!state_->ownerThread() || state_->shuttingDown) return 0;
    auto token = ++state_->sequence; if (!token) token = ++state_->sequence;
    const auto reject = [this, token](Failure error) {
        state_->post([token, error](LiveCaptureSource* target) { Q_EMIT target->prepared(token, {}, error); });
    };
    if (state_->operation) { reject(fail(Failure::Code::Busy, QStringLiteral("Another authorized live source is still finishing"))); return token; }
    if (!parent || !parent->isVisible() || parent->thread() != thread() || (!options.monitors && !options.windows) ||
        options.maximumFrameRate > 1000 || options.pool.slotCount < 4 || !options.pool.maximumBytes || options.pool.maximumDimension <= 0) {
        reject(fail(Failure::Code::InvalidSnapshot, QStringLiteral("Live capture requires a visible parent, one source kind and a bounded frame pool")));
        return token;
    }
    try {
        auto pending = std::make_shared<State::Operation>(); pending->token = token;
        pending->sourceId = nextSourceIdentity(); pending->options = options; pending->parent = parent;
        state_->operation = pending;
        if (state_->ready) state_->begin(); else state_->initialize();
    } catch (const std::bad_alloc&) {
        if (state_->operation && state_->operation->token == token) state_->stopOperation(state_->operation, true, allocation());
        else reject(allocation());
    }
    return token;
}
bool LiveCaptureSource::isCurrent(quint64 sourceId, quint64 generation) const
{
    const auto pending = state_->operation;
    return state_->ownerThread() && !state_->shuttingDown && pending && pending->sourceReady && !pending->ending &&
        pending->data && pending->sourceId == sourceId && pending->generation == generation &&
        pending->data->valid.load(std::memory_order_acquire) &&
        pending->data->activity->load(std::memory_order_acquire) != Activity::Closed;
}
std::optional<Core::LiveSourceConsumer> LiveCaptureSource::claim(quint64 sourceId, quint64 generation, Core::FrameCrop selection)
{
    const auto pending = state_->operation;
    if (!isCurrent(sourceId, generation) || pending->claimed || !selection.width || !selection.height ||
        selection.x > static_cast<unsigned>(pending->extent.width()) ||
        selection.width > static_cast<unsigned>(pending->extent.width()) - selection.x ||
        selection.y > static_cast<unsigned>(pending->extent.height()) ||
        selection.height > static_cast<unsigned>(pending->extent.height()) - selection.y) return {};
    Core::LiveSourceConsumer consumer;
    {
        std::lock_guard lock(pending->data->mutex);
        if (!pending->data->firstFrame) return {};
        consumer.firstFrame = std::move(pending->data->firstFrame);
    }
    consumer.sourceId = sourceId; consumer.generation = generation; consumer.selection = selection;
    consumer.frames = pending->data->queue; consumer.activity = pending->data->activity;
    consumer.stop = pending->data->stop; consumer.claimedAt = Core::MediaClock::now();
    pending->consumer = pending->claimed = true;
    return consumer;
}
void LiveCaptureSource::consumerFinished(quint64 sourceId, quint64 generation)
{
    if (!state_->ownerThread()) return;
    const auto pending = state_->operation;
    if (!pending || pending->sourceId != sourceId || pending->generation != generation || !pending->consumer) return;
    pending->consumer = false;
    state_->stopOperation(pending, false);
}
void LiveCaptureSource::stop(quint64 token)
{
    if (!state_->ownerThread() || !state_->operation || state_->operation->token != token) return;
    state_->stopOperation(state_->operation, false);
}
void LiveCaptureSource::cancel(quint64 token)
{
    if (!state_->ownerThread() || !state_->operation || state_->operation->token != token) return;
    state_->stopOperation(state_->operation, true, cancelled());
}
void LiveCaptureSource::shutdown()
{
    if (!state_->ownerThread() || state_->shuttingDown) return;
    state_->shuttingDown = true;
    if (state_->operation) state_->stopOperation(state_->operation, true, cancelled());
    else state_->checkDone();
}

} // namespace HuhuCapture::Capture
