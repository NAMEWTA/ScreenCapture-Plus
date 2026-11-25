#include "ui/qt/HistoryWindow.h"

#include "ui/qt/DesktopTheme.h"
#include <QAbstractButton>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace Theme = DesktopTheme;
constexpr qsizetype pageSize = 20;
constexpr QSize thumbnailSize{320, 320};

bool sameEntry(const Media::Entry& left, const Media::Entry& right)
{ return left.path == right.path && left.snapshot == right.snapshot; }

QString formatName(Media::Kind kind)
{
    switch (kind) {
    case Media::Kind::Png: return QStringLiteral("PNG");
    case Media::Kind::Gif: return QStringLiteral("GIF");
    case Media::Kind::Mp4: return QStringLiteral("MP4");
    }
    return {};
}

QString captureTime(const QString& name)
{
    // Presentation of the service's managed filename, never admission, naming
    // or sorting logic. Invalid names stay visible without a fabricated date.
    const auto date = QDateTime::fromString(name.left(15), QStringLiteral("yyyyMMdd-HHmmss"));
    return date.isValid() ? date.toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss")) : QString();
}

QString byteSize(quint64 bytes)
{
    if (bytes < 1024) return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024) return QLocale().toString(static_cast<double>(bytes) / 1024, 'f', 1) + QStringLiteral(" KB");
    if (bytes < 1024ull * 1024 * 1024) return QLocale().toString(static_cast<double>(bytes) / (1024 * 1024), 'f', 1) + QStringLiteral(" MB");
    return QLocale().toString(static_cast<double>(bytes) / (1024ull * 1024 * 1024), 'f', 1) + QStringLiteral(" GB");
}

class ThumbnailButton final : public QAbstractButton {
public:
    explicit ThumbnailButton(Media::Kind kind, QWidget* parent) : QAbstractButton(parent), kind_(kind)
    {
        setFixedSize(96, 60); setFocusPolicy(::Qt::StrongFocus);
        setCursor(::Qt::PointingHandCursor); setFont(Theme::font(12));
    }
    void setImage(const QImage& image) { image_ = image; update(); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(::Qt::NoPen); painter.setBrush(Theme::color(Theme::Metrics::canvas));
        painter.drawRoundedRect(rect(), 8, 8);
        if (!image_.isNull()) {
            const auto size = image_.size().scaled(rect().size(), ::Qt::KeepAspectRatio);
            const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
            painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.drawImage(target, image_);
        } else {
            // The real media type is still useful when a decoder has no cover.
            painter.setFont(font()); painter.setPen(Theme::color(Theme::Metrics::secondary));
            painter.drawText(rect(), ::Qt::AlignCenter, formatName(kind_));
        }
        if (kind_ != Media::Kind::Png) {
            const QRect badge(width() - 40, height() - 22, 36, 18);
            painter.setBrush(Theme::color(Theme::Metrics::blue)); painter.setPen(::Qt::NoPen);
            painter.drawRoundedRect(badge, 5, 5); painter.setPen(::Qt::white);
            painter.drawText(badge, ::Qt::AlignCenter, formatName(kind_));
        }
        if (hasFocus()) {
            painter.setPen(QPen(Theme::color(Theme::Metrics::focusBlue), 2)); painter.setBrush(::Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);
        }
    }
private:
    Media::Kind kind_;
    QImage image_;
};

