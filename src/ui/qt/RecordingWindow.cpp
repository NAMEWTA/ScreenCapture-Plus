#include "ui/qt/RecordingWindow.h"
#include "ui/qt/DesktopTheme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QPointer>
#include <QShortcut>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace recording = HuhuCapture::Recording;
namespace theme = DesktopTheme;
bool active(recording::SessionState value)
{
    using S = recording::SessionState;
    return value == S::Preparing || value == S::Recording || value == S::Stopping ||
        value == S::Finalizing || value == S::Archiving;
}
}

struct RecordingWindow::State {
    DesktopTranslator translator;
    recording::PixelRegion region;
    recording::SessionState phase{recording::SessionState::Idle};
    recording::Format format{recording::Format::Mp4};
    Core::MediaTime elapsed{};
    bool pendingOutput{}, stopPending{}, starting{}, controlsMayAppear{};
    QString message, path, actualCodec;
    QLabel *title{}, *timer{}, *status{}, *source{}, *captureNotice{};
    QPushButton *mp4{}, *gif{}, *start{}, *discard{}, *save{}, *copy{}, *close{}, *file{};
    QCheckBox *system{}, *microphone{};
    QString text(QStringView key, const char* fallback) const
    {
        const auto value = translator ? translator(key) : QString{};
        return value.isEmpty() || value == key ? QString::fromUtf8(fallback) : value;
    }
};

RecordingWindow::RecordingWindow(QWidget* parent)
    : QWidget(parent, ::Qt::Tool | ::Qt::WindowStaysOnTopHint), state_(std::make_unique<State>())
{
    qRegisterMetaType<recording::BackendRequest>(); qRegisterMetaType<recording::StopIntent>();
    setObjectName(QStringLiteral("recordingWindow")); theme::apply(this);
    setMinimumSize(360, 140); resize(640, 160);
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(14, 12, 14, 12); outer->setSpacing(9);
    auto* heading = new QHBoxLayout;
    state_->title = theme::label(this, 16); state_->title->setFont(theme::font(16, QFont::DemiBold));
    state_->source = theme::label(this, 12, true); state_->source->setObjectName(QStringLiteral("recordingSource"));
    state_->timer = theme::label(this, 14); state_->timer->setObjectName(QStringLiteral("recordingTimer"));
    heading->addWidget(state_->title); heading->addWidget(state_->source); heading->addStretch(); heading->addWidget(state_->timer);
    outer->addLayout(heading);
    auto* configuration = new QHBoxLayout;
    state_->mp4 = theme::button(this, "recordingMp4"); state_->gif = theme::button(this, "recordingGif");
    state_->mp4->setText(QStringLiteral("MP4")); state_->gif->setText(QStringLiteral("GIF"));
    state_->mp4->setCheckable(true); state_->gif->setCheckable(true); state_->mp4->setChecked(true);
    auto* formats = new QButtonGroup(this); formats->addButton(state_->mp4); formats->addButton(state_->gif); formats->setExclusive(true);
    state_->system = new QCheckBox(this); state_->system->setObjectName(QStringLiteral("recordingSystemAudio")); state_->system->setChecked(true);
    state_->microphone = new QCheckBox(this); state_->microphone->setObjectName(QStringLiteral("recordingMicrophone"));
    configuration->addWidget(state_->mp4); configuration->addWidget(state_->gif);
    configuration->addWidget(state_->system); configuration->addWidget(state_->microphone); configuration->addStretch(); outer->addLayout(configuration);
    state_->status = theme::label(this, 12); state_->status->setObjectName(QStringLiteral("recordingStatus"));
    state_->status->setWordWrap(true); state_->status->setTextInteractionFlags(::Qt::TextSelectableByMouse);
    outer->addWidget(state_->status);
    state_->captureNotice = theme::label(this, 12, true);
    state_->captureNotice->setObjectName(QStringLiteral("recordingCaptureNotice"));
    state_->captureNotice->setWordWrap(true); outer->addWidget(state_->captureNotice);
    auto* actions = new QHBoxLayout;
    state_->start = theme::button(this, "recordingStart", true);
    state_->discard = theme::button(this, "recordingDiscard"); state_->discard->setProperty("destructive", true);
    state_->save = theme::button(this, "recordingSave", true); state_->copy = theme::button(this, "recordingCopy");
    state_->file = theme::button(this, "recordingOpenFile"); state_->close = theme::button(this, "recordingClose");
    actions->addWidget(state_->start); actions->addWidget(state_->discard); actions->addWidget(state_->save); actions->addWidget(state_->copy);
    actions->addWidget(state_->file); actions->addStretch(); actions->addWidget(state_->close); outer->addLayout(actions);
    connect(state_->mp4, &QPushButton::clicked, this, [this] { state_->format = recording::Format::Mp4; refresh(); });
    connect(state_->gif, &QPushButton::clicked, this, [this] {
        state_->format = recording::Format::Gif;
        // Preserve the old toolbar: selecting GIF turns both sources off;
        // returning to MP4 does not secretly re-enable an audio source.
        state_->system->setChecked(false); state_->microphone->setChecked(false); refresh();
    });
    connect(state_->start, &QPushButton::clicked, this, [this] { requestStart(); });
    connect(state_->discard, &QPushButton::clicked, this, [this] { requestStop(recording::StopIntent::Discard); });
    connect(state_->save, &QPushButton::clicked, this, [this] { requestStop(recording::StopIntent::Save); });
    connect(state_->copy, &QPushButton::clicked, this, [this] { requestStop(recording::StopIntent::Copy); });
    connect(state_->close, &QPushButton::clicked, this, &QWidget::close);
    connect(state_->file, &QPushButton::clicked, this, [this] { if (!state_->path.isEmpty()) Q_EMIT fileRequested(state_->path); });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, this);
    auto* discardShortcut = new QShortcut(QKeySequence(::Qt::Key_Escape), this);
    connect(saveShortcut, &QShortcut::activated, this, [this] { requestStop(recording::StopIntent::Save); });
    connect(copyShortcut, &QShortcut::activated, this, [this] { requestStop(recording::StopIntent::Copy); });
    connect(discardShortcut, &QShortcut::activated, this, [this] {
        if (!requestStop(recording::StopIntent::Discard) && !active(state_->phase)) close();
    });
    refresh();
}

