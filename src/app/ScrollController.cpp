#include "app/ScrollController.h"

#include "app/MediaWorkspace.h"
#include "app/ScrollProgress.h"
#include "capture/ScrollAccumulator.h"
#include "core/ImagePreview.h"
#include "platform/NativeScrollSource.h"
#include "platform/WindowActivation.h"
#include "recording/FramePixels.h"
#include "recording/PlatformRecording.h"
#include "services/capture/ScreenCastSource.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/DesktopTheme.h"
#include "ui/qt/ToolbarControls.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QShowEvent>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QRegion>
#include <QResizeEvent>
#include <QPointer>
#include <QToolButton>
#include <QScrollArea>
#include <QShortcut>
#include <QThreadPool>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
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
    explicit ScrollPreview(QWidget* parent) : QWidget(parent) { setObjectName(QStringLiteral("scrollPreview")); setMinimumSize(1, 80); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(235, 236, 239));
        if (image.isNull()) return;
        const auto size = image.size().scaled(this->size() - QSize(12, 12), ::Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((width() - size.width()) / 2, height() - size.height() - 6), size), image);
    }
};
// Start/outline surface reused from PR #6 ba9358443207b9fa349de2bc0f47cebc0b13aa5c,
// MIT, NAMEWTA. It is a view of this controller, with no second capture session.
class ScrollStartSurface final : public QWidget {
public:
    Capture::ScreenBinding binding;
    QRectF selected;
    QString label;
    QPointF pointer;
    std::function<void()> start, cancel;
    bool hovered{}, outline{};
    ScrollStartSurface(Capture::ScreenBinding screen, QRect source)
        : QWidget(nullptr, ::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint | ::Qt::BypassWindowManagerHint | ::Qt::NoDropShadowWindowHint), binding(std::move(screen))
    {
        setObjectName(QStringLiteral("scrollStartSurface")); setAttribute(::Qt::WA_TranslucentBackground);
        setAttribute(::Qt::WA_ShowWithoutActivating); setMouseTracking(true); setFocusPolicy(::Qt::StrongFocus);
        setScreen(binding.screen); setGeometry(binding.logicalGeometry.toAlignedRect());
        selected = QRectF(Capture::screenLocalFromSource(binding, source.topLeft()),
            Capture::screenLocalFromSource(binding, QPointF(source.x() + source.width(), source.y() + source.height())));
        setMask(QRegion(selected.intersected(rect()).toAlignedRect()));
        setFont(ui::DesktopTheme::font(16));
    }
    void showOutline()
    {
        outline = true; hovered = false; unsetCursor();
        // Keep the region visible without covering or intercepting its source
        // pixels. Use the same verified binding as the start interaction.
        setWindowFlag(::Qt::WindowTransparentForInput, true);
        const auto inner = selected.adjusted(-1, -1, 1, 1).toAlignedRect();
        const auto border = (QRegion(inner.adjusted(-4, -4, 4, 4)) - QRegion(inner)) & QRegion(rect());
        if (border.isEmpty()) { hide(); return; }
        setMask(border);
        setAttribute(::Qt::WA_ShowWithoutActivating); show(); update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(22, 119, 255), 2)); painter.setBrush(::Qt::NoBrush);
        painter.drawRect(outline ? selected.adjusted(-3, -3, 3, 3) : selected.adjusted(1, 1, -1, -1));
        if (!hovered) return;
        painter.setPen(::Qt::NoPen); painter.setBrush(QColor(0, 0, 0, 174)); painter.drawEllipse(pointer, 30, 30);
        painter.setPen(::Qt::white); painter.drawText(QRectF(pointer.x() - 30, pointer.y() - 30, 60, 60), ::Qt::AlignCenter, label);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        pointer = event->position(); hovered = selected.contains(pointer);
        setCursor(hovered ? ::Qt::BlankCursor : ::Qt::ArrowCursor); update();
    }
    void leaveEvent(QEvent*) override { hovered = false; unsetCursor(); update(); }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == ::Qt::RightButton) { const auto callback = cancel; if (callback) callback(); return; }
        if (event->button() == ::Qt::LeftButton && selected.contains(event->position())) {
            const auto callback = start; if (callback) callback();
        }
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == ::Qt::Key_Escape && cancel) { const auto callback = cancel; callback(); return; }
        if ((event->key() == ::Qt::Key_Space || event->key() == ::Qt::Key_Return) && hovered && start) { const auto callback = start; callback(); return; }
        QWidget::keyPressEvent(event);
    }
};