class HistoryRow final : public QFrame {
public:
    HistoryRow(qsizetype position, const Media::Entry& entry, QWidget* parent) : QFrame(parent), index(position)
    {
        setProperty("card", true); setObjectName(QStringLiteral("historyRow%1").arg(position));
        setFixedHeight(84); setFocusPolicy(::Qt::NoFocus);
        auto* line = new QHBoxLayout(this); line->setContentsMargins(12, 12, 12, 12); line->setSpacing(12);
        preview = new ThumbnailButton(entry.kind, this); preview->setObjectName(QStringLiteral("historyPreview%1").arg(position));
        line->addWidget(preview);
        auto* text = new QWidget(this); auto* labels = new QVBoxLayout(text);
        labels->setContentsMargins(0, 0, 0, 0); labels->setSpacing(4);
        title = new Theme::ElidingLabel(text); title->setFont(Theme::font()); title->setFullText(entry.name);
        metadata = new Theme::ElidingLabel(text); metadata->setFont(Theme::font(12)); metadata->setProperty("secondary", true);
        labels->addStretch(); labels->addWidget(title); labels->addWidget(metadata); labels->addStretch();
        text->setMinimumWidth(0); line->addWidget(text, 1);
        auto* actions = new QHBoxLayout; actions->setContentsMargins(0, 0, 0, 0); actions->setSpacing(8);
        copy = Theme::button(this, "historyCopy"); reveal = Theme::button(this, "historyReveal"); send = Theme::button(this, "historySend");
        // Reuse the native Qt action icon and focus/keyboard behavior. A
        // localized tooltip/accessibility name supplies the compact meaning.
        send->setFixedSize(32, 32); send->setIconSize({18, 18});
        send->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
        send->setStyleSheet(QStringLiteral("QPushButton { padding: 0; } QPushButton:focus { padding: 0; }"));
        actions->addWidget(copy); actions->addWidget(reveal); actions->addWidget(send); line->addLayout(actions);
        for (auto* child : findChildren<QWidget*>()) child->setProperty("historyIndex", QVariant::fromValue(position));
        setProperty("historyIndex", QVariant::fromValue(position));
    }
    qsizetype index;
    ThumbnailButton* preview{};
    Theme::ElidingLabel *title{}, *metadata{};
    QPushButton *copy{}, *reveal{}, *send{};
};

QMessageBox* confirmation(QWidget* owner, const QString& title, const QString& text)
{
    auto* box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Yes | QMessageBox::No, owner);
    box->setTextFormat(::Qt::PlainText); box->setDefaultButton(QMessageBox::No);
    box->setEscapeButton(QMessageBox::No); box->setWindowModality(::Qt::WindowModal);
    box->setAttribute(::Qt::WA_DeleteOnClose); box->setFont(Theme::font());
    return box;
}
}

struct HistoryWindow::State {
    DesktopTranslator translator;
    HistoryCapabilities capabilities;
    QString directory;
    std::vector<Media::Entry> entries;
    QVector<HistoryRow*> rows;
    QVector<Media::Entry> removalSnapshot;
    qsizetype page{}, selected{-1};
    quint64 nextToken{1}, reloadToken{}, commandToken{}, thumbnailGeneration{};
    HistoryCommand command{HistoryCommand::OpenFolder};
    bool removing{}, closed{}, loadFailed{};
    QPointer<QMessageBox> confirmation;
    QPointer<QMenu> contextMenu;
    QLabel *heading{}, *summary{}, *status{}, *shortcuts{}, *emptyHeading{}, *emptyHint{};
    Theme::ElidingLabel* path{};
    QPushButton *open{}, *refresh{}, *clear{}, *previous{}, *next{}, *close{};
    QScrollArea* scroll{};
    QWidget *rowsHost{}, *empty{};
    QVBoxLayout* rowsLayout{};
};