RecordingWindow::~RecordingWindow()
{ for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr); }
void RecordingWindow::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refresh(); }
void RecordingWindow::setRegion(recording::PixelRegion region)
{
    if (active(state_->phase) || state_->pendingOutput) return;
    state_->region = region; state_->phase = recording::SessionState::Idle;
    state_->elapsed = {}; state_->path.clear(); state_->message.clear(); state_->actualCodec.clear(); refresh();
}
void RecordingWindow::setState(recording::SessionState value, Core::MediaTime elapsed, const QString& message, const QString& path)
{
    if (value != state_->phase) state_->stopPending = false;
    state_->starting = false; state_->phase = value; state_->elapsed = elapsed;
    state_->message = message; state_->path = path; refresh();
}
void RecordingWindow::setOutputPending(bool pending) { state_->pendingOutput = pending; refresh(); }
void RecordingWindow::setControlsMayAppear(bool value) { state_->controlsMayAppear = value; refresh(); }
void RecordingWindow::showPrepared(const recording::PreparedRecording& prepared)
{
    state_->region = prepared.region;
    state_->actualCodec = prepared.codec == recording::Codec::Hevc ? QStringLiteral("HEVC") :
        prepared.codec == recording::Codec::H264 ? QStringLiteral("H.264") : QStringLiteral("GIF");
    refresh();
}
recording::BackendRequest RecordingWindow::request() const
{
    auto value = recording::recordingDefaults(state_->format); value.region = state_->region;
    if (value.format == recording::Format::Mp4) {
        value.systemAudio = state_->system->isChecked(); value.microphone = state_->microphone->isChecked();
    }
    return value;
}
recording::SessionState RecordingWindow::state() const noexcept { return state_->phase; }
bool RecordingWindow::requestStart()
{
    if (active(state_->phase) || state_->pendingOutput || state_->starting ||
        state_->region.width <= 0 || state_->region.height <= 0) return false;
    state_->starting = true; refresh(); const auto value = request();
    Q_EMIT startRequested(value); return true;
}
bool RecordingWindow::requestStop(recording::StopIntent intent)
{
    if ((state_->phase != recording::SessionState::Recording && state_->phase != recording::SessionState::Preparing) ||
        state_->stopPending || state_->pendingOutput) return false;
    state_->stopPending = true; refresh(); Q_EMIT stopRequested(intent); return true;
}
void RecordingWindow::refresh()
{
    using S = recording::SessionState;
    const auto text = [this](QStringView key, const char* fallback) { return state_->text(key, fallback); };
    setWindowTitle(text(u"recording.title", "Screen recording")); state_->title->setText(windowTitle());
    state_->system->setText(text(u"video.recordSystem", "Record system audio"));
    state_->microphone->setText(text(u"video.recordMic", "Record microphone"));
    state_->start->setText(text(u"video.startRecord", "Start recording"));
    state_->save->setText(text(u"tool.save", "Save")); state_->copy->setText(text(u"tool.copy", "Copy"));
    state_->discard->setText(text(u"recording.discard", "Discard")); state_->close->setText(text(u"video.exit", "Close"));
    state_->file->setText(text(u"clipboard.locate", "Show file"));
    state_->save->setToolTip(text(u"video.stopFile", "Stop recording and save to file"));
    state_->copy->setToolTip(text(u"video.stopClipboard", "Stop recording and copy to clipboard"));
    state_->discard->setToolTip(text(u"video.stopExit", "Stop recording and discard"));
    state_->captureNotice->setText(text(u"recording.controlsVisible",
        "This desktop cannot exclude the recording controls. Keep them outside the selected area, or share only the target window in the screen-sharing dialog."));
    state_->captureNotice->setVisible(state_->controlsMayAppear);
    state_->source->setText(QStringLiteral("%1 × %2%3").arg(state_->region.width).arg(state_->region.height)
        .arg(state_->actualCodec.isEmpty() ? QString{} : QStringLiteral(" · ") + state_->actualCodec));
    const auto seconds = (std::max)(std::int64_t{0}, std::chrono::duration_cast<std::chrono::seconds>(state_->elapsed).count());
    state_->timer->setText(QStringLiteral("%1:%2 / %3:00").arg(seconds / 60, 2, 10, QChar(u'0'))
        .arg(seconds % 60, 2, 10, QChar(u'0')).arg(state_->format == recording::Format::Gif ? 6 : 120, 2, 10, QChar(u'0')));
    QString status;
    switch (state_->phase) {
    case S::Preparing: status = text(u"recording.preparing", "Preparing recording…"); break;
    case S::Recording: status = text(u"recording.active", "Recording"); break;
    case S::Stopping: status = text(u"recording.stopping", "Stopping capture…"); break;
    case S::Finalizing: status = text(u"recording.finalizing", "Completing the media file…"); break;
    case S::Archiving: status = text(u"recording.archiving", "Checking and saving the complete recording…"); break;
    case S::Completed: status = text(u"recording.saved", "Recording saved"); break;
    case S::Cancelled: status = text(u"recording.cancelled", "Recording cancelled"); break;
    case S::Discarded: status = text(u"recording.discarded", "Recording discarded"); break;
    case S::RecoverableFailure: status = text(u"recording.recoverable", "Recording failed; the available file has been retained"); break;
    case S::Failed: status = text(u"recording.failed", "Recording failed"); break;
    default: break;
    }
    if (!state_->message.isEmpty()) status += (status.isEmpty() ? QString{} : QChar(u'\n')) + state_->message;
    if (!state_->path.isEmpty()) status += QChar(u'\n') + state_->path;
    state_->status->setText(status); state_->status->setVisible(!status.isEmpty());
    state_->status->setProperty("error", state_->phase == S::Failed || state_->phase == S::RecoverableFailure);
    theme::repolish(state_->status);
    const bool running = active(state_->phase);
    const bool editable = !running && !state_->pendingOutput && !state_->starting;
    state_->mp4->setEnabled(editable); state_->gif->setEnabled(editable);
    state_->system->setEnabled(editable && state_->format == recording::Format::Mp4);
    state_->microphone->setEnabled(editable && state_->format == recording::Format::Mp4);
    state_->start->setVisible(!running); state_->start->setEnabled(editable && state_->region.width > 0 && state_->region.height > 0);
    const bool stoppable = !state_->stopPending && !state_->pendingOutput && (state_->phase == S::Recording || state_->phase == S::Preparing);
    for (auto* button : {state_->discard, state_->save, state_->copy}) {
        button->setVisible(running); button->setEnabled(stoppable); button->setAccessibleName(button->toolTip());
    }
    state_->file->setVisible(!state_->path.isEmpty()); state_->file->setEnabled(!state_->pendingOutput);
}
void RecordingWindow::closeEvent(QCloseEvent* event)
{ event->accept(); Q_EMIT viewClosed(); }
void RecordingWindow::changeEvent(QEvent* event)
{ QWidget::changeEvent(event); if (event->type() == QEvent::LanguageChange) refresh(); }

} // namespace HuhuCapture::UI::Qt
