// Worker/progress/output flow adapted from this project's PR #7 ScrollController
// at 251bc0071278c6b711c840bf5188d16870bc7349. MIT, Copyright (c) 2026 NAMEWTA.
// Exact source and retained/different contracts: docs/scroll-capture.md.
#include "capture/ScrollCaptureSession.h"

#include "core/ImagePreview.h"
#include "platform/NativeScrollSource.h"
#include "services/capture/LiveCaptureSource.h"
#include "services/capture/ScreenCaptureService.h"
#include "services/media/FrameNormalizer.h"
#include <QColorSpace>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Capture {
namespace {
using namespace std::chrono_literals;
using Rules = ScrollCaptureRules;
Failure failed(Failure::Code code, QString detail) { return {code, 0, std::move(detail)}; }
Failure fromTerminal(const Core::FrameSourceTerminal& terminal)
{
    if (terminal.reason == Core::FrameSourceEnd::Stopped) return {};
    const auto code = terminal.reason == Core::FrameSourceEnd::Cancelled ? Failure::Code::Cancelled :
        terminal.reason == Core::FrameSourceEnd::PermissionDenied ? Failure::Code::Denied :
        terminal.reason == Core::FrameSourceEnd::SourceClosed ? Failure::Code::SourceChanged :
        terminal.reason == Core::FrameSourceEnd::BudgetExceeded ? Failure::Code::Allocation :
        terminal.reason == Core::FrameSourceEnd::UnsupportedFormat ? Failure::Code::Unavailable : Failure::Code::Platform;
    return {code, terminal.nativeCode, QString::fromStdString(terminal.detail)};
}
bool delay(std::stop_token stop, std::chrono::milliseconds duration)
{
    std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
    wake.wait_for(lock, stop, duration, [] { return false; });
    return !stop.stop_requested();
}
struct WorkerData {
    std::stop_source stop;
    std::mutex mutex;
    std::condition_variable_any acknowledged;
    quint64 requested{}, completed{};
    bool permitted{};
    std::optional<ScrollProgress> progress;
    std::shared_ptr<ScrollCaptureModel> model{std::make_shared<ScrollCaptureModel>()};
    std::optional<Core::LiveSourceConsumer> consumer;
    QImage::Format imageFormat{QImage::Format_ARGB32};
    QColorSpace imageColorSpace;
    Failure interruption;
};
struct WorkerResult { ScrollCompletion completion; };
void publish(const std::shared_ptr<WorkerData>& data, ScrollNotice notice, std::vector<std::uint8_t>& preview)
{
    const auto& model = *data->model;
    ScrollProgress update;
    update.notice = notice; update.imageSize = {model.width(), model.height()};
    try {
        const auto size = Core::makeImagePreview(model.pixels(), model.width(), model.height(), 360.f, 480, 4096, preview);
        if (size) {
            update.preview = QImage(preview.data(), size->width, size->height, size->width * 4, data->imageFormat).copy();
            update.preview.setColorSpace(data->imageColorSpace);
        }
    } catch (const std::bad_alloc&) {
        // CapLong keeps capturing if only its thumbnail cannot be allocated.
        // An empty update leaves the existing visible thumbnail in place.
    }
    std::lock_guard lock(data->mutex); data->progress = std::move(update);
}
WorkerResult runScroll(const ScrollSelection& selection, std::optional<Core::LiveSourceConsumer> live,
    const std::shared_ptr<WorkerData>& data, const std::function<bool(quint64)>& beforeNative,
    const std::function<void(quint64)>& afterNative)
{
    WorkerResult result;
    auto& model = *data->model;
    const auto stop = data->stop.get_token();
    const bool manual = live.has_value();
    data->imageColorSpace = selection.preview.colorSpace();
    if (live && live->firstFrame && live->firstFrame.format().alpha == Core::FrameAlpha::Premultiplied)
        data->imageFormat = QImage::Format_ARGB32_Premultiplied;
    std::unique_ptr<Platform::NativeScrollSource> native;
    std::vector<std::uint8_t> preview;
    Media::FrameNormalizer normalizer({1, 128u * 1024u * 1024u, 16384});
    quint64 sample{};
    bool initialized{};
    auto notice = manual ? ScrollNotice::ManualCapturing : ScrollNotice::Capturing;
    auto publishCurrent = [&] { if (model.hasImage()) publish(data, notice, preview); };
    try {
        if (!manual && !stop.stop_requested()) {
            const auto& rect = *selection.desktopRect;
            native = std::make_unique<Platform::NativeScrollSource>(rect.x(), rect.y(), rect.width(), rect.height(), stop);
        }
        auto read = [&]() -> std::optional<ScrollUpdate> {
            Core::FrameLease normalized;
            std::span<const std::uint8_t> bytes;
            const auto previousNotice = notice;
            if (manual) {
                auto next = live->frames->takeLatest();
                if (!next.frame && !next.terminal && live->firstFrame) next.frame = std::move(live->firstFrame);
                else live->firstFrame = {};
                if (next.terminal) { result.completion.failure = fromTerminal(*next.terminal); return {}; }
                if (!next.frame) {
                    if (!initialized) {
                        result.completion.failure = failed(Failure::Code::SourceChanged,
                            QStringLiteral("The authorized source no longer has its first complete frame"));
                        return {};
                    }
                    const auto previous = notice;
                    notice = live->streaming() ? ScrollNotice::ManualCapturing : ScrollNotice::SourcePaused;
                    if (notice != previous) publishCurrent();
                    return ScrollUpdate{ScrollNextStep::Scroll, ScrollStopReason::None, false};
                }
                if (next.frame.metadata().sourceId != selection.sourceId) {
                    result.completion.failure = failed(Failure::Code::SourceChanged, QStringLiteral("The authorized scrolling source identity changed")); return {};
                }
                auto converted = normalizer.normalize(next.frame, live->selection);
                if (!converted.frame) {
                    result.completion.failure = failed(Failure::Code::Unavailable, QStringLiteral("The authorized scrolling frame could not be normalized with its selection")); return {};
                }
                normalized = std::move(converted.frame);
                if (normalized.format().width != selection.sourceRect.width() || normalized.format().height != selection.sourceRect.height()) {
                    result.completion.failure = failed(Failure::Code::SourceChanged, QStringLiteral("The selected scrolling source dimensions changed")); return {};
                }
                bytes = normalized.plane(0);
                notice = ScrollNotice::ManualCapturing;
            } else {
                const auto current = ++sample;
                if (!beforeNative(current)) {
                    if (!stop.stop_requested()) result.completion.failure = failed(Failure::Code::Platform,
                        QStringLiteral("The scrolling controls could not be excluded before capture"));
                    return {};
                }
                const auto captured = native->capture(); afterNative(current);
                if (stop.stop_requested()) return {};
                if (!captured) {
                    if (initialized) {
                        const auto retry = model.captureFailed();
                        if (retry.next == ScrollNextStep::Stopped)
                            result.completion.failure = failed(Failure::Code::Platform, QString::fromStdString(native->error()));
                        return retry;
                    }
                    result.completion.failure = failed(Failure::Code::Platform, QString::fromStdString(native->error()));
                    return {};
                }
                bytes = native->pixels(); notice = ScrollNotice::Capturing;
            }
            if (!initialized) {
                const auto started = model.begin(bytes, selection.sourceRect.width(), selection.sourceRect.height());
                if (started != ScrollStartError::None) {
                    result.completion.failure = failed(started == ScrollStartError::MemoryLimit ? Failure::Code::Allocation : Failure::Code::InvalidSnapshot,
                        QStringLiteral("The first complete scrolling frame could not be retained")); return {};
                }
                initialized = true; publishCurrent();
                return ScrollUpdate{ScrollNextStep::Scroll, ScrollStopReason::None, false};
            }
            const auto update = model.accept(bytes, !manual);
            if (update.appended || notice != previousNotice) publishCurrent();
            return update;
        };
        auto initial = stop.stop_requested() ? std::optional<ScrollUpdate>{} : read();
        bool wheel = !manual;
        auto wait = std::chrono::milliseconds(Rules::firstScrollDelayMs);
        while (initial && model.running() && delay(stop, wait)) {
            if (wheel) {
                const auto input = native->scroll();
                if (stop.stop_requested()) break;
                if (input == Platform::ScrollInputResult::PointerOutsideTarget) {
                    if (notice != ScrollNotice::PointerPaused) { notice = ScrollNotice::PointerPaused; publishCurrent(); }
                    wait = std::chrono::milliseconds(Rules::retryDelayMs); continue;
                }
                if (input == Platform::ScrollInputResult::Failed) {
                    result.completion.failure = failed(Failure::Code::Platform, QString::fromStdString(native->error())); break;
                }
                if (!delay(stop, std::chrono::milliseconds(Rules::scrollSettleMs))) break;
            }
            auto update = read();
            if (!update || update->next == ScrollNextStep::Stopped) break;
            wheel = !manual && update->next == ScrollNextStep::Scroll;
            wait = std::chrono::milliseconds(update->next == ScrollNextStep::Settle ? Rules::settleRecheckMs :
                manual ? Rules::scrollSettleMs : Rules::retryDelayMs);
        }
    } catch (const std::bad_alloc&) {
        model.stop(ScrollStopReason::MemoryLimit);
        result.completion.failure = failed(Failure::Code::Allocation, QStringLiteral("The scrolling capture reached available memory"));
    } catch (const std::exception& error) {
        result.completion.failure = failed(Failure::Code::Platform, QString::fromUtf8(error.what()));
    } catch (...) {
        result.completion.failure = failed(Failure::Code::Platform, QStringLiteral("The scrolling capture worker did not complete"));
    }
    native.reset();
    if (live) {
        // Request real producer stop and drain its queue before releasing the
        // source claim. The GUI/Portal owner separately awaits native return.
        live->firstFrame = {}; live->stop.request_stop();
        for (;;) {
            auto next = live->frames->waitNext();
            if (next.terminal) {
                if (!result.completion.failure) result.completion.failure = fromTerminal(*next.terminal);
                break;
            }
        }
        live.reset();
    }
    model.stop();
    { std::lock_guard lock(data->mutex); if (data->interruption) result.completion.failure = data->interruption; }
    result.completion.reason = model.stopReason();
    result.completion.imageSize = model.hasImage() ? QSize(model.width(), model.height()) : QSize();
    return result;
}
}