HistoryWindow::HistoryWindow(QWidget* parent) : QWidget(parent), state_(std::make_unique<State>())
{
    setObjectName(QStringLiteral("historyWindow")); Theme::apply(this); setFocusPolicy(::Qt::StrongFocus);
    Theme::initialSize(this, {760, 560});
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(20, 18, 20, 12); outer->setSpacing(12);
    auto* title = new QHBoxLayout;
    state_->heading = Theme::label(this, 22); state_->heading->setFont(Theme::font(22, QFont::DemiBold));
    state_->close = Theme::button(this, "historyClose");
    title->addWidget(state_->heading, 1); title->addWidget(state_->close); outer->addLayout(title);
    state_->path = new Theme::ElidingLabel(this); state_->path->setFont(Theme::font(12));
    state_->path->setProperty("secondary", true); outer->addWidget(state_->path);
    auto* controls = new QHBoxLayout; controls->setSpacing(8);
    state_->open = Theme::button(this, "historyOpenFolder"); state_->refresh = Theme::button(this, "historyRefresh");
    state_->clear = Theme::button(this, "historyClear"); state_->clear->setProperty("destructive", true);
    state_->summary = Theme::label(this, 12, true);
    controls->addWidget(state_->open); controls->addWidget(state_->refresh); controls->addWidget(state_->summary, 1);
    controls->addWidget(state_->clear); outer->addLayout(controls);
    state_->scroll = new QScrollArea(this); state_->scroll->setObjectName(QStringLiteral("historyScroll"));
    state_->scroll->setWidgetResizable(true); state_->scroll->setFocusPolicy(::Qt::NoFocus);
    state_->scroll->viewport()->setAutoFillBackground(false);
    state_->rowsHost = new QWidget(state_->scroll);
    state_->rowsLayout = new QVBoxLayout(state_->rowsHost);
    state_->rowsLayout->setContentsMargins(0, 0, 0, 0); state_->rowsLayout->setSpacing(8);
    state_->rowsLayout->addStretch(); state_->scroll->setWidget(state_->rowsHost); outer->addWidget(state_->scroll, 1);
    state_->empty = new QWidget(state_->scroll->viewport()); state_->empty->setObjectName(QStringLiteral("historyEmpty"));
    state_->empty->setAttribute(::Qt::WA_TransparentForMouseEvents);
    auto* emptyLayout = new QVBoxLayout(state_->empty); emptyLayout->setContentsMargins(12, 12, 12, 12);
    state_->emptyHeading = Theme::label(state_->empty, 18); state_->emptyHeading->setFont(Theme::font(18, QFont::DemiBold));
    state_->emptyHeading->setAlignment(::Qt::AlignCenter); state_->emptyHeading->setWordWrap(true);
    state_->emptyHint = Theme::label(state_->empty); state_->emptyHint->setAlignment(::Qt::AlignCenter); state_->emptyHint->setWordWrap(true);
    emptyLayout->addStretch(); emptyLayout->addWidget(state_->emptyHeading); emptyLayout->addWidget(state_->emptyHint); emptyLayout->addStretch();
    state_->status = Theme::label(this, 12); state_->status->setObjectName(QStringLiteral("historyStatus"));
    state_->status->setWordWrap(true); outer->addWidget(state_->status);
    auto* footer = new QHBoxLayout; footer->setSpacing(8);
    state_->shortcuts = Theme::label(this, 12, true); state_->shortcuts->setWordWrap(true);
    state_->previous = Theme::button(this, "historyPrevious"); state_->next = Theme::button(this, "historyNext");
    footer->addWidget(state_->shortcuts, 1); footer->addWidget(state_->previous); footer->addWidget(state_->next); outer->addLayout(footer);
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
    connect(state_->close, &QPushButton::clicked, this, &QWidget::close);
    connect(state_->open, &QPushButton::clicked, this, [this] { requestCommand(HistoryCommand::OpenFolder); });
    connect(state_->refresh, &QPushButton::clicked, this, [this] { requestReload(); });
    connect(state_->clear, &QPushButton::clicked, this, [this] { requestClear(); });
    connect(state_->previous, &QPushButton::clicked, this, [this] {
        if (state_->page > 0) selectEntry((state_->page - 1) * pageSize);
    });
    connect(state_->next, &QPushButton::clicked, this, [this] { selectEntry((state_->page + 1) * pageSize); });
    refreshLanguage(); updateActions();
}

HistoryWindow::~HistoryWindow()
{
    // Children (including an open confirmation) are Qt-owned. Disconnect before
    // state destruction, so widget teardown cannot access the already-freed DTO.
    for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
}