class ScrollWindow final : public QWidget {
public:
    ScrollPreview* preview{};
    QLabel *status{}, *dimensions{};
    QToolButton *start{}, *stop{}, *pin{}, *copy{}, *save{}, *close{};
    ui::ToolbarControls::ToolbarStrip *setup{}, *actions{};
    QWidget* grip{};
    std::function<void()> dismiss;
    bool allowedClose{}, captureHidden{}, correctingGeometry{};
    QVector<QRect> safeAreas;
    std::function<void()> placementLost;
    bool constrain()
    {
        if (safeAreas.isEmpty()) return true;
        std::optional<QRect> closest; double distance{};
        for (const auto& area : safeAreas) {
            const auto frame = frameGeometry();
            if (frame.width() > area.width() || frame.height() > area.height()) continue;
            const QPoint position((std::clamp)(frame.x(), area.left(), area.x() + area.width() - frame.width()),
                (std::clamp)(frame.y(), area.top(), area.y() + area.height() - frame.height()));
            const auto delta = position - frame.topLeft();
            const double squared = double(delta.x()) * delta.x() + double(delta.y()) * delta.y();
            if (!closest || squared < distance) { closest = QRect(position, frame.size()); distance = squared; }
        }
        if (!closest) {
            hide();
            if (placementLost) QTimer::singleShot(0, this, [this] { if (!safeAreas.isEmpty() && placementLost) placementLost(); });
            return false;
        }
        if (frameGeometry().topLeft() != closest->topLeft()) {
            correctingGeometry = true; move(pos() + closest->topLeft() - frameGeometry().topLeft()); correctingGeometry = false;
        }
        return true;
    }
    void hideForCapture() { captureHidden = true; hide(); }
    void restore() { captureHidden = false; safeAreas.clear(); show(); }
    ScrollWindow() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        namespace toolbar = ui::ToolbarControls;
        setObjectName(QStringLiteral("scrollCaptureWindow")); setWindowTitle(QStringLiteral("huhucapture · Scrolling capture"));
        setFocusPolicy(Qt::StrongFocus); ui::DesktopTheme::apply(this);
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        auto* viewport = new QScrollArea(this); viewport->setWidgetResizable(true); viewport->setFrameShape(QFrame::NoFrame);
        viewport->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); viewport->setMinimumHeight(40);
        auto* content = new QWidget(viewport); auto* body = new QVBoxLayout(content);
        body->setContentsMargins(0, 0, 0, 0); body->setSpacing(0); viewport->setWidget(content); layout->addWidget(viewport, 1);
        status = ui::DesktopTheme::label(content, 12, true); status->setObjectName(QStringLiteral("scrollStatus"));
        status->setWordWrap(true); status->setMargin(8); body->addWidget(status);
        dimensions = ui::DesktopTheme::label(content, 12, true); dimensions->setObjectName(QStringLiteral("scrollDimensions"));
        dimensions->setAlignment(Qt::AlignCenter); body->addWidget(dimensions);
        preview = new ScrollPreview(content); preview->setFixedHeight(180); body->addWidget(preview);
        setup = new toolbar::ToolbarStrip(this); layout->addWidget(setup);
        grip = new toolbar::ToolbarDragHandle(setup); setup->add(grip, 16);
        const auto family = toolbar::iconFontFamily();
        start = toolbar::makeButton(setup, QStringLiteral("long.start"), u'\ue660', family);
        start->setObjectName(QStringLiteral("scrollStart")); setup->add(start);
        stop = new QToolButton(setup); stop->setObjectName(QStringLiteral("scrollStop"));
        stop->setIcon(style()->standardIcon(QStyle::SP_MediaStop)); stop->setIconSize({toolbar::iconSize, toolbar::iconSize});
        stop->setFocusPolicy(Qt::NoFocus); setup->add(stop);
        actions = new toolbar::ToolbarStrip(this); actions->setObjectName(QStringLiteral("scrollOutputActions")); layout->addWidget(actions);
        const auto make = [&](const char* name, const char* key, char16_t glyph) {
            auto* button = toolbar::makeButton(actions, QString::fromLatin1(key), glyph, family);
            button->setObjectName(QString::fromLatin1(name)); actions->add(button); return button;
        };
        pin = make("scrollPin", "tool.pin", u'\ue6a2'); close = make("scrollClose", "tool.close", u'\ue62d');
        save = make("scrollSave", "tool.save", u'\ue608'); copy = make("scrollCopy", "tool.clipboard", u'\ue6ad');
        resize(240, 360); setMinimumSize(toolbar::buttonSize + 2, 120);
    }