struct ScrollCaptureSession::State {
    ScrollCaptureSession* owner;
    ScrollSelection selection;
    ScrollProgress preview;
    ScrollCompletion completion;
    ScrollPhase phase{ScrollPhase::Empty};
    QPointer<ScreenCaptureService> nativeOwner;
    QPointer<LiveCaptureSource> liveOwner;
    QMetaObject::Connection invalidated;
    QMetaObject::Connection sourceDestroyed;
    QMetaObject::Connection refreshCompleted;
    std::optional<Core::LiveSourceConsumer> claim;
    std::shared_ptr<WorkerData> data;
    QThreadPool pool;
    QTimer progressTimer;
    quint64 sequence{}, token{}, refreshToken{};
    bool worker{}, claimed{}, notifying{}, shuttingDown{}, shutdownReported{};
    explicit State(ScrollCaptureSession* value) : owner(value)
    {
        pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1);
        progressTimer.setInterval(100);
        QObject::connect(&progressTimer, &QTimer::timeout, owner, [this] { deliverProgress(); });
    }
    bool ownerThread() const { return qGuiApp && owner->thread() == qGuiApp->thread() && QThread::currentThread() == owner->thread(); }
    bool admissible() const { return ownerThread() && !shuttingDown && !worker && !claimed && !refreshToken && !notifying && phase != ScrollPhase::Ready; }
    void deliverProgress()
    {
        if (!data) return;
        std::optional<ScrollProgress> latest;
        { std::lock_guard lock(data->mutex); latest.swap(data->progress); }
        if (!latest) return;
        preview = std::move(*latest);
        if (phase == ScrollPhase::Starting) phase = ScrollPhase::Capturing;
        Q_EMIT owner->previewChanged(preview);
    }
    void closed()
    {
        if (!shuttingDown || worker || claimed || refreshToken || notifying || shutdownReported) return;
        phase = ScrollPhase::Closed; shutdownReported = true;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
    }
    void sourceChanged(const Failure& error)
    {
        if (refreshToken) {
            if (!completion.failure) completion.failure = error;
            phase = ScrollPhase::Stopping;
            if (liveOwner) liveOwner->cancelRefresh(refreshToken);
            else completeWithoutWorker(error);
            return;
        }
        if (data) {
            { std::lock_guard lock(data->mutex); if (!data->interruption) data->interruption = error; }
            data->stop.request_stop();
        }
        if (!worker) { completion.failure = error; phase = ScrollPhase::Finished; }
        const QPointer<ScrollCaptureSession> alive(owner);
        Q_EMIT owner->changed();
        if (alive && !worker && claimed) releaseIdleConsumer();
    }
    void releaseConsumer()
    {
        if (!claimed) return;
        claimed = false;
        const auto native = liveOwner;
        const auto id = selection.sourceId, generation = selection.generation;
        Q_EMIT owner->consumerReleased(id, generation);
        if (native) native->consumerFinished(id, generation);
    }
    void releaseIdleConsumer()
    {
        // No consumer worker was started. Release its only retained lease and
        // hand it back immediately; LiveCaptureSource still waits for the real
        // producer/Portal drain before it emits stopped().
        if (claim) { claim->firstFrame = {}; claim->stop.request_stop(); claim.reset(); }
        const QPointer<ScrollCaptureSession> alive(owner);
        notifying = true;
        releaseConsumer();
        if (alive) { notifying = false; closed(); }
    }
    void completeWithoutWorker(Failure error)
    {
        refreshToken = 0;
        if (!completion.failure) completion.failure = std::move(error);
        phase = ScrollPhase::Finished; notifying = true;
        const QPointer<ScrollCaptureSession> alive(owner);
        if (claim) { claim->firstFrame = {}; claim->stop.request_stop(); claim.reset(); }
        releaseConsumer(); if (!alive) return;
        Q_EMIT owner->finished(token, completion); if (!alive) return;
        Q_EMIT owner->changed(); if (!alive) return;
        notifying = false; closed();
    }
    void liveRefreshed(quint64 refreshed, quint64 id, quint64 generation, const Failure& failure)
    {
        if (!refreshToken || refreshed != refreshToken || id != selection.sourceId || generation != selection.generation) return;
        refreshToken = 0;
        if (failure || shuttingDown || phase == ScrollPhase::Stopping) {
            completeWithoutWorker(failure ? failure : failed(Failure::Code::Cancelled,
                QStringLiteral("Scrolling capture was stopped before its first capture frame"))); return;
        }
        const auto& rectangle = selection.sourceRect;
        if (liveOwner) claim = liveOwner->claim(id, generation,
            {static_cast<std::uint32_t>(rectangle.x()), static_cast<std::uint32_t>(rectangle.y()),
                static_cast<std::uint32_t>(rectangle.width()), static_cast<std::uint32_t>(rectangle.height())});
        if (!claim) {
            completeWithoutWorker(failed(Failure::Code::SourceChanged,
                QStringLiteral("The refreshed scrolling source could not be claimed"))); return;
        }
        claimed = true;
        const QPointer<ScrollCaptureSession> alive(owner);
        if (!launch(token) && alive) completeWithoutWorker(completion.failure ? completion.failure :
            failed(Failure::Code::Platform, QStringLiteral("The refreshed scrolling worker could not start")));
    }
    quint64 start()
    {
        if (worker || claimed || refreshToken || notifying || phase != ScrollPhase::Ready) return 0;
        if (selection.coordinates != CoordinateSpace::AuthorizedSourcePixels) return launch();
        if (!liveOwner || !liveOwner->isCurrent(selection.sourceId, selection.generation)) {
            sourceChanged(failed(Failure::Code::SourceChanged, QStringLiteral("The authorized scrolling source is no longer current"))); return 0;
        }
        auto current = ++sequence; if (!current) current = ++sequence;
        token = current; completion = {}; phase = ScrollPhase::Starting;
        refreshToken = liveOwner->refreshForCapture(selection.sourceId, selection.generation);
        if (!refreshToken) { phase = ScrollPhase::Ready; return 0; }
        Q_EMIT owner->changed(); return current;
    }
    quint64 launch(quint64 preparedToken = 0)
    {
        if (worker || notifying || refreshToken ||
            (preparedToken ? phase != ScrollPhase::Starting : phase != ScrollPhase::Ready)) return 0;
        if (selection.coordinates == CoordinateSpace::DesktopPixels &&
            (!nativeOwner || !nativeOwner->isCurrent(selection.sourceId, selection.generation))) {
            sourceChanged(failed(Failure::Code::SourceChanged, QStringLiteral("The selected desktop source is no longer current"))); return 0;
        }
        if (selection.coordinates == CoordinateSpace::AuthorizedSourcePixels &&
            (!liveOwner || !liveOwner->isCurrent(selection.sourceId, selection.generation))) {
            sourceChanged(failed(Failure::Code::SourceChanged, QStringLiteral("The authorized scrolling source is no longer current"))); return 0;
        }
        try {
            auto nextData = std::make_shared<WorkerData>();
            auto watcher = std::make_unique<QFutureWatcher<WorkerResult>>(owner);
            auto current = preparedToken;
            if (!current) { current = ++sequence; if (!current) current = ++sequence; }
            QObject::connect(watcher.get(), &QFutureWatcher<WorkerResult>::finished, owner, [this, watcher = watcher.get(), current] {
                WorkerResult result;
                try { result = watcher->result(); }
                catch (const std::exception& error) { result.completion.failure = failed(Failure::Code::Platform, QString::fromUtf8(error.what())); }
                catch (...) { result.completion.failure = failed(Failure::Code::Platform, QStringLiteral("The scrolling capture worker did not return a result")); }
                watcher->deleteLater(); worker = false; notifying = true; progressTimer.stop();
                const QPointer<ScrollCaptureSession> alive(owner);
                deliverProgress(); if (!alive) return;
                completion = std::move(result.completion); phase = ScrollPhase::Finished;
                releaseConsumer(); if (!alive) return;
                Q_EMIT owner->finished(current, completion); if (!alive) return;
                Q_EMIT owner->changed(); if (!alive) return;
                notifying = false;
                closed();
            });
            auto before = [context = owner, current, shared = nextData](quint64 sample) {
                { std::lock_guard lock(shared->mutex); shared->requested = sample; shared->permitted = false; }
                QMetaObject::invokeMethod(context, [context, current, sample] { Q_EMIT context->nativeCaptureRequested(current, sample); }, Qt::QueuedConnection);
                std::unique_lock lock(shared->mutex);
                shared->acknowledged.wait_for(lock, shared->stop.get_token(), 2s, [&] { return shared->completed == sample; });
                return !shared->stop.stop_requested() && shared->completed == sample && shared->permitted;
            };
            auto after = [context = owner, current](quint64 sample) {
                QMetaObject::invokeMethod(context, [context, current, sample] { Q_EMIT context->nativeCaptureCompleted(current, sample); }, Qt::QueuedConnection);
            };
            auto work = [selected = selection, shared = nextData,
                before = std::move(before), after = std::move(after)]() mutable {
                return runScroll(selected, std::move(shared->consumer), shared, before, after);
            };
            // Every allocation needed to construct the worker is complete before
            // moving the sole claim. A rejected submission retains it for retry or
            // shutdown; a submitted worker owns it until its actual return.
            nextData->consumer = std::move(claim);
            try { watcher->setFuture(QtConcurrent::run(&pool, std::move(work))); }
            catch (...) { claim = std::move(nextData->consumer); throw; }
            data = std::move(nextData); token = current; worker = true; completion = {}; phase = ScrollPhase::Starting;
            claim.reset(); watcher.release(); progressTimer.start();
            Q_EMIT owner->changed(); return current;
        } catch (const std::bad_alloc&) {
            completion.failure = failed(Failure::Code::Allocation, QStringLiteral("The scrolling worker could not be allocated"));
        } catch (const std::exception& error) {
            completion.failure = failed(Failure::Code::Platform, QString::fromUtf8(error.what()));
        }
        Q_EMIT owner->changed(); return 0;
    }
};