void HistoryWindow::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refreshLanguage(); }
QString HistoryWindow::translated(QStringView key) const
{ return state_->translator ? state_->translator(key) : key.toString(); }
void HistoryWindow::setCapabilities(HistoryCapabilities capabilities)
{ state_->capabilities = capabilities; updateActions(); }
void HistoryWindow::setArchiveDirectory(const QString& path)
{ state_->directory = path; state_->path->setFullText(path); updateActions(); }
bool HistoryWindow::loading() const noexcept { return state_->reloadToken != 0; }
bool HistoryWindow::commandPending() const noexcept { return state_->commandToken != 0; }
qsizetype HistoryWindow::entryCount() const noexcept { return static_cast<qsizetype>(state_->entries.size()); }
qsizetype HistoryWindow::page() const noexcept { return state_->page; }
qsizetype HistoryWindow::selectedEntry() const noexcept { return state_->selected; }
quint64 HistoryWindow::thumbnailGeneration() const noexcept { return state_->thumbnailGeneration; }

void HistoryWindow::refreshLanguage()
{
    setWindowTitle(translated(u"clipboard.title")); state_->heading->setText(windowTitle());
    const std::array<std::pair<QPushButton*, const char16_t*>, 6> buttons{{
        {state_->close, u"clipboard.close"}, {state_->open, u"clipboard.openFolder"},
        {state_->refresh, u"clipboard.refresh"}, {state_->clear, u"clipboard.clear"},
        {state_->previous, u"clipboard.previous"}, {state_->next, u"clipboard.next"}}};
    for (const auto& [button, key] : buttons) { button->setText(translated(key)); button->setAccessibleName(button->text()); }
    state_->shortcuts->setText(translated(u"clipboard.shortcuts"));
    state_->emptyHeading->setText(translated(loading() ? u"clipboard.loading" : state_->loadFailed ? u"clipboard.loadFailed" : u"clipboard.empty"));
    state_->emptyHint->setText(translated(u"clipboard.emptyHint"));
    state_->emptyHint->setVisible(!loading() && !state_->loadFailed);
    refreshRows(); updateSummary();
}

void HistoryWindow::refreshRows()
{
    for (auto* row : state_->rows) {
        const auto& entry = state_->entries[static_cast<std::size_t>(row->index)];
        row->copy->setText(translated(u"clipboard.copy")); row->reveal->setText(translated(u"clipboard.reveal"));
        row->send->setText({}); row->send->setToolTip(translated(u"clipboard.send"));
        row->send->setAccessibleName(translated(u"clipboard.send") + QStringLiteral(" · ") + entry.name);
        row->preview->setAccessibleName(translated(u"clipboard.preview") + QStringLiteral(" · ") + entry.name);
        row->preview->setToolTip(translated(u"clipboard.preview"));
        row->metadata->setFullText(translated(entry.copyAsFile() ? u"clipboard.video" : u"clipboard.image") +
            QStringLiteral(" · ") + captureTime(entry.name) + QStringLiteral(" · ") + byteSize(entry.snapshot.bytes));
        const bool selected = row->index == state_->selected;
        if (row->property("selected").toBool() != selected) {
            row->setProperty("selected", selected); Theme::repolish(row);
        }
    }
}

void HistoryWindow::updateSummary()
{
    const auto total = entryCount(); const auto pages = (std::max)(qsizetype{1}, (total + pageSize - 1) / pageSize);
    auto count = QString::number(total) + QChar(u' ') + translated(u"clipboard.items");
    if (pages > 1) count += QStringLiteral("  ·  %1 / %2").arg(state_->page + 1).arg(pages);
    state_->summary->setText(count); state_->previous->setVisible(pages > 1); state_->next->setVisible(pages > 1);
    state_->empty->setVisible(total == 0); state_->empty->setGeometry(state_->scroll->viewport()->rect());
}

