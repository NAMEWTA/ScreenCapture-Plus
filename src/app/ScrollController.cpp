#include "app/ScrollController.h"

#include "app/MediaWorkspace.h"
#include "capture/ScrollAccumulator.h"
#include "core/ImagePreview.h"
#include "platform/NativeScrollSource.h"
#include "platform/WindowActivation.h"
#include "recording/FramePixels.h"
#include "services/capture/ScreenCastSource.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/DesktopTheme.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Application {
namespace ui = UI::Qt;
namespace {
using namespace std::chrono_literals;
class ScrollPreview final : public QWidget {
public:
    QImage image;
    explicit ScrollPreview(QWidget* parent) : QWidget(parent) { setMinimumSize(160, 160); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(235, 236, 239));
        if (image.isNull()) return;
        const auto size = image.size().scaled(this->size() - QSize(12, 12), ::Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((width() - size.width()) / 2, height() - size.height() - 6), size), image);
    }
};
class ScrollWindow final : public QWidget {
public:
    ScrollPreview* preview{};
    QLabel *status{}, *dimensions{};
    QPushButton *start{}, *stop{}, *pin{}, *copy{}, *save{}, *close{};
    std::function<void()> dismiss;
    bool allowedClose{};
    ScrollWindow()
    {
        setObjectName(QStringLiteral("scrollCaptureWindow")); setWindowTitle(QStringLiteral("huhucapture · Scrolling capture"));
        setWindowFlag(::Qt::WindowStaysOnTopHint); ui::DesktopTheme::apply(this);
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12, 12, 12, 12); layout->setSpacing(8);
        status = ui::DesktopTheme::label(this, 12, true); status->setObjectName(QStringLiteral("scrollStatus")); status->setWordWrap(true); layout->addWidget(status);
        dimensions = ui::DesktopTheme::label(this, 12, true); dimensions->setObjectName(QStringLiteral("scrollDimensions")); layout->addWidget(dimensions);
        preview = new ScrollPreview(this); layout->addWidget(preview, 1);
        auto* capture = new QHBoxLayout;
        start = ui::DesktopTheme::button(this, "scrollStart", true); stop = ui::DesktopTheme::button(this, "scrollStop");
        capture->addWidget(start); capture->addWidget(stop); layout->addLayout(capture);
        auto* actions = new QHBoxLayout;
        pin = ui::DesktopTheme::button(this, "scrollPin"); copy = ui::DesktopTheme::button(this, "scrollCopy");
        save = ui::DesktopTheme::button(this, "scrollSave"); close = ui::DesktopTheme::button(this, "scrollClose");
        for (auto* button : {pin, copy, save, close}) actions->addWidget(button);
        layout->addLayout(actions); resize(420, 520); setMinimumSize(360, 320);
    }
protected:
    void closeEvent(QCloseEvent* event) override
    { if (!allowedClose && dismiss) { event->ignore(); dismiss(); } else event->accept(); }
};
struct ScrollResult { QImage image; QString detail; bool failure{}; };
struct Progress { QImage preview; int width{}, height{}; QString detail; };
struct SourceContract {
    Core::FrameFormat format;
    std::optional<Core::FrameCrop> crop;
    Core::FrameTransform transform{};
    quint64 generation{}, sessionId{}, sourceId{};
};
struct FrameGate {
    std::mutex mutex;
    std::condition_variable_any completed;
    quint64 request{}, acknowledged{};
    bool hidden{};
};

