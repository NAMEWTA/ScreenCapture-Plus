#include "ui/qt/RecordingWindow.h"

#include "ui/qt/DesktopTheme.h"
#include "ui/qt/ToolbarControls.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPointer>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <chrono>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace controls = ToolbarControls;
using Recording::SessionState;
bool canStop(SessionState state)
{ return state == SessionState::Preparing || state == SessionState::Recording; }
bool working(SessionState state)
{
    return canStop(state) || state == SessionState::Stopping || state == SessionState::Finalizing ||
        state == SessionState::Archiving;
}
}

struct RecordingWindow::State {
    DesktopTranslator translator;
    RecordingCapabilities capabilities;
    RecordingOptions options;
    SessionState phase{SessionState::Idle};
    Core::MediaTime elapsed{};
    controls::ToolbarStrip *setup{}, *recording{}, *completion{};
    QToolButton *mp4{}, *gif{}, *system{}, *microphone{}, *start{}, *cancel{},
        *discard{}, *save{}, *copy{}, *retry{}, *close{};
    QLabel *timer{}, *status{}, *captureNotice{};
    QWidget *statusPanel{}, *grip{};
    QVBoxLayout* layout{};
    QString failure, retainedPath;
    QPointer<QScreen> placementScreen;
    QRectF selection, workArea;
    QVector<QRect> safeAreas;
    QPoint dragStart, dragOrigin;
    bool outputPending{}, stopAdmitted{}, retryAvailable{}, initialPlacement{}, layoutPending{}, captureExcluded{true};
    bool captureHidden{}, safePlacement{}, dragging{}, correctingPosition{};

    QString text(QStringView key) const
    { return translator ? translator(key) : key.toString(); }
};