void HistoryWindow::updateStatus(const QString& text, bool error)
{
    state_->status->setText(text); state_->status->setProperty("error", error); Theme::repolish(state_->status);
    state_->status->setVisible(!text.isEmpty());
}

void HistoryWindow::updateActions()
{
    const bool active = !state_->closed && !loading() && !commandPending() && !state_->confirmation;
    const auto& capabilities = state_->capabilities;
    state_->open->setEnabled(active && capabilities.openFolder && !state_->directory.isEmpty());
    state_->refresh->setEnabled(active && capabilities.reload);
    state_->clear->setEnabled(active && capabilities.clear && !state_->entries.empty());
    state_->previous->setEnabled(active && state_->page > 0);
    state_->next->setEnabled(active && (state_->page + 1) * pageSize < entryCount());
    for (auto* row : state_->rows) {
        row->preview->setEnabled(active && capabilities.preview);
        row->copy->setEnabled(active && capabilities.copy); row->reveal->setEnabled(active && capabilities.reveal);
        row->send->setVisible(capabilities.send); row->send->setEnabled(active && capabilities.send);
    }
}

bool HistoryWindow::requestReload() { return reload(false); }
bool HistoryWindow::reload(bool preserveStatus)
{
    if (!state_->capabilities.reload || loading() || commandPending() || state_->confirmation || !state_->nextToken) return false;
    state_->closed = false; state_->reloadToken = state_->nextToken++;
    if (state_->thumbnailGeneration != (std::numeric_limits<quint64>::max)()) ++state_->thumbnailGeneration;
    state_->loadFailed = false;
    if (!preserveStatus) updateStatus({}, false);
    refreshLanguage(); updateActions();
    const auto token = state_->reloadToken;
    Q_EMIT reloadRequested(token); // no members are touched after a synchronous host callback
    return true;
}

void HistoryWindow::completeReload(quint64 token, const Media::ListResult& result)
{
    if (state_->closed || !token || token != state_->reloadToken) return;
    state_->reloadToken = 0; state_->loadFailed = static_cast<bool>(result.failure);
    if (result.failure) {
        updateStatus(translated(u"clipboard.loadFailed"), true); refreshLanguage(); updateActions(); return;
    }
    state_->entries = result.entries; state_->selected = state_->entries.empty() ? -1 : 0;
    rebuildPage(0); refreshLanguage();
}

void HistoryWindow::rebuildPage(qsizetype requested)
{
    const auto last = state_->entries.empty() ? qsizetype{0} : (entryCount() - 1) / pageSize;
    state_->page = std::clamp(requested, qsizetype{0}, last);
    if (auto* focused = QApplication::focusWidget(); focused && state_->rowsHost->isAncestorOf(focused))
        setFocus(::Qt::OtherFocusReason);
    const auto oldRows = std::exchange(state_->rows, {});
    for (auto* row : oldRows) {
        row->removeEventFilter(this);
        for (auto* child : row->findChildren<QWidget*>()) child->removeEventFilter(this);
        state_->rowsLayout->removeWidget(row); delete row;
    }
    const auto end = (std::min)(entryCount(), (state_->page + 1) * pageSize);
    QVector<Media::Entry> batch; batch.reserve(pageSize);
    for (qsizetype i = state_->page * pageSize; i < end; ++i) {
        const auto& entry = state_->entries[static_cast<std::size_t>(i)];
        auto* row = new HistoryRow(i, entry, state_->rowsHost);
        state_->rows.push_back(row); state_->rowsLayout->insertWidget(state_->rowsLayout->count() - 1, row);
        const auto action = [this, i](HistoryCommand command) { selectEntry(i); requestCommand(command); };
        connect(row->preview, &QAbstractButton::clicked, this, [action] { action(HistoryCommand::Preview); });
        connect(row->copy, &QPushButton::clicked, this, [action] { action(HistoryCommand::Copy); });
        connect(row->reveal, &QPushButton::clicked, this, [action] { action(HistoryCommand::Reveal); });
        connect(row->send, &QPushButton::clicked, this, [action] { action(HistoryCommand::Send); });
        row->installEventFilter(this);
        for (auto* child : row->findChildren<QWidget*>()) child->installEventFilter(this);
        batch.push_back(entry);
    }
    state_->scroll->verticalScrollBar()->setValue(0); refreshRows(); updateSummary(); updateActions();
    if (state_->thumbnailGeneration == (std::numeric_limits<quint64>::max)()) return;
    const auto generation = ++state_->thumbnailGeneration;
    // Page switches/closure can invalidate the batch before dispatch. Qt's
    // context-bound callback is discarded on destruction, with no worker join.
    QTimer::singleShot(0, this, [this, generation, batch = std::move(batch)] {
        if (!state_->closed && generation == state_->thumbnailGeneration && !batch.isEmpty())
            Q_EMIT thumbnailsRequested(generation, batch, thumbnailSize);
    });
}