protected:
    void showEvent(QShowEvent* event) override
    { if (captureHidden) { hide(); return; } QWidget::showEvent(event); constrain(); }
    void moveEvent(QMoveEvent* event) override
    { QWidget::moveEvent(event); if (!correctingGeometry) constrain(); }
    void resizeEvent(QResizeEvent* event) override
    { QWidget::resizeEvent(event); if (!correctingGeometry) constrain(); }
    void closeEvent(QCloseEvent* event) override
    { if (!allowedClose && dismiss) { event->ignore(); dismiss(); } else event->accept(); }
};
struct ScrollResult { QImage image; QString detail; bool failure{}; };
using Progress = Detail::ScrollProgress;
struct SourceContract {
    Core::FrameFormat format;
    std::optional<Core::FrameCrop> crop;
    Core::FrameTransform transform{};
    quint64 generation{}, sessionId{}, sourceId{};
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
    std::shared_ptr<Core::CaptureControls> controls, std::optional<SourceContract> expectedSource)
{
    struct StopSource {
        std::unique_ptr<Core::FrameSource>& source;
        ~StopSource() { if (source) source->stopAndWait(); }
    } stopSource{stream};
    const bool manual = static_cast<bool>(stream);
    std::unique_ptr<Platform::NativeScrollSource> native;
    if (!manual) native = std::make_unique<Platform::NativeScrollSource>(region.x(), region.y(), region.width(), region.height(), stop, std::move(controls));
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
            const auto captured = native->capture();
            if (stop.stop_requested()) return false;
            if (!captured) { error = QString::fromStdString(native->error()); return false; }
            frame = native->pixels(); return true;
        }
        const auto deadline = Core::MediaClock::now() + (first ? 5s : 1s);
        while (!stop.stop_requested()) {
            auto next = stream->read(deadline, stop);
            if (next.status == Core::FrameReadStatus::Stopped) return false;
            if (next.status == Core::FrameReadStatus::Paused) {
                error = QStringLiteral("The authorized scrolling source paused. The last complete image is retained.");
                return false;
            }
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
    bool failed{}; QString detail;
    try {
      publish(manual ? QStringLiteral("long.manualCapturing") : QStringLiteral("long.capturing"));
      auto wait = 88ms; bool wheel = !manual;
      while (delay(stop, wait)) {
        if (wheel) {
            const auto input = native->scroll();
            if (stop.stop_requested()) break;
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
    } catch (const std::bad_alloc&) {
        failed = true; detail = QStringLiteral("The scrolling capture reached its memory limit. The last complete image is retained.");
    } catch (const std::exception& exception) {
        failed = true; detail = QString::fromUtf8(exception.what());
    } catch (...) {
        failed = true; detail = QStringLiteral("The scrolling source failed. The last complete image is retained.");
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
    std::vector<std::unique_ptr<ScrollStartSurface>> surfaces;
    QPointer<Capture::ScreenCastSource> stream;
    std::unique_ptr<Core::FrameSource> reader;
    std::shared_ptr<Core::CaptureControls> controls;
    QThreadPool pool;
    std::shared_ptr<Detail::ScrollProgressMailbox> progressMailbox;
    std::stop_source stop;
    QImage image;
    Capture::ActionRequest selection;
    QRect region;
    quint64 generation{}, outputToken{};
    bool running{}, manual{}, preparing{}, closing{}, stopping{}, outputPending{}, wasBusy{};
    bool sourceInvalid{}, sourceReady{}, shutdownReported{}, guardingControls{}, externalStopAvailable{};
    bool waitingRefresh{}, refreshStarted{}, nativePending{}, activationPending{};
    QString externalStopHint, activationToken, interruption, statusContent;
    std::optional<SourceContract> sourceContract;
    std::vector<QMetaObject::Connection> screenConnections;
    std::vector<QPointer<QWindow>> hiddenSurfaces;
    QTimer preparation;
    Core::MediaClock::time_point preparationDeadline;
    std::optional<ui::OutputIntent> afterStop;
    State(ScrollController* value, std::shared_ptr<Media::MediaArchiveService> service, MediaWorkspace& media)
        : owner(value), archive(std::move(service)), workspace(&media)
    {
        pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1);
        preparation.setInterval(20);
        QObject::connect(&preparation, &QTimer::timeout, owner, [this] { admitAfterUnmap(); });
    }
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
            Entry{"long.manualReady", "Press Start, scroll the authorized source manually, then use the available finish action."},
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
    QString displayedStatus() const
    {
        auto value = message(statusContent);
        if (externalStopAvailable && !externalStopHint.isEmpty() && (manual || (view && view->captureHidden)))
            value += QChar(u'\n') + externalStopHint;
        else if (manual && !externalStopAvailable)
            value += QChar(u'\n') + text(u"capture.stopUnavailable", QStringLiteral("A working tray, shortcut or desktop finish action is required before controls can hide."));
        return value;
    }
    void status(QString content)
    { statusContent = std::move(content); if (view) view->status->setText(displayedStatus()); }
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
        if (view->captureHidden) { activationPending = true; activationToken = std::move(token); return; }
        auto* target = picker && picker->visible() ? picker->dialogParent() : view.get();
        if (!target) return;
        const QPointer<QWidget> window(target);
        target->setAttribute(Qt::WA_ShowWithoutActivating); target->show(); target->raise();
        if (!window) return;
        if (auto* native = window->windowHandle()) {
            Platform::applyActivationToken(native, token); native->requestActivate();
        }
    }
    void refresh()
    {
        if (!view) return;
        view->status->setText(displayedStatus());
        view->setWindowTitle(QStringLiteral("huhucapture · ") + text(u"long.title", QStringLiteral("Scrolling capture")));
        view->start->setText(text(u"long.start", QStringLiteral("Start")));
        view->stop->setText(text(u"long.stop", QStringLiteral("Stop")));
        view->pin->setText(text(u"tool.pin", QStringLiteral("Pin")));
        view->copy->setText(text(u"tool.clipboard", QStringLiteral("Copy")));
        view->save->setText(text(u"tool.save", QStringLiteral("Save as")));
        view->close->setText(text(u"tool.close", QStringLiteral("Close")));
        view->start->setEnabled(!running && !preparing && !closing && !sourceInvalid && !outputPending && !image.isNull() && (!manual || (sourceReady && externalStopAvailable)));
        view->stop->setEnabled((running || waitingRefresh || nativePending) && !closing);
        for (auto* button : {view->pin, view->copy, view->save}) button->setEnabled(!preparing && !closing && !outputPending && !image.isNull());
        view->close->setEnabled(!closing);
        for (auto* button : {view->start, view->stop, view->pin, view->copy, view->save, view->close}) {
            button->setAccessibleName(button->text());
            button->setToolTip(guardingControls || preparing || running ? QString() : button->text());
        }
        view->setup->refreshTextWidths(); view->actions->refreshTextWidths();
        for (const auto& surface : surfaces) { surface->label = text(u"long.start", QStringLiteral("Start")); surface->update(); }
    }
    void protect(QWindow* native)
    {
        if (!native || !native->handle()) return;
        native->installEventFilter(owner);
        const bool excluded = Recording::excludeControlsFromCapture(static_cast<std::uintptr_t>(native->winId())) ==
            Recording::ControlsExclusion::Excluded;
        native->setProperty("huhuCaptureExcluded", excluded);
    }
    bool publishControls()
    {
        if (!guardingControls || !controls || !view || !view->windowHandle() || !view->windowHandle()->handle()) return false;
        auto snapshot = std::make_shared<Core::CaptureControlSnapshot>();
        snapshot->surfaces.push_back({static_cast<std::uintptr_t>(view->winId()), view->captureHidden ?
            Core::CaptureControlPlacement::Hidden : view->windowHandle()->property("huhuCaptureExcluded").toBool() ?
                Core::CaptureControlPlacement::Excluded : Core::CaptureControlPlacement::Outside});
        for (const auto& surface : surfaces) {
            auto* native = surface->windowHandle();
            if (!native || !native->handle()) return false;
            snapshot->surfaces.push_back({static_cast<std::uintptr_t>(native->winId()), !surface->isVisible() ?
                Core::CaptureControlPlacement::Hidden : native->property("huhuCaptureExcluded").toBool() ?
                    Core::CaptureControlPlacement::Excluded : Core::CaptureControlPlacement::Outside});
        }
        controls->snapshot.store(std::move(snapshot), std::memory_order_release);
        return true;
    }
    QVector<QRect> outsideAreas() const
    {
        QVector<QRect> result;
        for (const auto& binding : selection.screens) {
            if (!binding.screen || binding.screen->name().compare(binding.nativeId,
#ifdef Q_OS_WIN
                Qt::CaseInsensitive
#else
                Qt::CaseSensitive
#endif
                ) != 0 || binding.logicalGeometry != QRectF(binding.screen->geometry()) ||
                binding.logicalWorkArea != QRectF(binding.screen->availableGeometry()) || binding.sourcePixels.isEmpty() ||
                !std::isfinite(binding.pixelsPerDipX) || binding.pixelsPerDipX <= 0 ||
                !std::isfinite(binding.pixelsPerDipY) || binding.pixelsPerDipY <= 0) continue;
            const auto work = binding.logicalWorkArea.adjusted(4, 4, -4, -4);
            const auto first = Capture::screenLocalFromSource(binding, selection.sourceRect.topLeft()) + binding.logicalGeometry.topLeft();
            const auto end = Capture::screenLocalFromSource(binding, QPointF(selection.sourceRect.x() + selection.sourceRect.width(),
                selection.sourceRect.y() + selection.sourceRect.height())) + binding.logicalGeometry.topLeft();
            const auto blocked = QRectF(first, end).adjusted(-8, -8, 8, 8).intersected(work);
            const auto spaces = blocked.isEmpty() ? std::array<QRectF, 4>{work, {}, {}, {}} : std::array<QRectF, 4>{
                QRectF(work.left(), work.top(), work.width(), blocked.top() - work.top()),
                QRectF(work.left(), blocked.bottom(), work.width(), work.bottom() - blocked.bottom()),
                QRectF(work.left(), work.top(), blocked.left() - work.left(), work.height()),
                QRectF(blocked.right(), work.top(), work.right() - blocked.right(), work.height())};
            for (const auto& area : spaces) {
                const int left = static_cast<int>(std::ceil(area.left())), top = static_cast<int>(std::ceil(area.top()));
                const QRect inward(left, top, static_cast<int>(std::floor(area.right())) - left,
                    static_cast<int>(std::floor(area.bottom())) - top);
                if (!inward.isEmpty()) result.push_back(inward);
            }
        }
        return result;
    }
    void closeProgress()
    { if (progressMailbox) progressMailbox->close(); progressMailbox.reset(); }
    void restoreControls()
    {
        guardingControls = false; controls.reset(); preparation.stop(); hiddenSurfaces.clear();
        for (const auto& surface : surfaces) surface->hide();
        if (view && !closing) { view->setAttribute(Qt::WA_ShowWithoutActivating); view->restore(); }
    }
    void preparationFailed(const QString& detail)
    {
        interruption = detail; status(detail);
        if (running) { closeProgress(); stop.request_stop(); return; }
        preparing = waitingRefresh = refreshStarted = nativePending = sourceReady = false;
        preparation.stop(); reader.reset();
        if (picker) picker->shutdown();
        if (stream) stream->stop();
        restoreControls(); refresh();
        const QPointer<ScrollController> lifetime(owner);
        if (!closing) Q_EMIT owner->operationFailed(detail);
        if (lifetime) notify();
    }
    void cleanupView()
    {
        if (running || preparing || outputPending) return;
        closeProgress();
        guardingControls = false; controls.reset(); preparation.stop(); hiddenSurfaces.clear();
        const QPointer<ScrollController> lifetime(owner);
        if (picker) { picker->shutdown(); picker.reset(); }
        reader.reset();
        if (stream) stream->stop();
        if (!lifetime) return;
        surfaces.clear();
        if (view) { view->allowedClose = true; view->hide(); view.reset(); }
        image = {}; afterStop.reset(); closing = false; activationPending = false; activationToken.clear();
        waitingRefresh = refreshStarted = nativePending = sourceReady = false;
        for (const auto& connection : screenConnections) QObject::disconnect(connection);
        screenConnections.clear(); notify();
    }
    void close()
    {
        const QPointer<ScrollController> lifetime(owner);
        closing = true; afterStop.reset(); preparation.stop(); closeProgress();
        if (view && outputPending) workspace->cancelOutput(view.get(), outputToken);
        if (!lifetime) return;
        stop.request_stop();
        if (!running) preparing = waitingRefresh = refreshStarted = nativePending = sourceReady = false;
        if (picker) picker->cancel();
        if (!lifetime) return;
        reader.reset();
        if (stream && !running) stream->stop();
        if (!lifetime) return;
        for (const auto& surface : surfaces) surface->hide();
        if (view) { view->hide(); refresh(); }
        QTimer::singleShot(0, owner, [this] { if (!running && !preparing && !outputPending) cleanupView(); });
    }
    void output(ui::OutputIntent intent)
    {
        if (!view || closing || preparing || outputPending || image.isNull()) return;
        if (running) { afterStop = intent; closeProgress(); stop.request_stop(); refresh(); return; }
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
                else { if (!detail.isEmpty()) status(detail); refresh(); }
            });
        if (lifetime && !accepted) { outputPending = false; status(QStringLiteral("long.outputBusy")); refresh(); }
    }
    bool prepareControls()
    {
        if (!view || !view->windowHandle()) return false;
        hiddenSurfaces.clear();
        for (const auto& surface : surfaces) {
            if (surface->windowHandle()) hiddenSurfaces.push_back(surface->windowHandle());
            surface->hide();
        }
        for (auto* tooltip : QApplication::topLevelWidgets()) if (tooltip->windowType() == Qt::ToolTip) {
            if (tooltip->windowHandle()) hiddenSurfaces.push_back(tooltip->windowHandle());
            tooltip->hide();
        }
        protect(view->windowHandle());
        const bool excluded = !manual && view->windowHandle()->property("huhuCaptureExcluded").toBool();
        bool outside{};
        if (!excluded && !manual) {
            view->safeAreas = outsideAreas(); outside = !view->safeAreas.isEmpty() && view->constrain();
            if (outside) view->show();
        }
        if (!excluded && !outside) {
            view->safeAreas.clear();
            if (!externalStopAvailable) {
                preparationFailed(text(u"capture.stopUnavailable", QStringLiteral("A working tray, shortcut or desktop finish action is required before controls can hide.")));
                return false;
            }
            view->hideForCapture(); hiddenSurfaces.push_back(view->windowHandle());
        }
        controls = std::make_shared<Core::CaptureControls>(); guardingControls = true;
        if (!publishControls()) { preparationFailed(QStringLiteral("The scrolling controls have no verifiable native surface")); return false; }
        return true;
    }
    bool allSurfacesUnmapped() const
    {
        for (const auto& native : hiddenSurfaces) if (native && (native->isVisible() || native->isExposed())) return false;
        return !waitingRefresh || (view && view->captureHidden && !view->isVisible() &&
            view->windowHandle() && !view->windowHandle()->isExposed());
    }
    void admitAfterUnmap()
    {
        if (closing || !preparing || refreshStarted || (!nativePending && !waitingRefresh)) return;
        if (!allSurfacesUnmapped()) {
            if (Core::MediaClock::now() >= preparationDeadline)
                preparationFailed(text(u"long.hideFailed", QStringLiteral("The scrolling controls did not unmap before the capture deadline.")));
            return;
        }
        preparation.stop();
        if (waitingRefresh) {
            if (!externalStopAvailable || !stream) {
                preparationFailed(QStringLiteral("The authorized source or external finish action is no longer available")); return;
            }
            refreshStarted = true;
            if (!stream->refresh()) preparationFailed(QStringLiteral("The authorized scrolling source could not begin its capture refresh"));
        } else {
            // All old interior surfaces are actually unmapped before their
            // mask/window flags change. No worker exists during this switch.
            guardingControls = false;
            for (const auto& surface : surfaces) {
                surface->showOutline(); surface->winId(); protect(surface->windowHandle());
#ifdef Q_OS_WIN
                // Older Windows cannot validate a shaped fullscreen outline
                // with rectangle-only affinity fallback. Keep it hidden there.
                if (!surface->windowHandle()->property("huhuCaptureExcluded").toBool()) surface->hide();
#endif
            }
            guardingControls = true;
            if (!publishControls()) { preparationFailed(QStringLiteral("The scrolling outline has no verifiable native surface")); return; }
            startWorker();
        }
    }
    void start()
    {
        if (!view || running || preparing || closing || sourceInvalid || outputPending || (manual && !sourceReady)) return;
        preparing = true; stop = {}; interruption.clear();
        if (!prepareControls()) return;
        waitingRefresh = manual; nativePending = !manual; refreshStarted = false;
        if (manual) sourceReady = false;
        preparationDeadline = Core::MediaClock::now() + 2s;
        preparation.start(); refresh();
        const QPointer<ScrollController> lifetime(owner);
        admitAfterUnmap(); if (lifetime) notify();
    }
    void startWorker()
    {
        if (!view || closing || !preparing || (manual && !reader)) return;
        preparing = waitingRefresh = refreshStarted = nativePending = false;
        preparation.stop(); hiddenSurfaces.clear(); running = true; const auto current = ++generation;
        const QPointer<ScrollController> lifetime(owner);
        refresh(); notify();
        if (!lifetime) return;
        progressMailbox = std::make_shared<Detail::ScrollProgressMailbox>(*owner, [this, current](Progress progress) {
            if (current != generation || !view || closing) return;
            view->preview->image = std::move(progress.preview); view->preview->update();
            view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(progress.width).arg(progress.height));
            if (!progress.detail.isEmpty()) status(progress.detail);
        });
        auto* watcher = new QFutureWatcher<ScrollResult>(owner);
        QObject::connect(watcher, &QFutureWatcher<ScrollResult>::finished, owner, [this, watcher, current] {
            ScrollResult result;
            try { result = watcher->result(); }
            catch (const std::exception& error) { result.detail = QString::fromUtf8(error.what()); result.failure = true; }
            catch (...) { result.detail = QStringLiteral("The scrolling worker failed"); result.failure = true; }
            watcher->deleteLater();
            if (current != generation) return;
            closeProgress(); running = false;
            if (!interruption.isEmpty()) { result.detail = interruption; result.failure = true; }
            if (stream) stream->stop();
            if (!result.image.isNull()) image = std::move(result.image);
            restoreControls();
            if (view) {
                view->preview->image = image; view->preview->update(); status(result.detail);
                if (!image.isNull()) view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(image.width()).arg(image.height()));
            }
            const QPointer<ScrollController> lifetime(owner);
            if (result.failure && !closing) Q_EMIT owner->operationFailed(message(result.detail));
            if (!lifetime) return;
            if (closing) { cleanupView(); return; }
            refresh(); notify();
            if (!lifetime) return;
            if (activationPending) { activationPending = false; activate(std::exchange(activationToken, {})); }
            if (!lifetime) return;
            if (afterStop) { const auto intent = *afterStop; afterStop.reset(); output(intent); }
        });
        watcher->setFuture(QtConcurrent::run(&pool,
            [selected = region, source = std::move(reader), token = stop.get_token(),
                controlState = controls, contract = sourceContract, mailbox = progressMailbox]() mutable {
                struct CloseProgress {
                    std::shared_ptr<Detail::ScrollProgressMailbox>& mailbox;
                    ~CloseProgress() { mailbox->close(); }
                } close{mailbox};
                return runScroll(selected, std::move(source), token,
                    [mailbox](Progress progress) { mailbox->publish(std::move(progress)); },
                    std::move(controlState), contract);
            }));
    }
    void authorize()
    {
        preparing = true; sourceReady = false; refresh();
        stream = new Capture::ScreenCastSource(owner);
        QObject::connect(stream, &Capture::ScreenCastSource::failed, owner, [this](const Capture::Failure& failure) {
            if (closing) return;
            if (failure.code == Capture::Failure::Code::Cancelled && !running) {
                preparing = waitingRefresh = refreshStarted = nativePending = false;
                restoreControls(); status(failure.detail); refresh(); return;
            }
            preparationFailed(failure.detail);
        });
        QObject::connect(stream, &Capture::ScreenCastSource::stopped, owner, [this] {
            preparing = waitingRefresh = refreshStarted = sourceReady = false;
            auto finished = stream; stream.clear(); if (finished) finished->deleteLater();
            if (closing) cleanupView(); else { refresh(); notify(); }
        });
        QObject::connect(stream, &Capture::ScreenCastSource::refreshed, owner, [this](const Core::FrameLease& frame) {
            if (!preparing || !waitingRefresh || !refreshStarted || closing) return;
            const auto size = Recording::normalizedFrameSize(frame);
            if (!externalStopAvailable || !allSurfacesUnmapped() || !sourceContract || !size ||
                frame.format() != sourceContract->format || frame.metadata().crop != sourceContract->crop ||
                frame.metadata().transform != sourceContract->transform || frame.metadata().sessionId != sourceContract->sessionId ||
                frame.metadata().sourceId != sourceContract->sourceId || region.x() < 0 || region.y() < 0 ||
                region.x() + region.width() > size->first || region.y() + region.height() > size->second) {
                preparationFailed(QStringLiteral("The selected scrolling source or capture controls changed during refresh")); return;
            }
            sourceContract->generation = frame.formatGeneration();
            // Do not let a worker move the borrowed firstFrame while other
            // direct refreshed handlers are still observing it.
            const auto expected = stream;
            QTimer::singleShot(0, owner, [this, expected] {
                if (closing || !preparing || !waitingRefresh || !refreshStarted || !expected || stream != expected) return;
                if (!externalStopAvailable || !allSurfacesUnmapped()) {
                    preparationFailed(QStringLiteral("The scrolling stop route or controls changed before capture admission")); return;
                }
                reader = stream->takeFrameSource();
                if (!reader) { preparationFailed(QStringLiteral("The refreshed scrolling stream is no longer available")); return; }
                startWorker();
            });
        });
        QObject::connect(stream, &Capture::ScreenCastSource::ready, owner, [this](const Core::FrameLease& frame) {
            if (!view || closing) { if (stream) stream->stop(); return; }
            const auto size = Recording::normalizedFrameSize(frame);
            QImage previewImage;
            if (size) previewImage = QImage(size->first, size->second, QImage::Format_ARGB32);
            if (previewImage.isNull() || !Recording::copyFrameBgra(frame, {0, 0, size->first, size->second},
                {previewImage.bits(), static_cast<std::size_t>(previewImage.sizeInBytes())}, static_cast<std::size_t>(previewImage.bytesPerLine()))) {
                preparationFailed(QStringLiteral("The authorized source cannot be normalized to an SDR image")); return;
            }
            sourceContract = SourceContract{frame.format(), frame.metadata().crop, frame.metadata().transform,
                frame.formatGeneration(), frame.metadata().sessionId, frame.metadata().sourceId};
            Capture::Snapshot snapshot;
            snapshot.image = std::move(previewImage); snapshot.sourceId = frame.metadata().sourceId;
            snapshot.generation = frame.formatGeneration(); snapshot.coordinates = Capture::CoordinateSpace::AuthorizedSourcePixels;
            snapshot.cursor = {snapshot.image.width() / 2, snapshot.image.height() / 2};
            picker = std::make_unique<ui::CaptureWindow>(); picker->setTranslator(translator);
            QObject::connect(picker->session(), &Capture::CaptureSession::actionRequested, owner,
                [this](const Capture::ActionRequest& request) {
                    if (!picker || !stream || request.action != Capture::Action::Scroll || closing) return;
                    region = request.sourceRect; image = request.image; sourceReady = true; preparing = false;
                    view->preview->image = image; view->preview->update();
                    view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(image.width()).arg(image.height()));
                    status(QStringLiteral("long.manualReady"));
                    picker->hideForAction();
                    picker->session()->complete(request.token, Capture::ActionResult::Completed);
                    view->show(); view->raise(); refresh();
                });
            QObject::connect(picker.get(), &ui::CaptureWindow::closed, owner, [this] {
                if (preparing && !waitingRefresh && stream) stream->stop();
                if (view && !closing && !view->captureHidden) view->show();
            });
            view->hide();
            const auto failure = picker->showSnapshot(std::move(snapshot), {Capture::Action::Scroll});
            if (failure) { preparationFailed(failure.detail); return; }
            picker->session()->setCapability(Capture::Action::Scroll, {Capture::CapabilityState::Available, {}});
        });
        status(QStringLiteral("long.authorize")); view->winId();
        if (!stream->start(view->windowHandle(), false, 8)) {
            preparing = false; stream->deleteLater(); stream.clear();
            status(QStringLiteral("The screen-sharing request could not start")); refresh(); notify();
        }
    }
};