RecordingWindow::RecordingWindow(QWidget* parent)
    : QWidget(parent, ::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint),
      state_(std::make_unique<State>())
{
    setObjectName(QStringLiteral("recordingWindow"));
    setFocusPolicy(::Qt::StrongFocus);
    DesktopTheme::apply(this);
    state_->layout = new QVBoxLayout(this);
    state_->layout->setContentsMargins(0, 0, 0, 0); state_->layout->setSpacing(0);
    const auto family = controls::iconFontFamily();
    const auto make = [&](controls::ToolbarStrip* strip, QStringView key, char16_t glyph) {
        auto* button = controls::makeButton(strip, key.toString(), glyph, family);
        strip->add(button); return button;
    };
    const auto divider = [](controls::ToolbarStrip* strip) {
        strip->add(new controls::ToolbarDivider(strip), static_cast<int>(UI::toolbarDivider));
    };
    state_->setup = new controls::ToolbarStrip(this); state_->setup->setObjectName(QStringLiteral("recordingSetup"));
    state_->layout->addWidget(state_->setup);
    state_->grip = new controls::ToolbarDragHandle(state_->setup);
    state_->grip->setObjectName(QStringLiteral("recordingDragHandle"));
    state_->grip->installEventFilter(this); state_->setup->add(state_->grip, 16);
    state_->mp4 = new QToolButton(state_->setup); state_->mp4->setText(QStringLiteral("MP4"));
    state_->mp4->setObjectName(QStringLiteral("recordingMp4")); state_->mp4->setCheckable(true);
    state_->mp4->setFocusPolicy(::Qt::NoFocus); state_->mp4->setFont(DesktopTheme::font(16));
    state_->setup->add(state_->mp4, 60);
    state_->gif = new QToolButton(state_->setup); state_->gif->setText(QStringLiteral("GIF"));
    state_->gif->setObjectName(QStringLiteral("recordingGif")); state_->gif->setCheckable(true);
    state_->gif->setFocusPolicy(::Qt::NoFocus); state_->gif->setFont(DesktopTheme::font(16));
    state_->setup->add(state_->gif, 60);
    divider(state_->setup);
    state_->system = make(state_->setup, u"video.recordSystem", u'\ue654'); state_->system->setCheckable(true);
    state_->microphone = make(state_->setup, u"video.recordMic", u'\ue73b'); state_->microphone->setCheckable(true);
    divider(state_->setup);
    state_->start = make(state_->setup, u"video.startRecord", u'\ue660');
    state_->cancel = make(state_->setup, u"video.exit", u'\ue62d');
    state_->recording = new controls::ToolbarStrip(this); state_->recording->setObjectName(QStringLiteral("recordingControls"));
    state_->layout->addWidget(state_->recording);
    state_->timer = DesktopTheme::label(state_->recording, 14); state_->timer->setObjectName(QStringLiteral("recordingTimer"));
    state_->timer->setAlignment(::Qt::AlignCenter); state_->timer->installEventFilter(this);
    state_->recording->add(state_->timer, 136); divider(state_->recording);
    state_->discard = make(state_->recording, u"video.stopExit", u'\ue62d');
    state_->save = make(state_->recording, u"video.stopFile", u'\ue608');
    state_->copy = make(state_->recording, u"video.stopClipboard", u'\ue6ad');
    state_->completion = new controls::ToolbarStrip(this); state_->layout->addWidget(state_->completion);
    state_->retry = make(state_->completion, u"video.retryOutput", u'\ued8a');
    state_->close = make(state_->completion, u"video.exit", u'\ue62d');
    state_->statusPanel = new QWidget(this);
    auto* statusLayout = new QVBoxLayout(state_->statusPanel); statusLayout->setContentsMargins(12, 8, 12, 8);
    state_->status = DesktopTheme::label(state_->statusPanel, 12); state_->status->setObjectName(QStringLiteral("recordingStatus"));
    state_->status->setTextFormat(::Qt::PlainText); state_->status->setWordWrap(true);
    state_->status->setTextInteractionFlags(::Qt::TextSelectableByMouse);
    state_->captureNotice = DesktopTheme::label(state_->statusPanel, 12);
    state_->captureNotice->setObjectName(QStringLiteral("recordingCaptureNotice"));
    state_->captureNotice->setTextFormat(::Qt::PlainText); state_->captureNotice->setWordWrap(true);
    statusLayout->addWidget(state_->status); statusLayout->addWidget(state_->captureNotice);
    state_->layout->addWidget(state_->statusPanel);
    connect(state_->mp4, &QToolButton::clicked, this, [this] {
        if (state_->phase != SessionState::Idle || !state_->capabilities.mp4.canRequest()) return;
        state_->options.format = Recording::Format::Mp4; refresh();
    });
    connect(state_->gif, &QToolButton::clicked, this, [this] {
        if (state_->phase != SessionState::Idle || !state_->capabilities.gif.canRequest()) return;
        state_->options.format = Recording::Format::Gif;
        state_->options.systemAudio = false; state_->options.microphone = false; refresh();
    });
    connect(state_->system, &QToolButton::clicked, this, [this] {
        if (state_->phase != SessionState::Idle || state_->options.format != Recording::Format::Mp4 ||
            !state_->capabilities.systemAudio.canRequest()) return;
        state_->options.systemAudio = !state_->options.systemAudio; refresh();
    });
    connect(state_->microphone, &QToolButton::clicked, this, [this] {
        if (state_->phase != SessionState::Idle || state_->options.format != Recording::Format::Mp4 ||
            !state_->capabilities.microphone.canRequest()) return;
        state_->options.microphone = !state_->options.microphone; refresh();
    });
    connect(state_->start, &QToolButton::clicked, this, [this] {
        if (state_->phase != SessionState::Idle || !state_->start->isEnabled()) return;
        const auto selected = state_->options;
        setState(SessionState::Preparing);
        Q_EMIT startRequested(selected);
    });
    connect(state_->cancel, &QToolButton::clicked, this, &QWidget::close);
    connect(state_->discard, &QToolButton::clicked, this, [this] { stop(Recording::StopIntent::Discard); });
    connect(state_->save, &QToolButton::clicked, this, [this] { stop(Recording::StopIntent::Save); });
    connect(state_->copy, &QToolButton::clicked, this, [this] { stop(Recording::StopIntent::Copy); });
    connect(state_->retry, &QToolButton::clicked, this, [this] {
        if (!state_->retryAvailable || state_->outputPending) return;
        setOutputPending(true); Q_EMIT retryOutputRequested();
    });
    connect(state_->close, &QToolButton::clicked, this, &QWidget::close);
    resize(300, 46); refresh();
}
RecordingWindow::~RecordingWindow() = default;
void RecordingWindow::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refresh(); }
void RecordingWindow::setCapabilities(RecordingCapabilities capabilities)
{
    state_->capabilities = std::move(capabilities);
    if (!state_->capabilities.systemAudio.canRequest()) state_->options.systemAudio = false;
    if (!state_->capabilities.microphone.canRequest()) state_->options.microphone = false;
    refresh();
}
void RecordingWindow::setState(SessionState state)
{
    state_->phase = state;
    if (state == SessionState::Idle) { state_->stopAdmitted = false; state_->elapsed = {}; }
    if (state == SessionState::Preparing) { state_->failure.clear(); state_->retainedPath.clear(); state_->elapsed = {}; }
    refresh();
}
void RecordingWindow::setElapsed(Core::MediaTime elapsed)
{
    state_->elapsed = (std::max)(elapsed, Core::MediaTime{});
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(state_->elapsed).count();
    const auto minutes = state_->options.format == Recording::Format::Gif ? 6 : 120;
    state_->timer->setText(QStringLiteral("%1:%2 / %3:00").arg(seconds / 60, 2, 10, QChar(u'0'))
        .arg(seconds % 60, 2, 10, QChar(u'0')).arg(minutes, 2, 10, QChar(u'0')));
}
void RecordingWindow::setOutputPending(bool pending)
{ state_->outputPending = pending; refresh(); }
void RecordingWindow::setCaptureExcluded(bool excluded)
{ if (state_->captureExcluded != excluded) { state_->captureExcluded = excluded; refresh(); } }
void RecordingWindow::showFailure(const QString& detail, const QString& retainedPath, bool canRetryOutput)
{
    state_->failure = detail; state_->retainedPath = retainedPath;
    state_->retryAvailable = canRetryOutput; state_->outputPending = false;
    if (working(state_->phase)) state_->phase = SessionState::Failed;
    refresh(); show();
}
void RecordingWindow::setPlacement(QScreen* screen, QRectF selection, QRectF workArea)
{
    if (isVisible() || !screen || selection.isEmpty() || workArea.isEmpty()) return;
    state_->placementScreen = screen; state_->selection = selection; state_->workArea = workArea;
    state_->initialPlacement = true; setScreen(screen);
}
RecordingOptions RecordingWindow::options() const { return state_->options; }
bool RecordingWindow::setCaptureSafeAreas(QVector<QRect> areas)
{
    state_->safeAreas = std::move(areas); state_->safePlacement = !state_->safeAreas.isEmpty();
    state_->dragging = false;
    refresh();
    return !state_->safePlacement || safeGeometry(size(), pos()).has_value();
}
void RecordingWindow::setHiddenForCapture(bool hidden)
{
    if (state_->captureHidden == hidden) return;
    state_->captureHidden = hidden; state_->dragging = false;
    if (hidden) hide();
    else show();
}
bool RecordingWindow::hiddenForCapture() const noexcept { return state_->captureHidden; }