bool HistoryWindow::completeThumbnail(quint64 generation, const Media::Entry& entry, const QImage& image)
{
    if (state_->closed || generation != state_->thumbnailGeneration || loading()) return false;
    if (!image.isNull() && (image.width() > thumbnailSize.width() || image.height() > thumbnailSize.height() ||
        image.sizeInBytes() > thumbnailSize.width() * thumbnailSize.height() * 8)) return false;
    for (auto* row : state_->rows) {
        if (sameEntry(state_->entries[static_cast<std::size_t>(row->index)], entry)) {
            row->preview->setImage(image); return true;
        }
    }
    return false;
}

void HistoryWindow::selectEntry(qsizetype selected)
{
    if (selected < 0 || selected >= entryCount() || state_->closed) return;
    state_->selected = selected;
    if (selected / pageSize != state_->page) rebuildPage(selected / pageSize);
    refreshRows();
    for (auto* row : state_->rows) if (row->index == selected) state_->scroll->ensureWidgetVisible(row, 0, 0);
}

bool HistoryWindow::requestCommand(HistoryCommand command)
{
    if (state_->closed || loading() || commandPending() || state_->confirmation || !state_->nextToken) return false;
    const auto& c = state_->capabilities;
    const bool allowed = command == HistoryCommand::OpenFolder ? c.openFolder && !state_->directory.isEmpty() :
        command == HistoryCommand::Preview ? c.preview : command == HistoryCommand::Copy ? c.copy :
        command == HistoryCommand::Reveal ? c.reveal : c.send;
    if (!allowed || (command != HistoryCommand::OpenFolder && (state_->selected < 0 || state_->selected >= entryCount()))) return false;
    const Media::Entry entry = command == HistoryCommand::OpenFolder ? Media::Entry{} :
        state_->entries[static_cast<std::size_t>(state_->selected)];
    state_->command = command; state_->removing = false; state_->commandToken = state_->nextToken++;
    updateStatus({}, false); updateActions(); const auto token = state_->commandToken;
    Q_EMIT commandRequested(token, command, entry);
    return true;
}

void HistoryWindow::completeCommand(quint64 token, DesktopResult result, const QString& message)
{
    if (state_->closed || !token || state_->commandToken != token || state_->removing) return;
    state_->commandToken = 0;
    if (result == DesktopResult::Failed) updateStatus(message.isEmpty() ? translated(u"clipboard.failed") : message, true);
    else if (result == DesktopResult::Succeeded)
        updateStatus(state_->command == HistoryCommand::Copy ? translated(u"clipboard.copied") : message, false);
    updateActions();
}