QImage takeImage(Capture::ScrollAccumulator& accumulator)
{
    if (accumulator.pixels().empty()) return {};
    auto* bytes = new std::vector<std::uint8_t>(accumulator.takePixels());
    QImage image(static_cast<const uchar*>(bytes->data()), accumulator.width(), accumulator.height(),
        static_cast<qsizetype>(accumulator.width()) * 4, QImage::Format_ARGB32,
        [](void* context) { delete static_cast<std::vector<std::uint8_t>*>(context); }, bytes);
    return image;
}
bool delay(std::stop_token stop, std::chrono::milliseconds duration)
{
    std::mutex mutex; std::condition_variable_any wake;
    std::unique_lock lock(mutex); wake.wait_for(lock, stop, duration, [] { return false; });
    return !stop.stop_requested();
}
ScrollResult runScroll(QRect region, std::unique_ptr<Core::FrameSource> stream,
    std::stop_token stop, std::function<void(Progress)> progress,
    std::function<bool()> beforeNativeCapture, std::function<void()> afterNativeCapture,
    std::optional<SourceContract> expectedSource)
{
    struct StopSource {
        std::unique_ptr<Core::FrameSource>& source;
        ~StopSource() { if (source) source->stopAndWait(); }
    } stopSource{stream};
    const bool manual = static_cast<bool>(stream);
    std::unique_ptr<Platform::NativeScrollSource> native;
    if (!manual) native = std::make_unique<Platform::NativeScrollSource>(region.x(), region.y(), region.width(), region.height());
    const auto layout = Core::pixelLayout(region.width(), region.height(), 4);
    if (!layout || layout->bytes > 512ull * 1024 * 1024) return {{}, QStringLiteral("The scrolling region exceeds the pixel budget"), true};
    std::vector<std::uint8_t> normalized, preview;
    if (manual) normalized.resize(layout->bytes);
    const Recording::PixelRegion selection{region.x(), region.y(), region.width(), region.height()};
    std::optional<std::pair<int, int>> sourceSize;
    std::span<const std::uint8_t> frame;
    QString error;
    auto read = [&](bool first) -> bool {
        if (!manual) {
            if (!beforeNativeCapture()) {
                if (!stop.stop_requested()) error = QStringLiteral("The capture controls could not be excluded from the selected region");
                return false;
            }
            const auto captured = native->capture(); afterNativeCapture();
            if (!captured) { error = QString::fromStdString(native->error()); return false; }
            frame = native->pixels(); return true;
        }
        const auto deadline = Core::MediaClock::now() + (first ? 5s : 1s);
        while (!stop.stop_requested()) {
            auto next = stream->read(deadline, stop);
            if (next.status == Core::FrameReadStatus::Stopped) return false;
            if (next.status == Core::FrameReadStatus::Failed) { error = QString::fromStdString(next.detail); return false; }
            if (next.status == Core::FrameReadStatus::Idle) {
                if (first) { error = QStringLiteral("No frame arrived from the authorized source"); return false; }
                frame = normalized; return true; // a still-live static source
            }
            const auto size = Recording::normalizedFrameSize(next.frame);
            if (!size || (expectedSource && (next.frame.format() != expectedSource->format ||
                    next.frame.formatGeneration() != expectedSource->generation || next.frame.metadata().crop != expectedSource->crop ||
                    next.frame.metadata().sessionId != expectedSource->sessionId || next.frame.metadata().sourceId != expectedSource->sourceId ||
                    next.frame.metadata().transform != expectedSource->transform)) || (sourceSize && *sourceSize != *size) ||
                !Recording::copyFrameBgra(next.frame, selection, normalized, layout->stride)) {
                error = QStringLiteral("The authorized source geometry or pixel format changed"); return false;
            }
            sourceSize = size; frame = normalized; return true;
        }
        return false;
    };
    Capture::ScrollAccumulator accumulator;
    if (!read(true) || !accumulator.start(frame, region.width(), region.height(), {})) {
        if (error.isEmpty() && !stop.stop_requested()) error = QString::fromStdString(accumulator.error());
        return {{}, error, !error.isEmpty()};
    }
    auto publish = [&](QString detail = {}) {
        const auto size = Core::makeImagePreview(accumulator.pixels(), accumulator.width(), accumulator.height(),
            360.f, 360, 4096, preview);
        if (size) progress({QImage(preview.data(), size->width, size->height, size->width * 4, QImage::Format_ARGB32).copy(),
            accumulator.width(), accumulator.height(), std::move(detail)});
    };
    publish(manual ? QStringLiteral("long.manualCapturing") : QStringLiteral("long.capturing"));
    auto wait = 88ms; bool wheel = !manual;
    bool failed{}; QString detail;
    while (delay(stop, wait)) {
        if (wheel) {
            const auto input = native->scroll();
            if (input == Platform::ScrollInputResult::PointerOutsideTarget) {
                publish(QStringLiteral("long.pointerPaused")); wait = 500ms; continue;
            }
            if (input == Platform::ScrollInputResult::Failed) { failed = true; detail = QString::fromStdString(native->error()); break; }
            if (!delay(stop, 250ms)) break;
        }
        if (!read(false)) { failed = !error.isEmpty(); detail = error; break; }
        const auto appended = accumulator.append(frame, !manual);
        if (appended == Capture::ScrollAppend::Appended) {
            publish(); wait = manual ? 250ms : 500ms; wheel = !manual;
        } else if (appended == Capture::ScrollAppend::WaitForSettling) {
            wait = 250ms; wheel = false;
        } else if (appended == Capture::ScrollAppend::RetryScroll) {
            wait = manual ? 250ms : 500ms; wheel = !manual;
        } else {
            failed = appended == Capture::ScrollAppend::Failed;
            detail = failed ? QString::fromStdString(accumulator.error()) : appended == Capture::ScrollAppend::Limit ?
                QStringLiteral("long.sizeLimit") : QStringLiteral("long.captureBottom");
            break;
        }
    }
    if (stream) stream->stopAndWait();
    stream.reset(); native.reset();
    if (detail.isEmpty()) detail = QStringLiteral("long.finished");
    return {takeImage(accumulator), detail, failed};
}
}