std::optional<QRect> RecordingWindow::safeGeometry(QSize size, QPoint preferred) const
{
    std::optional<QRect> closest;
    double distance{};
    for (const auto& area : state_->safeAreas) {
        if (size.width() > area.width() || size.height() > area.height()) continue;
        const QPoint position((std::clamp)(preferred.x(), area.left(), area.x() + area.width() - size.width()),
            (std::clamp)(preferred.y(), area.top(), area.y() + area.height() - size.height()));
        const auto dx = static_cast<double>(position.x()) - preferred.x();
        const auto dy = static_cast<double>(position.y()) - preferred.y();
        const auto candidate = dx * dx + dy * dy;
        if (!closest || candidate < distance) { closest = QRect(position, size); distance = candidate; }
    }
    return closest;
}

void RecordingWindow::refresh()
{
    setWindowTitle(state_->text(u"video.recordingTitle"));
    const bool setup = state_->phase == SessionState::Idle;
    const bool mp4 = state_->options.format == Recording::Format::Mp4;
    state_->setup->setVisible(setup);
    state_->recording->setVisible(!setup && (working(state_->phase) || state_->outputPending));
    state_->completion->setVisible(!setup && !working(state_->phase) && !state_->outputPending);
    state_->mp4->setChecked(mp4); state_->gif->setChecked(!mp4);
    state_->mp4->setEnabled(setup && state_->capabilities.mp4.canRequest());
    state_->gif->setEnabled(setup && state_->capabilities.gif.canRequest());
    state_->system->setChecked(state_->options.systemAudio); state_->microphone->setChecked(state_->options.microphone);
    state_->system->setEnabled(setup && mp4 && state_->capabilities.systemAudio.canRequest());
    state_->microphone->setEnabled(setup && mp4 && state_->capabilities.microphone.canRequest());
    state_->start->setEnabled(setup && (mp4 ? state_->capabilities.mp4 : state_->capabilities.gif).canRequest());
    const bool stoppingAllowed = canStop(state_->phase) && !state_->stopAdmitted && !state_->outputPending;
    for (auto* button : {state_->discard, state_->save, state_->copy}) button->setEnabled(stoppingAllowed);
    state_->retry->setEnabled(state_->retryAvailable && !state_->outputPending);
    state_->retry->setVisible(state_->retryAvailable);
    for (auto* strip : {state_->setup, state_->recording, state_->completion}) {
        for (auto* button : strip->findChildren<QToolButton*>()) {
            const auto key = button->property("languageKey").toString();
            if (key.isEmpty()) continue;
            const auto text = state_->text(key);
            // A tooltip is a separate native surface. During recording the
            // button keeps its accessible name without creating an unguarded
            // popup that could overlap pixels even when its toolbar is outside.
            button->setToolTip(working(state_->phase) ? QString() : text); button->setAccessibleName(text);
            if (button->icon().isNull()) button->setText(text);
        }
        strip->refreshTextWidths();
    }
    state_->mp4->setToolTip(state_->capabilities.mp4.canRequest() ? state_->text(u"video.outputMp4") : state_->capabilities.mp4.reason);
    state_->gif->setToolTip(state_->capabilities.gif.canRequest() ? state_->text(u"video.outputGif") : state_->capabilities.gif.reason);
    if (mp4 && !state_->capabilities.systemAudio.canRequest()) state_->system->setToolTip(state_->capabilities.systemAudio.reason);
    if (mp4 && !state_->capabilities.microphone.canRequest()) state_->microphone->setToolTip(state_->capabilities.microphone.reason);
    QString status = state_->failure;
    if (!state_->retainedPath.isEmpty()) status += (status.isEmpty() ? QString() : QStringLiteral("\n\n")) +
        state_->text(u"video.recovery").arg(state_->retainedPath);
    if (status.isEmpty()) {
        if (state_->phase == SessionState::Preparing) status = state_->text(u"video.preparing");
        else if (state_->phase == SessionState::Stopping || state_->phase == SessionState::Finalizing)
            status = state_->text(u"video.finalizing");
        else if (state_->phase == SessionState::Archiving) status = state_->text(u"video.archiving");
    }
    state_->status->setText(status); state_->status->setProperty("error", !state_->failure.isEmpty());
    DesktopTheme::repolish(state_->status); state_->status->setVisible(!status.isEmpty());
    const bool showCaptureNotice = !state_->captureExcluded && !state_->safePlacement && !state_->captureHidden &&
        (setup || working(state_->phase));
    state_->captureNotice->setText(state_->text(u"video.controlsVisible"));
    state_->captureNotice->setVisible(showCaptureNotice);
    state_->statusPanel->setVisible(!status.isEmpty() || showCaptureNotice);
    const auto moveText = state_->text(u"video.moveControls");
    state_->grip->setToolTip(working(state_->phase) ? QString() : moveText); state_->grip->setAccessibleName(moveText);
    state_->timer->setToolTip(working(state_->phase) ? QString() : moveText); state_->timer->setAccessibleName(moveText);
    setElapsed(state_->elapsed);
    reflow();
}
void RecordingWindow::reflow()
{
    auto* current = screen();
    const auto available = current ? current->availableGeometry().width() : 640;
    const auto* strip = state_->phase == SessionState::Idle ? state_->setup :
        (working(state_->phase) || state_->outputPending) ? state_->recording : state_->completion;
    const int natural = state_->statusPanel->isHidden() ? strip->sizeHint().width() :
        (std::max)(strip->sizeHint().width(), 360);
    const auto desired = (std::max)(controls::buttonSize + 2, (std::min)(natural, available));
    state_->layout->activate();
    const auto height = state_->layout->totalHeightForWidth(desired);
    const auto wanted = height >= 0 ? height : state_->layout->sizeHint().height();
    if (wanted <= 0) return;
    const auto bounds = state_->safePlacement ? safeGeometry(QSize(desired, wanted), pos()) :
        std::optional<QRect>{QRect(pos(), QSize(desired, wanted))};
    if (!bounds) {
        hide();
        QTimer::singleShot(0, this, [this] { if (state_->safePlacement && !state_->captureHidden) Q_EMIT capturePlacementLost(); });
        return;
    }
    // One native ConfigureWindow applies both position and size. Resizing into
    // the selection and moving outside afterwards would expose one bad frame.
    if (geometry() != *bounds) {
        state_->correctingPosition = true; setGeometry(*bounds); state_->correctingPosition = false;
    }
}
void RecordingWindow::stop(Recording::StopIntent intent)
{
    if (!canStop(state_->phase) || state_->stopAdmitted || state_->outputPending) return;
    state_->stopAdmitted = true; refresh(); Q_EMIT stopRequested(intent);
}
void RecordingWindow::closeEvent(QCloseEvent* event)
{
    if (working(state_->phase)) { event->ignore(); stop(Recording::StopIntent::Discard); return; }
    if (state_->outputPending) { event->ignore(); return; }
    event->accept(); Q_EMIT cancelRequested();
}
void RecordingWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == ::Qt::Key_Escape) { event->accept(); close(); return; }
    if (event->modifiers() & ::Qt::ControlModifier) {
        if (event->key() == ::Qt::Key_S) { event->accept(); stop(Recording::StopIntent::Save); return; }
        if (event->key() == ::Qt::Key_C) { event->accept(); stop(Recording::StopIntent::Copy); return; }
    }
    QWidget::keyPressEvent(event);
}
void RecordingWindow::showEvent(QShowEvent* event)
{
    if (state_->captureHidden) { hide(); return; }
    QWidget::showEvent(event); reflow();
    if (state_->initialPlacement && state_->placementScreen) {
        state_->initialPlacement = false;
        const auto area = state_->workArea;
        const auto selected = state_->selection;
        const auto left = (std::clamp)(selected.right() - width(), area.left(), (std::max)(area.left(), area.right() - width()));
        qreal top = selected.bottom() + 8;
        if (top + height() > area.bottom()) top = selected.top() - height() - 8;
        top = (std::clamp)(top, area.top(), (std::max)(area.top(), area.bottom() - height()));
        move(qRound(left), qRound(top));
    }
}
void RecordingWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    if (state_->safePlacement && !state_->correctingPosition) reflow();
}
void RecordingWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (state_->layoutPending) return;
    state_->layoutPending = true;
    QTimer::singleShot(0, this, [this] { state_->layoutPending = false; reflow(); });
}
bool RecordingWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == state_->timer || watched == state_->grip) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == ::Qt::LeftButton && state_->safePlacement) {
                state_->dragging = true; state_->dragStart = mouse->globalPosition().toPoint(); state_->dragOrigin = pos();
                return true;
            }
            if (mouse->button() == ::Qt::LeftButton && windowHandle() && windowHandle()->startSystemMove()) return true;
        } else if (event->type() == QEvent::MouseMove && state_->dragging) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (const auto bounds = safeGeometry(size(), state_->dragOrigin + mouse->globalPosition().toPoint() - state_->dragStart))
                setGeometry(*bounds);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease && state_->dragging) {
            state_->dragging = false; return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace HuhuCapture::UI::Qt