bool HistoryWindow::requestClear()
{
    if (state_->closed || loading() || commandPending() || state_->confirmation || !state_->capabilities.clear ||
        state_->entries.empty() || !state_->nextToken) return false;
    QVector<Media::Entry> snapshot;
    snapshot.reserve(entryCount()); for (const auto& entry : state_->entries) snapshot.push_back(entry);
    auto* box = confirmation(this, windowTitle(), translated(u"clipboard.clearConfirm"));
    box->setObjectName(QStringLiteral("historyClearConfirmation")); state_->confirmation = box;
    connect(box, &QMessageBox::finished, this, [this, box, snapshot = std::move(snapshot)](int answer) {
        if (state_->confirmation != box) return;
        state_->confirmation.clear();
        if (state_->closed || answer != QMessageBox::Yes || !state_->capabilities.clear) { updateActions(); return; }
        state_->commandToken = state_->nextToken++; state_->removing = true; state_->removalSnapshot = snapshot;
        updateStatus({}, false); updateActions();
        const auto token = state_->commandToken;
        Q_EMIT removalRequested(token, snapshot);
    });
    updateActions(); box->open(); return true;
}

bool HistoryWindow::confirmPermanentRemoval(quint64 token, const QVector<Media::Entry>& entries)
{
    if (state_->closed || !token || token != state_->commandToken || !state_->removing || state_->confirmation || entries.isEmpty()) return false;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (std::none_of(state_->removalSnapshot.cbegin(), state_->removalSnapshot.cend(),
            [&entry](const auto& original) { return sameEntry(original, entry); })) return false;
        for (qsizetype prior = 0; prior < i; ++prior) if (sameEntry(entry, entries[prior])) return false;
    }
    auto* box = confirmation(this, windowTitle(), translated(u"clipboard.permanentConfirm"));
    box->setObjectName(QStringLiteral("historyPermanentConfirmation"));
    QStringList names; for (const auto& entry : entries) names.push_back(entry.path);
    box->setDetailedText(names.join(QChar(u'\n'))); state_->confirmation = box;
    connect(box, &QMessageBox::finished, this, [this, box, token, entries](int answer) {
        if (state_->confirmation != box || state_->commandToken != token || !state_->removing) return;
        state_->confirmation.clear();
        const bool approved = !state_->closed && answer == QMessageBox::Yes;
        updateActions(); Q_EMIT permanentRemovalConfirmed(token, entries, approved);
    });
    box->open(); return true;
}

void HistoryWindow::completeRemoval(quint64 token, DesktopResult result, const QString& message)
{
    if (state_->closed || !token || state_->commandToken != token || !state_->removing) return;
    state_->commandToken = 0; state_->removing = false; state_->removalSnapshot.clear();
    if (state_->confirmation) { state_->confirmation->reject(); state_->confirmation.clear(); }
    updateStatus(result == DesktopResult::Failed && message.isEmpty() ? translated(u"clipboard.failed") : message,
        result == DesktopResult::Failed);
    updateActions(); reload(true);
}

bool HistoryWindow::handleKey(QKeyEvent* event)
{
    const auto key = event->key(); const bool control = event->modifiers().testFlag(::Qt::ControlModifier);
    if (key == ::Qt::Key_Escape) { close(); return true; }
    if (key == ::Qt::Key_F5) { requestReload(); return true; }
    if (control && key == ::Qt::Key_C) { requestCommand(HistoryCommand::Copy); return true; }
    if (loading() || commandPending() || state_->confirmation) return false;
    if (key == ::Qt::Key_PageUp || key == ::Qt::Key_PageDown) {
        if (control) {
            const auto nextPage = key == ::Qt::Key_PageUp ? state_->page - 1 : state_->page + 1;
            if (nextPage >= 0 && nextPage * pageSize < entryCount()) selectEntry(nextPage * pageSize);
        } else {
            auto* scroll = state_->scroll->verticalScrollBar();
            scroll->setValue(scroll->value() + (key == ::Qt::Key_PageUp ? -1 : 1) * scroll->pageStep());
        }
        return true;
    }
    if (key == ::Qt::Key_Up || key == ::Qt::Key_Down || key == ::Qt::Key_Home || key == ::Qt::Key_End) {
        if (state_->entries.empty()) return true;
        const auto selected = (std::max)(qsizetype{0}, state_->selected);
        selectEntry(key == ::Qt::Key_Home ? 0 : key == ::Qt::Key_End ? entryCount() - 1 :
            std::clamp(selected + (key == ::Qt::Key_Up ? -1 : 1), qsizetype{0}, entryCount() - 1));
        for (auto* row : state_->rows) if (row->index == state_->selected) row->preview->setFocus(::Qt::OtherFocusReason);
        return true;
    }
    if (key == ::Qt::Key_Enter || key == ::Qt::Key_Return || key == ::Qt::Key_Space) {
        if (auto* button = qobject_cast<QAbstractButton*>(QApplication::focusWidget()); button && isAncestorOf(button)) {
            if (button->isEnabled()) button->click();
        } else requestCommand(HistoryCommand::Preview);
        return true;
    }
    return false;
}