ScrollController::ScrollController(std::shared_ptr<Media::MediaArchiveService> archive, MediaWorkspace& workspace, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), workspace)) {}
ScrollController::~ScrollController()
{
    state_->stopping = state_->closing = true; state_->preparation.stop(); state_->closeProgress();
    if (state_->stream) disconnect(state_->stream, nullptr, this, nullptr);
    if (state_->picker) {
        disconnect(state_->picker.get(), nullptr, this, nullptr);
        disconnect(state_->picker->session(), nullptr, this, nullptr);
        state_->picker->shutdown();
    }
    state_->stop.request_stop(); state_->reader.reset();
    if (state_->stream) state_->stream->stop();
    state_->pool.waitForDone();
    state_->guardingControls = false; state_->controls.reset();
    if (state_->view) {
        auto* identity = state_->view.get(); state_->view->dismiss = {}; state_->view->placementLost = {}; state_->view->allowedClose = true;
        state_->view.reset(); state_->workspace->cancelOutput(identity, state_->outputToken);
    }
}
void ScrollController::setTranslator(ui::DesktopTranslator translator)
{ state_->translator = std::move(translator); if (state_->picker) state_->picker->setTranslator(state_->translator); state_->refresh(); }
bool ScrollController::begin(const Capture::ActionRequest& request, QWidget* parent)
{
    if (QThread::currentThread() != thread() || state_->stopping || state_->busy() || !state_->archive ||
        request.action != Capture::Action::Scroll || request.image.isNull()) return false;
    const bool manual = request.coordinates == Capture::CoordinateSpace::AuthorizedSourcePixels ||
        QGuiApplication::platformName().startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive) ||
        qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0;
    if (!manual && (!request.desktopRect || request.desktopRect->isEmpty() || request.screens.isEmpty())) return false;
    state_->manual = manual; state_->selection = request; state_->image = request.image;
    state_->region = request.desktopRect.value_or(request.sourceRect);
    state_->sourceInvalid = state_->sourceReady = false; state_->interruption.clear(); state_->sourceContract.reset();
    state_->closing = false; state_->view = std::make_unique<ScrollWindow>();
    auto* view = state_->view.get(); view->installEventFilter(this); view->grip->installEventFilter(this);
    for (auto* button : {view->start, view->stop, view->pin, view->copy, view->save, view->close}) button->installEventFilter(this);
    view->preview->image = request.image;
    if (!manual) {
        auto invalidate = [this] {
            if (!state_->view || state_->closing || state_->manual || state_->sourceInvalid) return;
            state_->sourceInvalid = true;
            state_->interruption = QStringLiteral("long.displayChanged");
            state_->closeProgress(); state_->stop.request_stop(); state_->status(state_->interruption); state_->refresh();
        };
        for (const auto& screen : request.screens) if (screen.screen) {
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::geometryChanged, this, invalidate));
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::availableGeometryChanged, this, invalidate));
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::physicalDotsPerInchChanged, this, invalidate));
        }
        state_->screenConnections.push_back(connect(qGuiApp, &QGuiApplication::screenRemoved, this, invalidate));
        state_->screenConnections.push_back(connect(qGuiApp, &QGuiApplication::screenAdded, this, invalidate));
    }
    if (parent && parent->screen()) view->setScreen(parent->screen());
    const auto work = view->screen()->availableGeometry().adjusted(8, 8, -8, -8);
    view->resize((std::max)(ui::ToolbarControls::buttonSize + 2, (std::min)(240, work.width())),
        (std::max)(120, (std::min)(360, work.height())));
    view->move(work.center() - view->rect().center());
    view->dimensions->setText(QStringLiteral("%1 × %2 px").arg(request.image.width()).arg(request.image.height()));
    state_->status(QStringLiteral("long.ready"));
    view->dismiss = [this] { state_->close(); };
    view->placementLost = [this] {
        if (!state_->guardingControls || !state_->view) return;
        state_->view->hideForCapture(); state_->publishControls();
        if (!state_->externalStopAvailable)
            state_->preparationFailed(state_->text(u"capture.stopUnavailable", QStringLiteral("The scrolling controls cannot remain safely accessible.")));
    };
    connect(view->start, &QToolButton::clicked, this, [this] { state_->start(); });
    connect(view->stop, &QToolButton::clicked, this, [this] { finish(); });
    connect(view->pin, &QToolButton::clicked, this, [this] { state_->output(ui::OutputIntent::Pin); });
    connect(view->copy, &QToolButton::clicked, this, [this] { state_->output(ui::OutputIntent::Clipboard); });
    connect(view->save, &QToolButton::clicked, this, [this] { state_->output(ui::OutputIntent::SaveAs); });
    connect(view->close, &QToolButton::clicked, this, [this] { state_->close(); });
    auto* save = new QShortcut(QKeySequence::Save, view);
    connect(save, &QShortcut::activated, this, [this] { state_->output(ui::OutputIntent::SaveAs); });
    auto* copy = new QShortcut(QKeySequence::Copy, view);
    connect(copy, &QShortcut::activated, this, [this] { state_->output(ui::OutputIntent::Clipboard); });
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* copyOnEnter = new QShortcut(QKeySequence(key), view);
        connect(copyOnEnter, &QShortcut::activated, this, [this] { state_->output(ui::OutputIntent::Clipboard); });
    }
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), view);
    connect(escape, &QShortcut::activated, this, [this] { state_->close(); });
    if (!manual) for (const auto& binding : request.screens) {
        if (!binding.screen || binding.logicalGeometry != QRectF(binding.screen->geometry()) ||
            !binding.sourcePixels.intersects(request.sourceRect)) continue;
        auto surface = std::make_unique<ScrollStartSurface>(binding, request.sourceRect);
        surface->start = [this] { state_->start(); };
        surface->cancel = [this] { state_->close(); };
        surface->installEventFilter(this); surface->winId(); state_->protect(surface->windowHandle()); surface->show();
        state_->surfaces.push_back(std::move(surface));
    }
    const QPointer<ScrollController> lifetime(this);
    state_->refresh(); view->winId(); state_->protect(view->windowHandle()); view->show(); view->raise(); view->activateWindow(); state_->notify();
    if (!lifetime) return true;
    if (manual) QTimer::singleShot(0, this, [this] { if (state_->view && !state_->closing) state_->authorize(); });
    return true;
}
void ScrollController::setExternalStopAvailable(bool available, const QString& hint)
{
    if (QThread::currentThread() != thread() || state_->stopping) return;
    state_->externalStopAvailable = available; state_->externalStopHint = hint; state_->refresh();
    if (!available && state_->guardingControls && state_->view && state_->view->captureHidden)
        state_->preparationFailed(state_->text(u"capture.stopUnavailable", QStringLiteral("The external scrolling finish action is no longer available.")));
}
bool ScrollController::finish()
{
    if (QThread::currentThread() != thread() || state_->stopping || !state_->view) return false;
    if (state_->running) { state_->closeProgress(); state_->stop.request_stop(); return true; }
    if (!state_->preparing) return false;
    state_->preparation.stop(); state_->preparing = state_->waitingRefresh = state_->refreshStarted = state_->nativePending = false;
    state_->sourceReady = false; state_->reader.reset();
    if (state_->picker) state_->picker->shutdown();
    if (state_->stream) state_->stream->stop();
    state_->restoreControls(); state_->status(QStringLiteral("long.finished")); state_->refresh(); state_->notify(); return true;
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
bool ScrollController::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ToolTip && (state_->guardingControls || state_->preparing || state_->running)) return true;
    if (state_->view && watched == state_->view->grip && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && state_->view->windowHandle()) {
            state_->view->windowHandle()->startSystemMove(); return true;
        }
    }
    if (event->type() == QEvent::PlatformSurface) {
        const auto phase = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
        if (phase == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && state_->guardingControls) {
            if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
            state_->preparationFailed(QStringLiteral("A scrolling control surface was replaced; the last complete image is retained."));
        } else if (phase == QPlatformSurfaceEvent::SurfaceCreated) {
            if (auto* native = qobject_cast<QWindow*>(watched)) state_->protect(native);
        }
    } else if (event->type() == QEvent::WinIdChange) {
        if (state_->guardingControls) {
            if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
            state_->preparationFailed(QStringLiteral("A scrolling control identifier changed; the last complete image is retained."));
        } else if (auto* widget = qobject_cast<QWidget*>(watched)) state_->protect(widget->windowHandle());
    }
    return QObject::eventFilter(watched, event);
}

} // namespace HuhuCapture::Application