ScrollCaptureSession::ScrollCaptureSession(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
ScrollCaptureSession::~ScrollCaptureSession()
{
    state_->progressTimer.stop(); QObject::disconnect(state_->invalidated); QObject::disconnect(state_->sourceDestroyed);
    QObject::disconnect(state_->refreshCompleted);
    if (state_->refreshToken && state_->liveOwner) state_->liveOwner->cancelRefresh(state_->refreshToken);
    if (state_->data) state_->data->stop.request_stop();
    if (state_->claim) {
        state_->claim->firstFrame = {}; state_->claim->stop.request_stop();
        state_->claim.reset();
    }
    state_->pool.waitForDone();
    if (state_->claimed && state_->liveOwner) state_->liveOwner->consumerFinished(state_->selection.sourceId, state_->selection.generation);
}
Failure ScrollCaptureSession::prepareNative(const ActionRequest& request, const Snapshot& source, ScreenCaptureService& owner)
{
    if (!state_->admissible()) return failed(Failure::Code::Busy, QStringLiteral("Another scrolling capture is still active"));
    if (request.action != Action::Scroll || request.coordinates != CoordinateSpace::DesktopPixels ||
        source.coordinates != request.coordinates || source.sourceId != request.sourceId || source.generation != request.generation ||
        request.image.isNull() || request.image.size() != request.sourceRect.size() || request.sourceRect.isEmpty() ||
        !source.image.rect().contains(request.sourceRect) || !source.desktopOrigin || !request.desktopRect ||
        *request.desktopRect != request.sourceRect.translated(*source.desktopOrigin) || source.screens.isEmpty() ||
        !owner.isCurrent(request.sourceId, request.generation))
        return failed(Failure::Code::SourceChanged, QStringLiteral("The scrolling rectangle is not bound to the current native desktop"));
    QObject::disconnect(state_->invalidated);
    QObject::disconnect(state_->sourceDestroyed);
    QObject::disconnect(state_->refreshCompleted);
    state_->selection = {request.sourceId, request.generation, request.coordinates, request.sourceRect, request.desktopRect, source.screens, request.image};
    state_->nativeOwner = &owner; state_->liveOwner.clear(); state_->data.reset(); state_->completion = {};
    state_->preview = {request.image, {}, ScrollNotice::Ready}; state_->phase = ScrollPhase::Ready;
    state_->invalidated = connect(&owner, &ScreenCaptureService::sourceInvalidated, this,
        [this](quint64 id, quint64 generation, const Failure& error) {
            if (id == state_->selection.sourceId && generation == state_->selection.generation) state_->sourceChanged(error);
        });
    state_->sourceDestroyed = connect(&owner, &QObject::destroyed, this, [this] {
        state_->sourceChanged(failed(Failure::Code::SourceChanged, QStringLiteral("The desktop capture source was closed")));
    });
    Q_EMIT changed(); return {};
}
Failure ScrollCaptureSession::prepareLive(const ActionRequest& request, LiveCaptureSource& owner)
{
    if (!state_->admissible()) return failed(Failure::Code::Busy, QStringLiteral("Another scrolling capture is still active"));
    if (request.action != Action::Scroll || request.coordinates != CoordinateSpace::AuthorizedSourcePixels || request.desktopRect ||
        request.sourceRect.x() < 0 || request.sourceRect.y() < 0 || request.sourceRect.isEmpty() ||
        request.image.isNull() || request.image.size() != request.sourceRect.size() || !owner.isCurrent(request.sourceId, request.generation))
        return failed(Failure::Code::SourceChanged, QStringLiteral("The scrolling selection does not belong to the current authorized source"));
    QObject::disconnect(state_->invalidated);
    QObject::disconnect(state_->sourceDestroyed);
    QObject::disconnect(state_->refreshCompleted);
    state_->selection = {request.sourceId, request.generation, request.coordinates, request.sourceRect, {}, {}, request.image};
    state_->liveOwner = &owner; state_->nativeOwner.clear();
    state_->data.reset(); state_->completion = {}; state_->preview = {request.image, {}, ScrollNotice::ManualReady}; state_->phase = ScrollPhase::Ready;
    state_->invalidated = connect(&owner, &LiveCaptureSource::sourceClosed, this, [this](quint64 id, quint64 generation, const Failure& error) {
        if (id == state_->selection.sourceId && generation == state_->selection.generation && error) state_->sourceChanged(error);
    });
    state_->sourceDestroyed = connect(&owner, &QObject::destroyed, this, [this] {
        state_->sourceChanged(failed(Failure::Code::SourceChanged, QStringLiteral("The authorized capture source was closed")));
    });
    state_->refreshCompleted = connect(&owner, &LiveCaptureSource::captureReady, this,
        [this](quint64 refreshed, quint64 id, quint64 generation, const Failure& error) {
            state_->liveRefreshed(refreshed, id, generation, error);
        });
    Q_EMIT changed(); return {};
}
const ScrollSelection& ScrollCaptureSession::selection() const noexcept { return state_->selection; }
ScrollPhase ScrollCaptureSession::phase() const noexcept { return state_->phase; }
ScrollProgress ScrollCaptureSession::progress() const { return state_->preview; }
ScrollCompletion ScrollCaptureSession::completion() const { return state_->completion; }
bool ScrollCaptureSession::busy() const noexcept { return state_->worker || state_->claimed || state_->refreshToken; }
bool ScrollCaptureSession::hasCompleteImage() const noexcept
{ return state_->data && !state_->worker && state_->data->model->hasImage(); }
QImage ScrollCaptureSession::image() const
{
    if (!hasCompleteImage()) return {};
    try {
        // The QImage borrows an immutable completed model allocation and keeps
        // that model alive. No copy of the complete long image is required.
        auto keeper = std::make_unique<std::shared_ptr<ScrollCaptureModel>>(state_->data->model);
        const auto& model = **keeper;
        QImage image(model.pixels().data(), model.width(), model.height(), qsizetype(model.width()) * 4, state_->data->imageFormat,
            [](void* context) { delete static_cast<std::shared_ptr<ScrollCaptureModel>*>(context); }, keeper.get());
        if (!image.isNull()) { keeper.release(); image.setColorSpace(state_->data->imageColorSpace); }
        return image;
    } catch (const std::bad_alloc&) { return {}; }
}
quint64 ScrollCaptureSession::start()
{ return state_->ownerThread() && !state_->shuttingDown ? state_->start() : 0; }
void ScrollCaptureSession::stop()
{
    if (!state_->ownerThread()) return;
    if (state_->refreshToken) {
        state_->phase = ScrollPhase::Stopping;
        if (state_->liveOwner) state_->liveOwner->cancelRefresh(state_->refreshToken);
        Q_EMIT changed(); return;
    }
    if (!state_->worker || !state_->data) return;
    state_->phase = ScrollPhase::Stopping; state_->data->stop.request_stop(); Q_EMIT changed();
}
void ScrollCaptureSession::acknowledgeNativeCapture(quint64 token, quint64 sample, bool ready)
{
    if (!state_->ownerThread() || !state_->worker || token != state_->token || !state_->data) return;
    const auto data = state_->data;
    { std::lock_guard lock(data->mutex); if (sample != data->requested) return; data->completed = sample; data->permitted = ready; }
    data->acknowledged.notify_all();
}
void ScrollCaptureSession::shutdown()
{
    if (!state_->ownerThread() || state_->shuttingDown) return;
    state_->shuttingDown = true;
    if (state_->worker || state_->refreshToken) { stop(); return; }
    if (state_->claimed) { state_->releaseIdleConsumer(); return; }
    state_->closed();
}
} // namespace HuhuCapture::Capture