bool HistoryWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == state_->scroll->viewport() && event->type() == QEvent::Resize)
        state_->empty->setGeometry(state_->scroll->viewport()->rect());
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress) {
        const auto value = watched->property("historyIndex");
        if (value.isValid()) selectEntry(value.toLongLong());
    }
    if (event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent*>(event))) return true;
    if (event->type() == QEvent::ContextMenu && state_->capabilities.send && !state_->closed) {
        const auto value = watched->property("historyIndex");
        if (value.isValid()) {
            const auto index = value.toLongLong();
            if (index < 0 || index >= entryCount()) return false;
            selectEntry(index);
            const auto selected = state_->entries[static_cast<std::size_t>(index)];
            if (state_->contextMenu) state_->contextMenu->close();
            auto* menu = new QMenu(this); menu->setObjectName(QStringLiteral("historyContextMenu"));
            menu->setAttribute(::Qt::WA_DeleteOnClose); menu->setFont(Theme::font()); state_->contextMenu = menu;
            auto* send = menu->addAction(translated(u"clipboard.send")); send->setObjectName(QStringLiteral("historyContextSend"));
            send->setEnabled(!loading() && !commandPending() && !state_->confirmation);
            connect(send, &QAction::triggered, this, [this, selected] {
                const auto found = std::find_if(state_->entries.begin(), state_->entries.end(),
                    [&](const auto& entry) { return sameEntry(entry, selected); });
                if (found == state_->entries.end() || state_->closed) return;
                selectEntry(static_cast<qsizetype>(std::distance(state_->entries.begin(), found)));
                requestCommand(HistoryCommand::Send);
            });
            const auto* context = static_cast<QContextMenuEvent*>(event);
            auto position = context->globalPos();
            if (context->reason() == QContextMenuEvent::Keyboard)
                if (auto* widget = qobject_cast<QWidget*>(watched)) position = widget->mapToGlobal(widget->rect().center());
            menu->popup(position); event->accept(); return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void HistoryWindow::keyPressEvent(QKeyEvent* event)
{ if (handleKey(event)) event->accept(); else QWidget::keyPressEvent(event); }
void HistoryWindow::closeEvent(QCloseEvent* event)
{
    event->accept(); state_->closed = true;
    const QPointer<HistoryWindow> lifetime(this);
    if (state_->contextMenu) state_->contextMenu->close();
    if (state_->confirmation) state_->confirmation->reject();
    if (!lifetime) return;
    state_->reloadToken = 0; state_->commandToken = 0; state_->removing = false;
    if (state_->thumbnailGeneration != (std::numeric_limits<quint64>::max)()) ++state_->thumbnailGeneration;
    updateActions(); Q_EMIT viewClosed();
}
void HistoryWindow::showEvent(QShowEvent* event)
{ state_->closed = false; updateActions(); QWidget::showEvent(event); }
void HistoryWindow::changeEvent(QEvent* event)
{ if (event->type() == QEvent::LanguageChange) refreshLanguage(); QWidget::changeEvent(event); }

} // namespace HuhuCapture::UI::Qt
