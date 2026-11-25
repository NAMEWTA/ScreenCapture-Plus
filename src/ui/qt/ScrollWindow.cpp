// Preview and stop-before-output flow adapted from this project's PR #7
// ScrollController, fixed 251bc0071278c6b711c840bf5188d16870bc7349.
// MIT, Copyright (c) 2026 NAMEWTA. See docs/scroll-capture.md.
#include "ui/qt/ScrollWindow.h"

#include "services/desktop/CaptureExclusion.h"
#include "ui/qt/DesktopTheme.h"
#include "ui/qt/ToolbarControls.h"
#include <QCloseEvent>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QShowEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace controls = ToolbarControls;
using Capture::ScrollPhase;
using Capture::ScrollNotice;
bool working(ScrollPhase phase)
{ return phase == ScrollPhase::Starting || phase == ScrollPhase::Capturing || phase == ScrollPhase::Stopping; }
class ScrollPreview final : public QWidget {
public:
    QImage image;
    explicit ScrollPreview(QWidget* parent) : QWidget(parent) { setObjectName(QStringLiteral("scrollPreview")); setMinimumHeight(80); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(235, 236, 239));
        if (image.isNull()) return;
        const auto size = image.size().scaled(this->size() - QSize(12, 12), ::Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((width() - size.width()) / 2, height() - size.height() - 6), size), image);
    }
};
class ScrollStartSurface final : public QWidget {
public:
    Capture::ScreenBinding binding;
    QRectF selected;
    QString label;
    QPointF pointer;
    std::function<void()> start, cancel;
    bool hovered{}, outline{};
    ScrollStartSurface(Capture::ScreenBinding screen, QRect source)
        : QWidget(nullptr, ::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint), binding(std::move(screen))
    {
        setObjectName(QStringLiteral("scrollStartSurface")); setAttribute(::Qt::WA_TranslucentBackground);
        setAttribute(::Qt::WA_ShowWithoutActivating); setMouseTracking(true); setFocusPolicy(::Qt::StrongFocus);
        setScreen(binding.screen); setGeometry(binding.logicalGeometry.toAlignedRect());
        selected = QRectF(Capture::screenLocalFromSource(binding, source.topLeft()),
            Capture::screenLocalFromSource(binding, QPointF(source.x() + source.width(), source.y() + source.height())));
        setMask(QRegion(selected.intersected(rect()).toAlignedRect()));
        setFont(DesktopTheme::font(16));
    }
    void showOutline()
    {
        outline = true; hovered = false; unsetCursor();
        // Keep the region visible without covering or intercepting its source
        // pixels. Use the same verified binding as the start interaction.
        setWindowFlag(::Qt::WindowTransparentForInput, true);
        const auto outside = QRegion(selected.adjusted(-4, -4, 4, 4).toAlignedRect());
        const auto border = outside.subtracted(QRegion(selected.toAlignedRect())).intersected(QRegion(rect()));
        if (border.isEmpty()) { hide(); return; }
        setMask(border);
        setAttribute(::Qt::WA_ShowWithoutActivating); show(); update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(22, 119, 255), 2)); painter.setBrush(::Qt::NoBrush);
        painter.drawRect(outline ? selected.adjusted(-2, -2, 2, 2) : selected.adjusted(1, 1, -1, -1));
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
}
struct ScrollWindow::State {
    QPointer<Capture::ScrollCaptureSession> session;
    DesktopTranslator translator;
    Desktop::WindowActivation activation;
    controls::ToolbarStrip *setup{}, *actions{};
    QToolButton *start{}, *stop{}, *pin{}, *close{}, *save{}, *copy{};
    QWidget* grip{};
    QLabel *status{}, *dimensions{}, *notice{};
    ScrollPreview* preview{};
    QVBoxLayout* layout{};
    std::vector<std::unique_ptr<ScrollStartSurface>> surfaces;
    QVector<QRectF> sourceRectangles;
    QTimer frameDeadline;
    quint64 nextOutput{}, outputToken{}, captureToken{}, sample{}, capturingSample{};
    std::optional<OutputIntent> afterStop;
    QString outputFailure;
    bool opened{}, closing{}, reportedClosed{}, pending{}, excluded{}, hiddenForCapture{};
    bool externalStopAvailable{}, pendingLiveStart{};