struct ScrollController::State {
    ScrollController* owner{};
    std::shared_ptr<Media::MediaArchiveService> archive;
    MediaWorkspace* workspace{};
    ui::DesktopTranslator translator;
    std::unique_ptr<ScrollWindow> view;
    std::unique_ptr<ui::CaptureWindow> picker;
    Capture::ScreenCastSource* stream{};
    std::unique_ptr<Core::FrameSource> reader;
    QThreadPool pool;
    std::stop_source stop;
    QImage image;
    QRect region;
    QVector<QRectF> logicalCaptureRegions;
    quint64 generation{}, outputToken{};
    bool running{}, manual{}, preparing{}, closing{}, stopping{}, outputPending{}, wasBusy{};
    bool sourceInvalid{}, shutdownReported{}, nativeCaptureHidden{}, activationPending{};
    QString activationToken;
    QString interruption;
    QString statusContent;
    std::optional<SourceContract> sourceContract;
    std::vector<QMetaObject::Connection> screenConnections;
    std::optional<ui::OutputIntent> afterStop;
    State(ScrollController* value, std::shared_ptr<Media::MediaArchiveService> service, MediaWorkspace& media)
        : owner(value), archive(std::move(service)), workspace(&media)
    { pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1); }
    QString text(QStringView key, const QString& fallback) const
    {
        const auto value = translator ? translator(key) : QString();
        return value.isEmpty() || value == key ? fallback : value;
    }
    QString message(const QString& key) const
    {
        struct Entry { const char* key; const char* fallback; };
        static constexpr std::array entries{
            Entry{"long.ready", "Press Start to capture the selected region. Save or copy keeps the last complete image."},
            Entry{"long.manualReady", "Keep this control window outside the selected source region. Press Start, scroll the authorized source manually, then Stop."},
            Entry{"long.authorize", "Choose one screen or window to share, then select the scrolling region in that authorized image."},
            Entry{"long.capturing", "Capturing. Moving the pointer to another window pauses automatic scrolling."},
            Entry{"long.manualCapturing", "Scroll the authorized source manually. Stop when the required content is captured."},
            Entry{"long.pointerPaused", "Move the pointer back over the scrolling target to continue."},
            Entry{"long.sizeLimit", "The scrolling image reached its size limit. The last complete image is retained."},
            Entry{"long.captureBottom", "The bottom was reached. The complete image is ready."},
            Entry{"long.finished", "Stopped. The complete image is ready."},
            Entry{"long.outputBusy", "The output service is busy; the complete image is retained."},
            Entry{"long.displayChanged", "The display layout changed. The last complete scrolling image is retained; select a fresh region to continue."}};
        for (const auto& entry : entries) if (key == QLatin1String(entry.key)) return text(key, QString::fromUtf8(entry.fallback));
        return key;
    }
    void status(QString content)
    { statusContent = std::move(content); if (view) view->status->setText(message(statusContent)); }
    bool busy() const { return view || running || preparing || (stream && stream->busy()); }
    void notify()
    {
        const bool now = busy();
        const QPointer<ScrollController> lifetime(owner);
        if (now != wasBusy) { wasBusy = now; Q_EMIT owner->busyChanged(now); }
        if (lifetime && stopping && !now && !shutdownReported) { shutdownReported = true; Q_EMIT owner->shutdownFinished(); }
    }
    void activate(QString token)
    {
        if (closing || !view) return;
        if (nativeCaptureHidden) { activationPending = true; activationToken = std::move(token); return; }
        auto* target = picker && picker->visible() ? picker->dialogParent() : view.get();
        if (!target) return;
        const QPointer<QWidget> window(target);
        target->setAttribute(::Qt::WA_ShowWithoutActivating); target->show(); target->raise();
        if (!window) return;
        if (auto* native = window->windowHandle()) {
            Platform::applyActivationToken(native, token); native->requestActivate();
        }
    }
    void refresh()
    {
        if (!view) return;
        view->status->setText(message(statusContent));
        view->setWindowTitle(QStringLiteral("huhucapture · ") + text(u"long.title", QStringLiteral("Scrolling capture")));
        view->start->setText(text(u"long.start", QStringLiteral("Start")));
        view->stop->setText(text(u"long.stop", QStringLiteral("Stop")));
        view->pin->setText(text(u"tool.pin", QStringLiteral("Pin")));
        view->copy->setText(text(u"tool.clipboard", QStringLiteral("Copy")));
        view->save->setText(text(u"tool.save", QStringLiteral("Save as")));
        view->close->setText(text(u"tool.close", QStringLiteral("Close")));
        view->start->setEnabled(!running && !preparing && !closing && !sourceInvalid && !outputPending && !image.isNull() && (!manual || reader));
        view->stop->setEnabled(running && !closing);
        for (auto* button : {view->pin, view->copy, view->save}) button->setEnabled(!preparing && !closing && !outputPending && !image.isNull());
        view->close->setEnabled(!closing);
    }
    void cleanupView()
    {
        if (running || preparing || outputPending) return;
        const QPointer<ScrollController> lifetime(owner);
        if (picker) { picker->shutdown(); picker.reset(); }
        reader.reset();
        if (stream) stream->stop();
        if (!lifetime) return;
        if (view) { view->allowedClose = true; view->hide(); view.reset(); }
        image = {}; afterStop.reset(); closing = false; nativeCaptureHidden = activationPending = false; activationToken.clear();
        for (const auto& connection : screenConnections) QObject::disconnect(connection);
        screenConnections.clear();
        notify();
    }
    void close()
    {
        const QPointer<ScrollController> lifetime(owner);
        closing = true; afterStop.reset();
        if (view && outputPending) { workspace->cancelOutput(view.get(), outputToken); }
        if (!lifetime) return;
        stop.request_stop();
        if (picker) picker->cancel();
        if (!lifetime) return;
        reader.reset();
        if (stream && !running) stream->stop();
        if (!lifetime) return;
        if (view) { view->hide(); refresh(); }
        QTimer::singleShot(0, owner, [this] { if (!running && !preparing && !outputPending) cleanupView(); });
    }
    void output(ui::OutputIntent intent)
    {
        if (!view || closing || preparing || outputPending || image.isNull()) return;
        if (running) { afterStop = intent; stop.request_stop(); refresh(); return; }
        outputPending = true; ++outputToken; refresh();
        const auto token = outputToken;
        const QPointer<ScrollController> lifetime(owner);
        const auto accepted = workspace->outputImage(view.get(), view.get(), token, intent, image,
            [this, token](ui::OutputResult result, const QString& detail) {
                if (!view || token != outputToken) return;
                outputPending = false;
                const QPointer<ScrollController> lifetime(owner);
                if (result == ui::OutputResult::Succeeded) {
                    Q_EMIT owner->archiveChanged();
                    if (lifetime) close();
                } else if (closing) cleanupView();
                else {
                    if (!detail.isEmpty()) status(detail);
                    refresh();
                }
            });
        if (lifetime && !accepted) { outputPending = false; status(QStringLiteral("long.outputBusy")); refresh(); }
    }
    void start()
    {
        if (!view || running || preparing || closing || sourceInvalid || outputPending || (manual && !reader)) return;
        running = true; stop = {}; const auto current = ++generation;
        const QPointer<ScrollController> lifetime(owner);
        refresh(); notify();
        if (!lifetime) return;
        auto* watcher = new QFutureWatcher<ScrollResult>(owner);
        QObject::connect(watcher, &QFutureWatcher<ScrollResult>::finished, owner, [this, watcher, current] {
            ScrollResult result;
            try { result = watcher->result(); }
            catch (const std::exception& error) { result.detail = QString::fromUtf8(error.what()); result.failure = true; }
            catch (...) { result.detail = QStringLiteral("The scrolling worker failed"); result.failure = true; }
            watcher->deleteLater();
            if (current != generation) return;
            running = false;
            nativeCaptureHidden = false;
            if (!interruption.isEmpty()) { result.detail = interruption; result.failure = true; }
            if (stream) stream->stop();
            if (!result.image.isNull()) image = std::move(result.image);
            if (view) {
                status(result.detail);
                if (!image.isNull()) view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(image.width()).arg(image.height()));
            }
            const QPointer<ScrollController> lifetime(owner);
            if (result.failure && !closing) Q_EMIT owner->operationFailed(message(result.detail));
            if (!lifetime) return;
            if (closing) { cleanupView(); return; }
            refresh();
            if (activationPending) { activationPending = false; activate(std::exchange(activationToken, {})); }
            if (!lifetime) return;
            if (afterStop) { const auto intent = *afterStop; afterStop.reset(); output(intent); }
        });
        watcher->setFuture(QtConcurrent::run(&pool,
            [this, current, region = region, source = std::move(reader), token = stop.get_token(), contract = sourceContract]() mutable {
                const auto gate = std::make_shared<FrameGate>();
                auto prepare = [this, current, gate, token] {
                    quint64 request;
                    { std::lock_guard lock(gate->mutex); request = ++gate->request; }
                    QMetaObject::invokeMethod(owner, [this, current, gate, request] {
                        if (current != generation || !view || closing) return;
                        const bool overlapping = std::any_of(logicalCaptureRegions.begin(), logicalCaptureRegions.end(),
                            [this](const auto& rectangle) { return rectangle.intersects(view->frameGeometry()); });
                        if (overlapping) { view->hide(); gate->hidden = nativeCaptureHidden = true; }
                        auto acknowledge = [gate, request] {
                            { std::lock_guard lock(gate->mutex); gate->acknowledged = request; }
                            gate->completed.notify_all();
                        };
                        if (overlapping) QTimer::singleShot(40, owner, std::move(acknowledge));
                        else acknowledge();
                    }, ::Qt::QueuedConnection);
                    std::unique_lock lock(gate->mutex);
                    gate->completed.wait_for(lock, token, 2s, [gate, request] { return gate->acknowledged >= request; });
                    return !token.stop_requested() && gate->acknowledged >= request;
                };
                auto restore = [this, current, gate] {
                    QMetaObject::invokeMethod(owner, [this, current, gate] {
                        if (!gate->hidden) return;
                        gate->hidden = false;
                        if (current != generation || !view || closing) return;
                        nativeCaptureHidden = false;
                        view->setAttribute(::Qt::WA_ShowWithoutActivating); view->show();
                        if (activationPending) { activationPending = false; activate(std::exchange(activationToken, {})); }
                    }, ::Qt::QueuedConnection);
                };
                return runScroll(region, std::move(source), token, [this, current](Progress progress) {
                    QMetaObject::invokeMethod(owner, [this, current, progress = std::move(progress)] {
                        if (current != generation || !view || closing) return;
                        view->preview->image = progress.preview; view->preview->update();
                        view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(progress.width).arg(progress.height));
                        if (!progress.detail.isEmpty()) status(progress.detail);
                    }, ::Qt::QueuedConnection);
                }, std::move(prepare), std::move(restore), contract);
            }));
    }
    void authorize()
    {
        preparing = true; refresh();
        stream = new Capture::ScreenCastSource(owner);
        QObject::connect(stream, &Capture::ScreenCastSource::failed, owner, [this](const Capture::Failure& failure) {
            if (view) status(failure.detail);
            if (running) stop.request_stop();
            if (!closing && failure.code != Capture::Failure::Code::Cancelled) Q_EMIT owner->operationFailed(failure.detail);
        });
        QObject::connect(stream, &Capture::ScreenCastSource::stopped, owner, [this] {
            preparing = false;
            auto* finished = std::exchange(stream, nullptr); if (finished) finished->deleteLater();
            if (closing) cleanupView(); else { refresh(); notify(); }
        });
        QObject::connect(stream, &Capture::ScreenCastSource::ready, owner, [this](const Core::FrameLease& frame) {
            if (!view || closing) { stream->stop(); return; }
            const auto size = Recording::normalizedFrameSize(frame);
            QImage previewImage;
            if (size) previewImage = QImage(size->first, size->second, QImage::Format_ARGB32);
            if (previewImage.isNull() || !Recording::copyFrameBgra(frame, {0, 0, size->first, size->second},
                {previewImage.bits(), static_cast<std::size_t>(previewImage.sizeInBytes())}, static_cast<std::size_t>(previewImage.bytesPerLine()))) {
                status(QStringLiteral("The authorized source cannot be normalized to an SDR image")); stream->stop(); return;
            }
            sourceContract = SourceContract{frame.format(), frame.metadata().crop, frame.metadata().transform,
                frame.formatGeneration(), frame.metadata().sessionId, frame.metadata().sourceId};
            Capture::Snapshot snapshot;
            snapshot.image = std::move(previewImage); snapshot.sourceId = frame.metadata().sourceId;
            snapshot.generation = frame.formatGeneration(); snapshot.coordinates = Capture::CoordinateSpace::AuthorizedSourcePixels;
            snapshot.cursor = {snapshot.image.width() / 2, snapshot.image.height() / 2};
            picker = std::make_unique<ui::CaptureWindow>(); picker->setTranslator(translator);
            picker->session()->setCapability(Capture::Action::Scroll, {Capture::CapabilityState::Available, {}});
            QObject::connect(picker->session(), &Capture::CaptureSession::actionRequested, owner,
                [this](const Capture::ActionRequest& request) {
                    if (!picker || !stream || request.action != Capture::Action::Scroll) return;
                    region = request.sourceRect; image = request.image; reader = stream->takeFrameSource();
                    if (!reader) { picker->session()->complete(request.token, Capture::ActionResult::Failed, QStringLiteral("The authorized stream is no longer available")); return; }
                    preparing = false;
                    view->preview->image = image; view->preview->update();
                    view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(image.width()).arg(image.height()));
                    status(QStringLiteral("long.manualReady"));
                    picker->session()->complete(request.token, Capture::ActionResult::Completed);
                    view->show(); view->raise(); refresh();
                });
            QObject::connect(picker.get(), &ui::CaptureWindow::closed, owner, [this] {
                if (preparing && stream) stream->stop();
                if (view && !closing) view->show();
            });
            view->hide();
            const auto failure = picker->showSnapshot(std::move(snapshot), {Capture::Action::Scroll});
            if (failure) { view->show(); status(failure.detail); stream->stop(); }
        });
        status(QStringLiteral("long.authorize"));
        view->winId();
        if (!stream->start(view->windowHandle(), false, 8)) {
            preparing = false; stream->deleteLater(); stream = nullptr;
            status(QStringLiteral("The screen-sharing request could not start")); refresh(); notify();
        }
    }
};