    QString text(QStringView key, const char* fallback) const
    {
        const auto translated = translator ? translator(key) : QString();
        return translated.isEmpty() || translated == key ? QString::fromUtf8(fallback) : translated;
    }
    bool manual() const { return session && session->selection().coordinates == Capture::CoordinateSpace::AuthorizedSourcePixels; }
};
ScrollWindow::ScrollWindow(Capture::ScrollCaptureSession& session, QWidget* parent)
    : QWidget(parent, ::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint), state_(std::make_unique<State>())
{
    state_->session = &session; setObjectName(QStringLiteral("scrollCaptureWindow"));
    setFocusPolicy(::Qt::StrongFocus); DesktopTheme::apply(this);
    state_->layout = new QVBoxLayout(this); state_->layout->setContentsMargins(0, 0, 0, 0); state_->layout->setSpacing(0);
    auto* viewport = new QScrollArea(this); viewport->setWidgetResizable(true); viewport->setFrameShape(QFrame::NoFrame);
    viewport->setHorizontalScrollBarPolicy(::Qt::ScrollBarAlwaysOff); viewport->setMinimumHeight(40);
    auto* content = new QWidget(viewport); auto* body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0); body->setSpacing(0); viewport->setWidget(content);
    state_->layout->addWidget(viewport, 1);
    state_->status = DesktopTheme::label(content, 12); state_->status->setWordWrap(true); state_->status->setMargin(8);
    state_->status->setObjectName(QStringLiteral("scrollStatus")); body->addWidget(state_->status);
    state_->dimensions = DesktopTheme::label(content, 12, true); state_->dimensions->setAlignment(::Qt::AlignCenter);
    state_->dimensions->setObjectName(QStringLiteral("scrollDimensions")); body->addWidget(state_->dimensions);
    state_->preview = new ScrollPreview(content); state_->preview->setFixedHeight(180); body->addWidget(state_->preview);
    state_->setup = new controls::ToolbarStrip(this); state_->layout->addWidget(state_->setup);
    state_->grip = new controls::ToolbarDragHandle(state_->setup); state_->grip->installEventFilter(this); state_->setup->add(state_->grip, 16);
    const auto family = controls::iconFontFamily();
    state_->start = controls::makeButton(state_->setup, QStringLiteral("long.start"), u'\ue660', family); state_->setup->add(state_->start);
    state_->stop = new QToolButton(state_->setup); state_->stop->setObjectName(QStringLiteral("long.stop"));
    state_->stop->setIcon(style()->standardIcon(QStyle::SP_MediaStop)); state_->stop->setIconSize({controls::iconSize, controls::iconSize});
    state_->stop->setFocusPolicy(::Qt::NoFocus); state_->setup->add(state_->stop);
    state_->actions = new controls::ToolbarStrip(this); state_->actions->setObjectName(QStringLiteral("scrollOutputActions"));
    state_->layout->addWidget(state_->actions);
    const auto make = [&](const char* key, char16_t glyph) {
        auto* button = controls::makeButton(state_->actions, QString::fromLatin1(key), glyph, family);
        state_->actions->add(button); return button;
    };
    state_->pin = make("tool.pin", u'\ue6a2'); state_->close = make("tool.close", u'\ue62d');
    state_->save = make("tool.save", u'\ue608'); state_->copy = make("tool.clipboard", u'\ue6ad');
    state_->notice = DesktopTheme::label(content, 12, true); state_->notice->setWordWrap(true); state_->notice->setMargin(8);
    state_->notice->setObjectName(QStringLiteral("scrollCaptureNotice")); body->addWidget(state_->notice);
    state_->frameDeadline.setSingleShot(true); state_->frameDeadline.setInterval(1900);
    connect(&state_->frameDeadline, &QTimer::timeout, this, [this] {
        if (state_->pendingLiveStart) {
            state_->pendingLiveStart = false; state_->hiddenForCapture = false;
            state_->outputFailure = state_->text(u"long.hideFailed", "The capture controls did not unmap before the deadline. No scrolling capture was started.");
            show(); refresh(); return;
        }
        if (state_->session && state_->sample) state_->session->acknowledgeNativeCapture(state_->captureToken, state_->sample, false);
        state_->sample = 0;
    });
    connect(state_->start, &QToolButton::clicked, this, &ScrollWindow::start);
    connect(state_->stop, &QToolButton::clicked, &session, &Capture::ScrollCaptureSession::stop);
    connect(state_->pin, &QToolButton::clicked, this, [this] { output(OutputIntent::Pin); });
    connect(state_->save, &QToolButton::clicked, this, [this] { output(OutputIntent::SaveAs); });
    connect(state_->copy, &QToolButton::clicked, this, [this] { output(OutputIntent::Clipboard); });
    connect(state_->close, &QToolButton::clicked, this, &QWidget::close);
    connect(&session, &Capture::ScrollCaptureSession::changed, this, &ScrollWindow::refresh);
    connect(&session, &Capture::ScrollCaptureSession::previewChanged, this, [this] { refresh(); });
    connect(&session, &Capture::ScrollCaptureSession::nativeCaptureRequested, this, &ScrollWindow::prepareNativeFrame);
    connect(&session, &Capture::ScrollCaptureSession::nativeCaptureCompleted, this, [this](quint64 token, quint64 sample) {
        if (token != state_->captureToken || sample != state_->capturingSample) return;
        state_->capturingSample = 0;
        if (!state_->hiddenForCapture || state_->closing) return;
        state_->hiddenForCapture = false; setAttribute(::Qt::WA_ShowWithoutActivating); show();
        QTimer::singleShot(0, this, [this] { Q_EMIT captureStateChanged(); });
    });
    connect(&session, &Capture::ScrollCaptureSession::finished, this, [this] {
        state_->frameDeadline.stop(); state_->sample = 0; state_->capturingSample = 0;
        if (state_->hiddenForCapture && !state_->closing) { state_->hiddenForCapture = false; setAttribute(::Qt::WA_ShowWithoutActivating); show(); }
        refresh();
        if (state_->afterStop && !state_->closing) { const auto intent = *state_->afterStop; state_->afterStop.reset(); output(intent); }
    });
    connect(&session, &Capture::ScrollCaptureSession::shutdownFinished, this, [this] {
        if (!state_->closing || state_->reportedClosed) return;
        state_->reportedClosed = true; Q_EMIT closed();
    });
    connect(&session, &QObject::destroyed, this, [this] { close(); });
    connect(&state_->activation, &Desktop::WindowActivation::finished, this,
        [this](quint64, const Desktop::WindowActivationResult& result) { Q_EMIT activationFinished(result); });
    resize(240, 360); refresh();
}
ScrollWindow::~ScrollWindow()
{
    state_->closing = true;
    if (state_->session) QObject::disconnect(state_->session, nullptr, this, nullptr);
    state_->frameDeadline.stop(); state_->surfaces.clear();
    if (state_->session) state_->session->shutdown();
}
void ScrollWindow::setTranslator(DesktopTranslator translator) { state_->translator = std::move(translator); refresh(); }
void ScrollWindow::refreshLanguage() { refresh(); }
Capture::ScrollCaptureSession* ScrollWindow::session() const noexcept { return state_->session; }
Capture::Failure ScrollWindow::open()
{
    if (!state_->session || state_->session->phase() != ScrollPhase::Ready || state_->opened || state_->closing)
        return {Capture::Failure::Code::Busy, 0, QStringLiteral("The scrolling source is not ready for a new view")};
    state_->opened = true;
    const auto selected = state_->session->selection();
    const auto initialArea = screen()->availableGeometry().size() - QSize(16, 16);
    setFixedWidth(std::max(controls::buttonSize + 2, std::min(240, initialArea.width())));
    resize(width(), std::max(120, std::min(360, initialArea.height())));
    std::optional<QPoint> position;
    for (const auto& binding : selected.screens) {
        const auto intersected = selected.sourceRect.intersected(binding.sourcePixels);
        if (intersected.isEmpty() || !binding.screen) continue;
        const auto first = Capture::screenLocalFromSource(binding, intersected.topLeft()) + binding.logicalGeometry.topLeft();
        const auto last = Capture::screenLocalFromSource(binding,
            QPointF(intersected.x() + intersected.width(), intersected.y() + intersected.height())) + binding.logicalGeometry.topLeft();
        const QRectF rectangle(first, last); state_->sourceRectangles.push_back(rectangle);
        const auto area = QRectF(binding.screen->availableGeometry());
        if (!position) {
            setScreen(binding.screen); setFixedWidth(std::max(controls::buttonSize + 2, std::min(240, qFloor(area.width()) - 16)));
            resize(width(), std::max(120, std::min(360, qFloor(area.height()) - 16)));
            const std::array spaces{
                QRectF(rectangle.right() + 8, area.top(), area.right() - rectangle.right() - 8, area.height()),
                QRectF(area.left(), area.top(), rectangle.left() - area.left() - 8, area.height()),
                QRectF(area.left(), rectangle.bottom() + 8, area.width(), area.bottom() - rectangle.bottom() - 8),
                QRectF(area.left(), area.top(), area.width(), rectangle.top() - area.top() - 8)};
            for (const auto& space : spaces) if (space.width() >= width() && space.height() >= height()) { position = space.topLeft().toPoint(); break; }
            if (!position) position = area.bottomRight().toPoint() - QPoint(width(), height());
        }
        auto overlay = std::make_unique<ScrollStartSurface>(binding, selected.sourceRect);
        overlay->label = state_->text(u"long.start", "Start");
        overlay->start = [this] { start(); }; overlay->cancel = [this] { close(); };
        overlay->show();
        if (overlay->windowHandle()) overlay->windowHandle()->installEventFilter(this);
        state_->surfaces.push_back(std::move(overlay));
    }
    if (position) move(*position);
    refresh(); show();
    if (windowHandle()) {
        windowHandle()->installEventFilter(this);
        state_->excluded = Desktop::excludeFromCapture(windowHandle()).applied;
    }
    refresh(); return {};
}
void ScrollWindow::activate(const QString& token)
{
    if (state_->closing) return;
    if (!state_->hiddenForCapture) { show(); raise(); if (windowHandle()) state_->activation.request(windowHandle(), token); }
}
void ScrollWindow::setExternalStopAvailable(bool available)
{
    if (state_->externalStopAvailable == available) return;
    state_->externalStopAvailable = available;
    if (!available && state_->hiddenForCapture && state_->manual()) stopCapture();
    refresh();
}
bool ScrollWindow::canStop() const noexcept
{
    if (state_->closing || !state_->session) return false;
    const auto phase = state_->session->phase();
    return state_->pendingLiveStart || phase == ScrollPhase::Starting || phase == ScrollPhase::Capturing;
}
bool ScrollWindow::stopCapture()
{
    if (!canStop()) return false;
    if (state_->pendingLiveStart) {
        state_->pendingLiveStart = false; state_->frameDeadline.stop();
        state_->hiddenForCapture = false; show(); refresh();
    } else state_->session->stop();
    return true;
}
bool ScrollWindow::hiddenForCapture() const noexcept { return state_->hiddenForCapture; }
void ScrollWindow::refresh()
{
    const auto phase = state_->session ? state_->session->phase() : ScrollPhase::Closed;
    const auto progress = state_->session ? state_->session->progress() : Capture::ScrollProgress{};
    const auto completion = state_->session ? state_->session->completion() : Capture::ScrollCompletion{};
    setWindowTitle(QStringLiteral("huhucapture · ") + state_->text(u"long.title", "Scrolling capture"));
    const auto setLabel = [&](QToolButton* button, QStringView key, const char* fallback) {
        const auto label = state_->text(key, fallback); button->setToolTip(label); button->setAccessibleName(label);
        if (button->icon().isNull()) button->setText(label);
    };
    setLabel(state_->start, u"long.start", "Start"); setLabel(state_->stop, u"long.stop", "Stop");
    setLabel(state_->pin, u"tool.pin", "Pin"); setLabel(state_->close, u"tool.close", "Close");
    setLabel(state_->save, u"tool.save", "Save as"); setLabel(state_->copy, u"tool.clipboard", "Copy");
    for (const auto& surface : state_->surfaces) { surface->label = state_->text(u"long.start", "Start"); surface->update(); }
    state_->start->setVisible(state_->manual());
    state_->start->setEnabled(phase == ScrollPhase::Ready && !state_->pending && !state_->closing &&
        !state_->pendingLiveStart && (!state_->manual() || state_->externalStopAvailable));
    state_->stop->setEnabled((phase == ScrollPhase::Starting || phase == ScrollPhase::Capturing) && !state_->closing);
    const bool canOutput = state_->session && !state_->pending && !state_->closing &&
        (state_->session->hasCompleteImage() || (!progress.imageSize.isEmpty() && working(phase)));
    for (auto* button : {state_->pin, state_->save, state_->copy}) button->setEnabled(canOutput);
    state_->close->setEnabled(!state_->closing);
    if (!progress.preview.isNull()) { state_->preview->image = progress.preview; state_->preview->update(); }
    const auto size = !progress.imageSize.isEmpty() ? progress.imageSize : state_->session ? state_->session->selection().preview.size() : QSize();
    state_->dimensions->setText(size.isEmpty() ? QString() : QStringLiteral("%1 × %2 px").arg(size.width()).arg(size.height()));
    QString status;
    if (!state_->outputFailure.isEmpty()) status = state_->outputFailure;
    else if (completion.failure) status = completion.failure.detail;
    else if (phase == ScrollPhase::Ready) status = state_->manual() ?
        state_->text(u"long.manualReady", "Press Start, then scroll the authorized source manually.") :
        state_->text(u"long.ready", "Click inside the selected region to start.");
    else if (phase == ScrollPhase::Finished) status = completion.reason == Capture::ScrollStopReason::ReachedBottom ?
        state_->text(u"long.captureBottom", "The bottom was reached.") : completion.reason == Capture::ScrollStopReason::HeightLimit ||
        completion.reason == Capture::ScrollStopReason::MemoryLimit ? state_->text(u"long.sizeLimit", "The image limit was reached. The last complete image is retained.") :
        state_->text(u"long.finished", "Stopped. The last complete image is retained.");
    else if (progress.notice == ScrollNotice::PointerPaused) status = state_->text(u"long.pointerPaused", "Move the pointer back over the scrolling target to continue.");
    else if (progress.notice == ScrollNotice::SourcePaused) status = state_->text(u"long.sourcePaused", "Screen sharing is paused; waiting for the source to resume.");
    else if (state_->manual()) status = state_->text(u"long.manualCapturing", "Scroll the authorized source manually; stop when finished.");
    state_->status->setText(status); state_->status->setVisible(!status.isEmpty());
    state_->notice->setText(state_->externalStopAvailable ?
        state_->text(u"long.controlsHidden", "Controls hide during capture. Use the tray or registered global shortcut to stop.") :
        state_->text(u"long.stopUnavailable", "A working tray or global stop action is required before these controls can be hidden for capture."));
    state_->notice->setVisible(state_->manual());
    state_->setup->refreshTextWidths(); state_->actions->refreshTextWidths();
    if (phase != ScrollPhase::Ready) for (const auto& surface : state_->surfaces)
        if (!working(phase) || !surface->outline) surface->hide();
    state_->layout->activate();
    QTimer::singleShot(0, this, [this] { Q_EMIT captureStateChanged(); });
}
void ScrollWindow::start()
{
    if (!state_->session || state_->session->phase() != ScrollPhase::Ready || state_->closing || state_->pendingLiveStart) return;
    for (const auto& surface : state_->surfaces) surface->hide();
    if (state_->manual()) {
        if (!state_->externalStopAvailable) { refresh(); return; }
        for (auto* tooltip : QApplication::topLevelWidgets())
            if (tooltip->windowType() == ::Qt::ToolTip) {
                tooltip->hide();
                if (tooltip->windowHandle()) tooltip->windowHandle()->installEventFilter(this);
            }
        state_->outputFailure.clear(); state_->pendingLiveStart = true; state_->hiddenForCapture = true;
        hide(); state_->frameDeadline.start(); refresh();
        QTimer::singleShot(0, this, &ScrollWindow::acknowledgeHidden);
        return;
    }
    const QPointer<ScrollWindow> alive(this);
    const auto accepted = state_->session->start(); if (!alive) return;
    if (accepted) {
        // The gate first observes actual unexposure of every old start surface;
        // only then may it replace the interior mask with an outside outline.
        QTimer::singleShot(0, this, &ScrollWindow::acknowledgeHidden);
    }
    else if (state_->session && state_->session->phase() == ScrollPhase::Ready)
        for (const auto& surface : state_->surfaces) surface->show();
}
void ScrollWindow::output(OutputIntent intent)
{
    if (!state_->session || state_->closing || state_->pending) return;
    if (working(state_->session->phase())) {
        if (state_->session->progress().imageSize.isEmpty()) return;
        state_->afterStop = intent; state_->session->stop(); return;
    }
    if (!state_->session->hasCompleteImage()) return;
    const auto image = state_->session->image();
    if (image.isNull()) { state_->outputFailure = state_->text(u"long.outputBusy", "The output could not start. The complete image is retained."); refresh(); return; }
    state_->pending = true; state_->outputToken = ++state_->nextOutput;
    if (!state_->outputToken) state_->outputToken = ++state_->nextOutput;
    state_->outputFailure.clear(); refresh();
    Q_EMIT outputRequested(state_->outputToken, intent, image);
}
void ScrollWindow::completeOutput(quint64 token, OutputResult result, const QString& detail)
{
    if (!state_->pending || token != state_->outputToken) return;
    state_->pending = false;
    if (result == OutputResult::Succeeded) { close(); return; }
    state_->outputFailure = detail; refresh();
}
void ScrollWindow::prepareNativeFrame(quint64 token, quint64 sample)
{
    if (!state_->session || state_->closing) { if (state_->session) state_->session->acknowledgeNativeCapture(token, sample, false); return; }
    state_->captureToken = token; state_->sample = sample; state_->capturingSample = sample;
    if (!state_->excluded && std::any_of(state_->sourceRectangles.begin(), state_->sourceRectangles.end(),
            [this](const QRectF& area) { return area.intersects(frameGeometry()); })) {
        state_->hiddenForCapture = true; hide();
    }
    state_->frameDeadline.start();
    QTimer::singleShot(0, this, &ScrollWindow::acknowledgeHidden);
}
void ScrollWindow::acknowledgeHidden()
{
    if (state_->pendingLiveStart) {
        if (!state_->session || state_->closing || !state_->externalStopAvailable) return;
        if (isVisible() || (windowHandle() && windowHandle()->isExposed())) return;
        for (auto* tooltip : QApplication::topLevelWidgets())
            if (tooltip->windowType() == ::Qt::ToolTip && (tooltip->isVisible() ||
                (tooltip->windowHandle() && tooltip->windowHandle()->isExposed()))) return;
        state_->pendingLiveStart = false; state_->frameDeadline.stop();
        const QPointer<ScrollWindow> alive(this);
        const auto accepted = state_->session->start();
        if (!alive) return;
        if (!accepted) {
            state_->hiddenForCapture = false; show();
            state_->outputFailure = state_->text(u"long.refreshRejected", "The authorized source could not begin a capture refresh.");
        }
        refresh(); return;
    }
    if (!state_->sample || !state_->session || state_->closing) return;
    if (state_->hiddenForCapture && windowHandle() && windowHandle()->isExposed()) return;
    for (const auto& surface : state_->surfaces)
        if (!surface->outline && surface->windowHandle() && surface->windowHandle()->isExposed()) return;
    for (const auto& surface : state_->surfaces) if (!surface->outline) surface->showOutline();
    const auto sample = std::exchange(state_->sample, 0); state_->frameDeadline.stop();
    state_->session->acknowledgeNativeCapture(state_->captureToken, sample, true);
}
void ScrollWindow::closeEvent(QCloseEvent* event)
{
    event->ignore();
    if (state_->closing) return;
    state_->closing = true; state_->pendingLiveStart = false; state_->afterStop.reset(); state_->frameDeadline.stop();
    for (const auto& surface : state_->surfaces) surface->hide();
    hide();
    const QPointer<ScrollWindow> alive(this);
    if (state_->pending) Q_EMIT outputCancelRequested(state_->outputToken);
    if (!alive) return;
    if (state_->session) state_->session->shutdown();
    else if (!state_->reportedClosed) { state_->reportedClosed = true; QTimer::singleShot(0, this, [this] { Q_EMIT closed(); }); }
}
void ScrollWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == ::Qt::Key_Escape) { close(); event->accept(); return; }
    if (event->matches(QKeySequence::Save)) { output(OutputIntent::SaveAs); event->accept(); return; }
    if (event->matches(QKeySequence::Copy) || event->key() == ::Qt::Key_Return || event->key() == ::Qt::Key_Enter) {
        output(OutputIntent::Clipboard); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}
void ScrollWindow::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); }
void ScrollWindow::showEvent(QShowEvent* event)
{
    if (state_->hiddenForCapture) {
        // Reuse the recording toolbar's hidden-window rule: activation or a
        // stray show cannot map a surface over an ongoing source capture.
        hide();
        if (state_->manual() && !state_->pendingLiveStart && state_->session) state_->session->stop();
        return;
    }
    QWidget::showEvent(event);
}
bool ScrollWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Expose) {
        if (watched == windowHandle() && state_->manual() && state_->hiddenForCapture && isVisible()) {
            hide(); if (state_->session) state_->session->stop();
        }
        QTimer::singleShot(0, this, &ScrollWindow::acknowledgeHidden);
    }
    if (watched == state_->grip && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == ::Qt::LeftButton && windowHandle()) { windowHandle()->startSystemMove(); return true; }
    }
    return QWidget::eventFilter(watched, event);
}
} // namespace HuhuCapture::UI::Qt