ScrollController::ScrollController(std::shared_ptr<Media::MediaArchiveService> archive, MediaWorkspace& workspace, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), workspace)) {}
ScrollController::~ScrollController()
{
    state_->stopping = state_->closing = true;
    if (state_->stream) disconnect(state_->stream, nullptr, this, nullptr);
    if (state_->picker) {
        disconnect(state_->picker.get(), nullptr, this, nullptr);
        disconnect(state_->picker->session(), nullptr, this, nullptr);
        state_->picker->shutdown();
    }
    state_->stop.request_stop(); state_->reader.reset();
    if (state_->stream) state_->stream->stop();
    state_->pool.waitForDone();
    if (state_->view) {
        auto* identity = state_->view.get(); state_->view->dismiss = {}; state_->view->allowedClose = true;
        state_->view.reset(); state_->workspace->cancelOutput(identity, state_->outputToken);
    }
}
void ScrollController::setTranslator(ui::DesktopTranslator translator)
{ state_->translator = std::move(translator); if (state_->picker) state_->picker->setTranslator(state_->translator); state_->refresh(); }
bool ScrollController::begin(const Capture::ActionRequest& request, QWidget* parent)
{
    Q_UNUSED(parent);
    if (state_->stopping || state_->busy() || !state_->archive || request.action != Capture::Action::Scroll || request.image.isNull()) return false;
    const bool manual = request.coordinates == Capture::CoordinateSpace::AuthorizedSourcePixels;
    if (!manual && (!request.desktopRect || request.desktopRect->isEmpty())) return false;
    state_->manual = manual; state_->image = request.image; state_->region = request.desktopRect.value_or(request.sourceRect);
    state_->sourceInvalid = false; state_->interruption.clear();
    state_->sourceContract.reset();
    state_->logicalCaptureRegions.clear();
    state_->closing = false; state_->view = std::make_unique<ScrollWindow>();
    auto* view = state_->view.get(); view->preview->image = request.image;
    std::optional<QPoint> initialPosition;
    for (const auto& screen : request.screens) {
        const auto source = request.sourceRect.intersected(screen.sourcePixels);
        if (source.isEmpty()) continue;
        const auto first = Capture::screenLocalFromSource(screen, source.topLeft()) + screen.logicalGeometry.topLeft();
        const auto last = Capture::screenLocalFromSource(screen, QPointF(source.x() + source.width(), source.y() + source.height())) + screen.logicalGeometry.topLeft();
        const QRectF selected(first, last); state_->logicalCaptureRegions.push_back(selected);
        const auto area = screen.logicalWorkArea;
        const std::array freeAreas{
            QRectF(selected.right() + 8, area.top(), area.right() - selected.right() - 8, area.height()),
            QRectF(area.left(), area.top(), selected.left() - area.left() - 8, area.height()),
            QRectF(area.left(), selected.bottom() + 8, area.width(), area.bottom() - selected.bottom() - 8),
            QRectF(area.left(), area.top(), area.width(), selected.top() - area.top() - 8)};
        if (!initialPosition) for (const auto& free : freeAreas) {
            if (free.width() >= view->width() && free.height() >= view->height()) {
                initialPosition = free.topLeft().toPoint(); break;
            }
        }
    }
    if (!manual && state_->logicalCaptureRegions.isEmpty()) {
        // A caller without verified screen bindings cannot safely place tools
        // outside physical pixels. Reject admission, retaining its source view.
        state_->view.reset(); state_->image = {}; return false;
    }
    if (!manual) {
        auto invalidate = [this] {
            if (!state_->view || state_->closing || state_->manual || state_->sourceInvalid) return;
            state_->sourceInvalid = true;
            state_->interruption = QStringLiteral("long.displayChanged");
            state_->stop.request_stop(); state_->status(state_->interruption); state_->refresh();
        };
        for (const auto& screen : request.screens) if (screen.screen) {
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::geometryChanged, this, invalidate));
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::physicalDotsPerInchChanged, this, invalidate));
        }
        state_->screenConnections.push_back(connect(qGuiApp, &QGuiApplication::screenRemoved, this, invalidate));
        state_->screenConnections.push_back(connect(qGuiApp, &QGuiApplication::screenAdded, this, invalidate));
    }
    if (initialPosition) view->move(*initialPosition);
    view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(request.image.width()).arg(request.image.height()));
    state_->status(QStringLiteral("long.ready"));
    view->dismiss = [this] { state_->close(); };
    connect(view->start, &QPushButton::clicked, this, [this] { state_->start(); });
    connect(view->stop, &QPushButton::clicked, this, [this] { state_->stop.request_stop(); });
    connect(view->pin, &QPushButton::clicked, this, [this] { state_->output(ui::OutputIntent::Pin); });
    connect(view->copy, &QPushButton::clicked, this, [this] { state_->output(ui::OutputIntent::Clipboard); });
    connect(view->save, &QPushButton::clicked, this, [this] { state_->output(ui::OutputIntent::SaveAs); });
    connect(view->close, &QPushButton::clicked, this, [this] { state_->close(); });
    auto* save = new QShortcut(QKeySequence::Save, view);
    connect(save, &QShortcut::activated, this, [this] { state_->output(ui::OutputIntent::SaveAs); });
    auto* copy = new QShortcut(QKeySequence::Copy, view);
    connect(copy, &QShortcut::activated, this, [this] { state_->output(ui::OutputIntent::Clipboard); });
    auto* escape = new QShortcut(QKeySequence(::Qt::Key_Escape), view);
    connect(escape, &QShortcut::activated, this, [this] { state_->close(); });
    const QPointer<ScrollController> lifetime(this);
    state_->refresh(); view->show(); view->raise(); view->activateWindow(); state_->notify();
    if (!lifetime) return true;
    if (manual) QTimer::singleShot(0, this, [this] { if (state_->view && !state_->closing) state_->authorize(); });
    return true;
}
bool ScrollController::busy() const noexcept { return state_->busy(); }
void ScrollController::activate(const QString& activationToken) { state_->activate(activationToken); }
void ScrollController::shutdown()
{
    if (state_->stopping) return;
    const QPointer<ScrollController> lifetime(this);
    state_->stopping = true; state_->close();
    if (lifetime) state_->notify();
}

} // namespace HuhuCapture::Application
