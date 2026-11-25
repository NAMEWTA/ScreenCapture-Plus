#include "ui/qt/SettingsWindow.h"

#include "app/Brand.h"
#include "core/ShortcutKeys.h"
#include "ui/qt/AboutPanel.h"
#include "ui/qt/DesktopTheme.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListView>
#include <QMoveEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <array>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace Theme = DesktopTheme;
QString brand(std::wstring_view value) { return QString::fromWCharArray(value.data(), static_cast<qsizetype>(value.size())); }

class ToggleSwitch final : public QCheckBox {
public:
    explicit ToggleSwitch(QWidget* parent) : QCheckBox(parent) { setFixedSize(44, 32); setCursor(::Qt::PointingHandCursor); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(::Qt::NoPen);
        painter.setBrush(isEnabled() && isChecked() ? Theme::color(Theme::Metrics::blue) : QColor(160, 160, 165));
        painter.drawRoundedRect(QRectF(4, 6, 36, 20), 10, 10);
        painter.setBrush(::Qt::white); painter.drawEllipse(QRectF(isChecked() ? 22 : 6, 8, 16, 16));
        if (hasFocus()) {
            painter.setBrush(::Qt::NoBrush); painter.setPen(QPen(Theme::color(Theme::Metrics::focusBlue), 2));
            painter.drawRoundedRect(QRectF(1, 2, 42, 28), 9, 9);
        }
    }
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
};

class LanguageDelegate final : public QStyledItemDelegate {
public:
    explicit LanguageDelegate(QComboBox* owner) : QStyledItemDelegate(owner), owner_(owner) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override
    { return {180, index.data(::Qt::AccessibleDescriptionRole).toString() == QStringLiteral("separator") ? 8 : 32}; }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (index.data(::Qt::AccessibleDescriptionRole).toString() == QStringLiteral("separator")) {
            painter->save(); painter->setPen(Theme::color(Theme::Metrics::hairline));
            painter->drawLine(option.rect.left() + 12, option.rect.center().y(), option.rect.right() - 12, option.rect.center().y());
            painter->restore(); return;
        }
        auto text = option; text.rect.adjust(26, 0, -8, 0); text.font = owner_->font();
        QStyledItemDelegate::paint(painter, text, index);
        if (index.data(::Qt::UserRole).toString() == owner_->property("selectedLanguage").toString() &&
            !index.data(::Qt::UserRole).toString().isEmpty()) {
            painter->save(); painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(Theme::color(Theme::Metrics::blue), 1.6, ::Qt::SolidLine, ::Qt::RoundCap, ::Qt::RoundJoin));
            const QPoint center(option.rect.left() + 13, option.rect.center().y());
            painter->drawLine(center + QPoint(-4, 0), center + QPoint(-1, 3));
            painter->drawLine(center + QPoint(-1, 3), center + QPoint(5, -4)); painter->restore();
        }
    }
private:
    QComboBox* owner_;
};

class LanguageChoice final : public QComboBox {
public:
    explicit LanguageChoice(QWidget* parent) : QComboBox(parent)
    {
        setFixedSize(180, 32); setMaxVisibleItems(8); setEditable(false);
        auto* list = new QListView(this); list->setWordWrap(false); list->setUniformItemSizes(false);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setMaximumHeight(8 * 32 + 12); setView(list);
        setItemDelegate(new LanguageDelegate(this));
    }
protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        const auto key = event->key();
        // Qt's closed QComboBox normally commits Up/Down immediately. Opening
        // its existing popup first preserves the application's explicit-commit
        // contract while reusing Qt navigation, geometry, focus and scrolling.
        if (!view()->isVisible() && (key == ::Qt::Key_Up || key == ::Qt::Key_Down || key == ::Qt::Key_Home ||
            key == ::Qt::Key_End || key == ::Qt::Key_PageUp || key == ::Qt::Key_PageDown ||
            (!event->text().isEmpty() && !event->modifiers().testFlag(::Qt::ControlModifier) &&
                !event->modifiers().testFlag(::Qt::AltModifier)))) {
            showPopup(); QApplication::sendEvent(view(), event); event->accept(); return;
        }
        if (key == ::Qt::Key_Return || key == ::Qt::Key_Enter || key == ::Qt::Key_Space) { showPopup(); event->accept(); return; }
        QComboBox::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class ShortcutRecorder final : public QKeySequenceEdit {
public:
    explicit ShortcutRecorder(QWidget* parent) : QKeySequenceEdit(parent)
    {
        setMaximumSequenceLength(1); setFinishingKeyCombinations({}); setClearButtonEnabled(false);
        setFixedHeight(32);
        connect(this, &QKeySequenceEdit::editingFinished, this, [this] { finish(); });
    }
    std::function<void(const QKeySequence&, bool, bool)> completed;
    std::function<void()> canceled;
    bool recording() const noexcept { return recording_; }
    void begin(const QString& prompt)
    {
        stop(); normalKey_ = 0; multiple_ = false; keypad_ = false; recording_ = true;
        if (auto* line = findChild<QLineEdit*>()) line->setPlaceholderText(prompt);
        setFocus(::Qt::OtherFocusReason);
    }
    void stop() { recording_ = false; setKeySequence({}); }
protected:
    bool event(QEvent* event) override
    {
        if (recording_ && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == ::Qt::Key_Escape) { cancel(); return true; }
            if (!key->isAutoRepeat() && key->key() != ::Qt::Key_Control && key->key() != ::Qt::Key_Alt &&
                key->key() != ::Qt::Key_Shift && key->key() != ::Qt::Key_Meta) {
                if (normalKey_ && normalKey_ != key->key()) multiple_ = true;
                if (!normalKey_) { normalKey_ = key->key(); keypad_ = key->modifiers().testFlag(::Qt::KeypadModifier); }
            }
        }
        return QKeySequenceEdit::event(event);
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (recording_ && event->type() == QEvent::MouseButtonPress) { cancel(); return true; }
        return QKeySequenceEdit::eventFilter(watched, event);
    }
    void keyReleaseEvent(QKeyEvent* event) override
    {
        // Qt performs layout-aware key translation. A single chord completes
        // on the first release, matching the existing capture interaction.
        const QPointer<ShortcutRecorder> lifetime(this);
        QKeySequenceEdit::keyReleaseEvent(event);
        if (!lifetime) return;
        if (recording_ && !event->isAutoRepeat()) finish();
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        const QPointer<ShortcutRecorder> lifetime(this);
        if (recording_) cancel();
        if (lifetime) QKeySequenceEdit::focusOutEvent(event);
    }
private:
    void finish()
    {
        if (!recording_) return;
        recording_ = false; const auto sequence = keySequence();
        if (completed) completed(sequence, keypad_, multiple_);
    }
    void cancel()
    { if (!recording_) return; recording_ = false; if (canceled) canceled(); }
    bool recording_{}, multiple_{}, keypad_{};
    int normalKey_{};
};

QString normalKeyName(::Qt::Key key, bool keypad)
{
    if (key >= ::Qt::Key_A && key <= ::Qt::Key_Z) return QString(QChar(static_cast<char16_t>(key)));
    if (key >= ::Qt::Key_0 && key <= ::Qt::Key_9)
        return (keypad ? QStringLiteral("Num") : QString()) + QChar(static_cast<char16_t>(key));
    if (key >= ::Qt::Key_F1 && key <= ::Qt::Key_F12) return QStringLiteral("F%1").arg(key - ::Qt::Key_F1 + 1);
    switch (key) {
    case ::Qt::Key_Up: return QStringLiteral("Up");
    case ::Qt::Key_Down: return QStringLiteral("Down");
    case ::Qt::Key_Left: return QStringLiteral("Left");
    case ::Qt::Key_Right: return QStringLiteral("Right");
    case ::Qt::Key_Return: case ::Qt::Key_Enter: return QStringLiteral("Enter");
    case ::Qt::Key_Escape: return QStringLiteral("Esc");
    case ::Qt::Key_Tab: case ::Qt::Key_Backtab: return QStringLiteral("Tab");
    case ::Qt::Key_Space: return QStringLiteral("Space");
    case ::Qt::Key_Backspace: return QStringLiteral("Backspace");
    case ::Qt::Key_Delete: return QStringLiteral("Delete");
    case ::Qt::Key_Insert: return QStringLiteral("Insert");
    case ::Qt::Key_Home: return QStringLiteral("Home");
    case ::Qt::Key_End: return QStringLiteral("End");
    case ::Qt::Key_PageUp: return QStringLiteral("PageUp");
    case ::Qt::Key_PageDown: return QStringLiteral("PageDown");
    case ::Qt::Key_Print: return QStringLiteral("PrintScreen");
    case ::Qt::Key_ScrollLock: return QStringLiteral("ScrollLock");
    case ::Qt::Key_Pause: return QStringLiteral("Pause");
    default: break;
    }
    if (key >= ::Qt::Key_Exclam && key <= ::Qt::Key_AsciiTilde) return QString(QChar(static_cast<char16_t>(key)));
    return {};
}

QStringList shortcutTokens(const QKeySequence& sequence, bool keypad, bool multiple)
{
    if (multiple || sequence.count() != 1) return {};
    const auto chord = sequence[0]; const auto normal = normalKeyName(chord.key(), keypad);
    std::vector<std::wstring> keys;
    if (chord.keyboardModifiers().testFlag(::Qt::ControlModifier)) keys.emplace_back(L"Ctrl");
    if (chord.keyboardModifiers().testFlag(::Qt::AltModifier)) keys.emplace_back(L"Alt");
    if (chord.keyboardModifiers().testFlag(::Qt::ShiftModifier) || chord.key() == ::Qt::Key_Backtab) keys.emplace_back(L"Shift");
    if (chord.keyboardModifiers().testFlag(::Qt::MetaModifier)) keys.emplace_back(L"Win");
    keys.push_back(normal.toStdWString());
    if (Core::validateShortcutKeys(keys, !normal.isEmpty()) != Core::ShortcutValidity::Valid) return {};
    Core::normalizeShortcutKeys(keys);
    QStringList result; for (const auto& key : keys) result.push_back(QString::fromStdWString(key)); return result;
}

QLabel* localizedLabel(QWidget* parent, const char16_t* key, int size = 14, bool secondary = false)
{
    auto* label = Theme::label(parent, size, secondary); label->setProperty("languageKey", QString::fromUtf16(key));
    label->setWordWrap(true); return label;
}

void heading(QVBoxLayout* layout, QWidget* parent, const char16_t* title, const char16_t* description)
{
    auto* label = localizedLabel(parent, title, 22); label->setFont(Theme::font(22, QFont::DemiBold));
    layout->addWidget(label); layout->addSpacing(2); layout->addWidget(localizedLabel(parent, description, 12, true)); layout->addSpacing(16);
}
}

struct SettingsWindow::State {
    DesktopTranslator translator;
    SettingsSnapshot snapshot;
    quint64 nextToken{1}, pendingToken{};
    SettingField pendingField{SettingField::AutoStart};
    bool captureActive{};
    QVBoxLayout* contentLayout{};
    QStackedWidget *pages{}, *shortcutControls{};
    QWidget *general{}, *shortcut{};
    AboutPanel* about{};
    std::array<QPushButton*, 3> navigation{};
    QPushButton *close{}, *archive{}, *shortcutButton{}, *reset{};
    ToggleSwitch* autoStart{};
    LanguageChoice* language{};
    ShortcutRecorder* recorder{};
    Theme::ElidingLabel* archivePath{};
    QLabel *footer{}, *archiveDescription{}, *shortcutHint{}, *status{};
};

SettingsWindow::SettingsWindow(QWidget* parent) : QWidget(parent), state_(std::make_unique<State>())
{
    setObjectName(QStringLiteral("settingsWindow")); Theme::apply(this);
    Theme::initialSize(this, {720, 500});
    auto* outer = new QHBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    auto* sidebar = new QWidget(this); sidebar->setFixedWidth(160); sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    sidebar->setStyleSheet(QStringLiteral("QWidget#settingsSidebar { background: white; }"));
    auto* menu = new QVBoxLayout(sidebar); menu->setContentsMargins(12, 40, 12, 20); menu->setSpacing(4);
    auto* name = Theme::label(sidebar, 15); name->setFont(Theme::font(15, QFont::DemiBold)); name->setText(brand(Brand::name));
    name->setContentsMargins(10, 0, 0, 0); menu->addWidget(name); menu->addSpacing(20);
    constexpr std::array<const char16_t*, 3> navigationKeys{u"setting.common", u"setting.shortcut", u"setting.about"};
    for (std::size_t i = 0; i < state_->navigation.size(); ++i) {
        auto* button = Theme::button(sidebar, "settingsNavigation"); button->setCheckable(true);
        button->setObjectName(QStringLiteral("settingsNavigation%1").arg(i));
        button->setProperty("languageKey", QString::fromUtf16(navigationKeys[i])); button->setProperty("navigation", true); button->setFixedHeight(36);
        state_->navigation[i] = button; menu->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i] { setPage(static_cast<Page>(i)); });
    }
    menu->addStretch(); outer->addWidget(sidebar);
    auto* right = new QWidget(this); auto* columnHost = new QHBoxLayout(right);
    columnHost->setContentsMargins(24, 0, 24, 0); columnHost->setSpacing(0); columnHost->addStretch();
    auto* column = new QWidget(right); column->setMaximumWidth(560); column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    state_->contentLayout = new QVBoxLayout(column); state_->contentLayout->setContentsMargins(0, 12, 0, 24); state_->contentLayout->setSpacing(8);
    auto* closeRow = new QHBoxLayout; closeRow->addStretch(); state_->close = Theme::button(column, "settingsClose");
    state_->close->setProperty("languageKey", QStringLiteral("clipboard.close")); closeRow->addWidget(state_->close);
    state_->contentLayout->addLayout(closeRow);
    auto* scroll = new QScrollArea(column); scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true); scroll->setFocusPolicy(::Qt::NoFocus);
    state_->pages = new QStackedWidget; state_->pages->setObjectName(QStringLiteral("settingsPages"));
    makeGeneral(); makeShortcut();
    state_->about = new AboutPanel; state_->pages->addWidget(state_->about); scroll->setWidget(state_->pages);
    state_->contentLayout->addWidget(scroll, 1);
    state_->status = Theme::label(column, 12); state_->status->setObjectName(QStringLiteral("settingsStatus")); state_->status->setWordWrap(true);
    state_->contentLayout->addWidget(state_->status); state_->status->hide();
    columnHost->addWidget(column, 1); columnHost->addStretch(); outer->addWidget(right, 1);
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
    connect(state_->close, &QPushButton::clicked, this, &QWidget::close);
    connect(state_->about, &AboutPanel::downloadRequested, this, &SettingsWindow::downloadRequested);
    connect(state_->about, &AboutPanel::openUpdateFolderRequested, this, &SettingsWindow::openUpdateFolderRequested);
    connect(state_->about, &AboutPanel::openLinkRequested, this, &SettingsWindow::openLinkRequested);
    setPage(Page::General); refreshLanguage(); refreshValues();
}

SettingsWindow::~SettingsWindow()
{
    endShortcutCapture();
    for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
    state_->recorder->completed = {}; state_->recorder->canceled = {};
}

void SettingsWindow::makeGeneral()
{
    state_->general = new QWidget; state_->general->setObjectName(QStringLiteral("settingsGeneralPage"));
    auto* layout = new QVBoxLayout(state_->general); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    heading(layout, state_->general, u"setting.common", u"setting.commonDescription");
    auto* preferences = Theme::card(state_->general); auto* contents = new QVBoxLayout(preferences);
    contents->setContentsMargins(18, 6, 18, 6); contents->setSpacing(0);
    auto* startup = new QWidget(preferences); startup->setMinimumHeight(56); auto* row = new QHBoxLayout(startup);
    row->setContentsMargins(0, 0, 0, 0); row->setSpacing(12);
    auto* copy = new QWidget(startup); auto* labels = new QVBoxLayout(copy); labels->setContentsMargins(0, 0, 0, 0); labels->setSpacing(0);
    labels->addWidget(localizedLabel(copy, u"setting.autoStart")); labels->addWidget(localizedLabel(copy, u"setting.autoStartDescription", 12, true));
    row->addWidget(copy, 1); state_->autoStart = new ToggleSwitch(startup); state_->autoStart->setObjectName(QStringLiteral("settingsAutoStart"));
    row->addWidget(state_->autoStart); contents->addWidget(startup); contents->addWidget(Theme::divider(preferences));
    auto* language = new QWidget(preferences); language->setMinimumHeight(48); auto* languageRow = new QHBoxLayout(language);
    languageRow->setContentsMargins(0, 0, 0, 0); languageRow->setSpacing(12);
    languageRow->addWidget(localizedLabel(language, u"setting.language"), 1);
    state_->language = new LanguageChoice(language); state_->language->setObjectName(QStringLiteral("settingsLanguage"));
    languageRow->addWidget(state_->language); contents->addWidget(language); layout->addWidget(preferences); layout->addSpacing(16);
    auto* archive = Theme::card(state_->general); auto* archiveLayout = new QVBoxLayout(archive);
    archiveLayout->setContentsMargins(18, 14, 18, 14); archiveLayout->setSpacing(4);
    auto* archiveRow = new QHBoxLayout; archiveRow->setSpacing(12);
    auto* title = localizedLabel(archive, u"setting.archiveTitle"); title->setFont(Theme::font(14, QFont::DemiBold));
    archiveRow->addWidget(title, 1); state_->archive = Theme::button(archive, "settingsArchiveFolder");
    state_->archive->setProperty("languageKey", QStringLiteral("setting.openArchive")); archiveRow->addWidget(state_->archive); archiveLayout->addLayout(archiveRow);
    state_->archiveDescription = localizedLabel(archive, u"setting.archiveDescription", 12, true); archiveLayout->addWidget(state_->archiveDescription);
    state_->archivePath = new Theme::ElidingLabel(archive); state_->archivePath->setObjectName(QStringLiteral("settingsArchivePath"));
    state_->archivePath->setFont(Theme::font(12)); state_->archivePath->setProperty("secondary", true); archiveLayout->addWidget(state_->archivePath);
    archiveLayout->addWidget(localizedLabel(archive, u"setting.archiveNaming", 12, true)); layout->addWidget(archive); layout->addSpacing(12);
    state_->footer = localizedLabel(state_->general, u"setting.savedImmediately", 12, true); layout->addWidget(state_->footer); layout->addStretch();
    state_->pages->addWidget(state_->general);
    connect(state_->autoStart, &QCheckBox::clicked, this, [this](bool enabled) {
        SettingChange change; change.field = SettingField::AutoStart; change.autoStart = enabled;
        if (!requestChange(change)) refreshValues();
    });
    connect(state_->language, &QComboBox::activated, this, [this](int selected) {
        const auto code = state_->language->itemData(selected).toString();
        if (code.isEmpty()) {
            refreshValues();
            if (state_->snapshot.canOpenLinks) Q_EMIT openLinkRequested(QUrl(brand(Brand::languagesUrl)));
            return;
        }
        if (code == state_->snapshot.languageCode) return;
        SettingChange change; change.field = SettingField::Language; change.languageCode = code;
        if (!requestChange(change)) refreshValues();
    });
    connect(state_->archive, &QPushButton::clicked, this, [this] {
        if (state_->snapshot.canOpenArchive && !state_->snapshot.archiveDirectory.isEmpty()) Q_EMIT openArchiveRequested();
    });
}

void SettingsWindow::makeShortcut()
{
    state_->shortcut = new QWidget; state_->shortcut->setObjectName(QStringLiteral("settingsShortcutPage"));
    auto* layout = new QVBoxLayout(state_->shortcut); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    heading(layout, state_->shortcut, u"setting.shortcut", u"shortcut.description");
    auto* card = Theme::card(state_->shortcut); auto* row = new QHBoxLayout(card); row->setContentsMargins(18, 12, 18, 12); row->setSpacing(12);
    row->addWidget(localizedLabel(card, u"shortcut.cap"), 1);
    state_->shortcutControls = new QStackedWidget(card); state_->shortcutControls->setFixedSize(180, 32);
    state_->shortcutButton = Theme::button(state_->shortcutControls, "settingsCaptureShortcut"); state_->shortcutButton->setFont(Theme::font(14, QFont::DemiBold));
    state_->recorder = new ShortcutRecorder(state_->shortcutControls); state_->recorder->setObjectName(QStringLiteral("settingsShortcutRecorder"));
    state_->shortcutControls->addWidget(state_->shortcutButton); state_->shortcutControls->addWidget(state_->recorder); row->addWidget(state_->shortcutControls);
    layout->addWidget(card); layout->addSpacing(16);
    auto* action = new QHBoxLayout; state_->shortcutHint = Theme::label(state_->shortcut, 12, true); state_->shortcutHint->setWordWrap(true);
    state_->shortcutHint->setObjectName(QStringLiteral("settingsShortcutHint")); action->addWidget(state_->shortcutHint, 1);
    state_->reset = Theme::button(state_->shortcut, "settingsResetShortcut"); state_->reset->setProperty("languageKey", QStringLiteral("shortcut.reset"));
    action->addWidget(state_->reset); layout->addLayout(action); layout->addStretch(); state_->pages->addWidget(state_->shortcut);
    connect(state_->shortcutButton, &QPushButton::clicked, this, [this] {
        if (state_->snapshot.shortcutEditing == ShortcutEditing::SystemDialog) {
            SettingChange change; change.field = SettingField::CaptureShortcut; requestChange(change);
        } else beginShortcutCapture();
    });
    connect(state_->reset, &QPushButton::clicked, this, [this] {
        const QPointer<SettingsWindow> lifetime(this); endShortcutCapture();
        if (!lifetime) return;
        SettingChange change; change.field = SettingField::CaptureShortcut;
        change.shortcutKeys = {QStringLiteral("F1")}; requestChange(change);
    });
    state_->recorder->canceled = [this] { endShortcutCapture(); };
    state_->recorder->completed = [this](const QKeySequence& sequence, bool keypad, bool multiple) {
        const auto keys = shortcutTokens(sequence, keypad, multiple);
        const QPointer<SettingsWindow> lifetime(this); endShortcutCapture();
        if (!lifetime) return;
        if (keys.isEmpty()) { showStatus(translated(u"shortcut.invalid"), true); return; }
        SettingChange change; change.field = SettingField::CaptureShortcut; change.shortcutKeys = keys; requestChange(change);
    };
}

void SettingsWindow::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); state_->about->setTranslator(state_->translator); refreshLanguage(); }
QString SettingsWindow::translated(QStringView key) const
{ return state_->translator ? state_->translator(key) : key.toString(); }
void SettingsWindow::setApplicationVersion(const QString& version) { state_->about->setApplicationVersion(version); }
void SettingsWindow::setUpdateSnapshot(const AboutUpdateSnapshot& snapshot) { state_->about->setUpdateSnapshot(snapshot); }
const SettingsSnapshot& SettingsWindow::snapshot() const noexcept { return state_->snapshot; }
SettingsWindow::Page SettingsWindow::page() const noexcept { return static_cast<Page>(state_->pages->currentIndex()); }
AboutPanel* SettingsWindow::aboutPanel() const noexcept { return state_->about; }
bool SettingsWindow::changePending() const noexcept { return state_->pendingToken != 0; }
bool SettingsWindow::capturingShortcut() const noexcept { return state_->captureActive; }

void SettingsWindow::setSnapshot(const SettingsSnapshot& snapshot)
{
    const QPointer<SettingsWindow> lifetime(this);
    if (state_->captureActive && snapshot.shortcutEditing != ShortcutEditing::LocalCapture) endShortcutCapture();
    if (!lifetime) return;
    state_->snapshot = snapshot; refreshValues();
}

void SettingsWindow::refreshLanguage()
{
    setWindowTitle(translated(u"setting.title"));
    for (auto* widget : findChildren<QWidget*>()) {
        const auto key = widget->property("languageKey").toString(); if (key.isEmpty()) continue;
        const auto value = translated(key);
        if (auto* label = qobject_cast<QLabel*>(widget)) label->setText(value);
        else if (auto* button = qobject_cast<QAbstractButton*>(widget)) { button->setText(value); button->setAccessibleName(value); }
    }
    state_->autoStart->setAccessibleName(translated(u"setting.autoStart"));
    state_->language->setAccessibleName(translated(u"setting.language"));
    state_->recorder->setAccessibleName(translated(u"shortcut.cap"));
    state_->about->refreshLanguage(); refreshValues();
}

void SettingsWindow::refreshValues()
{
    const auto& snapshot = state_->snapshot;
    const QSignalBlocker startup(state_->autoStart); state_->autoStart->setChecked(snapshot.autoStart);
    state_->autoStart->setToolTip(snapshot.autoStartUnavailableReason);
    state_->language->hidePopup();
    const QSignalBlocker language(state_->language); state_->language->clear();
    QStringList added;
    for (const auto& item : snapshot.languages) {
        if (item.code.isEmpty() || item.name.isEmpty() || added.contains(item.code)) continue;
        state_->language->addItem(item.name, item.code); added.push_back(item.code);
        if (!snapshot.canChangeLanguage)
            if (auto* model = qobject_cast<QStandardItemModel*>(state_->language->model()))
                if (auto* entry = model->item(state_->language->count() - 1)) entry->setEnabled(false);
    }
    auto selected = state_->language->findData(snapshot.languageCode);
    if (selected < 0) selected = state_->language->findData(snapshot.effectiveLanguageCode);
    state_->language->setProperty("selectedLanguage", selected >= 0 ? state_->language->itemData(selected) : QVariant());
    if (snapshot.canOpenLinks) { state_->language->insertSeparator(state_->language->count()); state_->language->addItem(translated(u"setting.getMoreLang"), QString()); }
    state_->language->setCurrentIndex(selected);
    state_->language->setToolTip(selected >= 0 ? state_->language->itemText(selected) : QString());
    state_->archivePath->setFullText(snapshot.archiveDirectory); state_->archive->setToolTip(snapshot.archiveDirectory);
    state_->shortcutButton->setText(snapshot.captureShortcut); state_->shortcutButton->setToolTip(snapshot.captureShortcut);
    state_->shortcutHint->setText(snapshot.shortcutEditing == ShortcutEditing::SystemDialog ? translated(u"shortcut.systemManaged") : snapshot.shortcutUnavailableReason);
    state_->about->setLinksEnabled(snapshot.canOpenLinks); updateActions();
}

void SettingsWindow::setPage(Page selected)
{
    const auto index = static_cast<int>(selected); if (index < 0 || index >= state_->pages->count()) return;
    const QPointer<SettingsWindow> lifetime(this); endShortcutCapture();
    if (!lifetime) return;
    state_->language->hidePopup(); state_->pages->setCurrentIndex(index);
    for (std::size_t i = 0; i < state_->navigation.size(); ++i) state_->navigation[i]->setChecked(static_cast<int>(i) == index);
    if (state_->status) showStatus({}, false);
}

void SettingsWindow::updateActions()
{
    const bool active = !changePending(); const auto& snapshot = state_->snapshot;
    state_->autoStart->setEnabled(active && snapshot.canChangeAutoStart);
    state_->language->setEnabled(active && (snapshot.canChangeLanguage || snapshot.canOpenLinks) && state_->language->count() > 0);
    state_->archive->setEnabled(snapshot.canOpenArchive && !snapshot.archiveDirectory.isEmpty());
    state_->shortcutButton->setEnabled(active && snapshot.shortcutEditing != ShortcutEditing::Unavailable);
    state_->reset->setEnabled(active && snapshot.shortcutEditing == ShortcutEditing::LocalCapture);
}

void SettingsWindow::showStatus(const QString& text, bool error)
{
    state_->status->setText(text); state_->status->setProperty("error", error); Theme::repolish(state_->status); state_->status->setVisible(!text.isEmpty());
}

bool SettingsWindow::requestChange(const SettingChange& change)
{
    if (changePending() || !state_->nextToken) return false;
    const auto& snapshot = state_->snapshot;
    if ((change.field == SettingField::AutoStart && !snapshot.canChangeAutoStart) ||
        (change.field == SettingField::Language && !snapshot.canChangeLanguage) ||
        (change.field == SettingField::CaptureShortcut && snapshot.shortcutEditing == ShortcutEditing::Unavailable)) return false;
    state_->pendingToken = state_->nextToken++; state_->pendingField = change.field;
    const auto token = state_->pendingToken; showStatus({}, false); updateActions();
    // A language completion may refresh the model. Defer dispatch until the
    // originating Qt activation event has fully returned.
    QTimer::singleShot(0, this, [this, token, change] {
        if (state_->pendingToken == token) Q_EMIT changeRequested(token, change);
    });
    return true;
}

void SettingsWindow::completeChange(quint64 token, DesktopResult result, const SettingsSnapshot& effective, const QString& message)
{
    if (!token || state_->pendingToken != token) return;
    const auto field = state_->pendingField; state_->pendingToken = 0; setSnapshot(effective);
    if (result == DesktopResult::Failed) showStatus(message.isEmpty() ? translated(u"setting.saveFailed") : message, true);
    else if (result == DesktopResult::Succeeded)
        showStatus(message.isEmpty() && field == SettingField::CaptureShortcut ? translated(u"shortcut.saved") : message, false);
    else showStatus({}, false);
}

void SettingsWindow::beginShortcutCapture()
{
    if (changePending() || state_->snapshot.shortcutEditing != ShortcutEditing::LocalCapture) return;
    if (state_->captureActive) { endShortcutCapture(); return; }
    showStatus({}, false); state_->captureActive = true;
    state_->shortcutControls->setCurrentWidget(state_->recorder); state_->recorder->begin(translated(u"shortcut.pressKey"));
    Q_EMIT shortcutCaptureActive(true);
}
void SettingsWindow::endShortcutCapture()
{
    if (!state_->captureActive) return;
    state_->captureActive = false; state_->recorder->stop(); state_->shortcutControls->setCurrentWidget(state_->shortcutButton);
    Q_EMIT shortcutCaptureActive(false);
}

bool SettingsWindow::event(QEvent* event)
{
    // QWidget delivers WindowDeactivate through event(), not changeEvent().
    // Restore the host binding before QWidget forwards the event to children;
    // that direct signal may legitimately destroy this entire view.
    if (event->type() == QEvent::WindowDeactivate) {
        const QPointer<SettingsWindow> lifetime(this);
        endShortcutCapture();
        if (!lifetime) return true;
    }
    return QWidget::event(event);
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (state_->captureActive && event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget != state_->recorder && !state_->recorder->isAncestorOf(widget)) {
            const QPointer<SettingsWindow> lifetime(this); endShortcutCapture();
            if (!lifetime) return true;
        }
    }
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == ::Qt::Key_Escape && !state_->language->view()->isVisible()) {
        if (state_->captureActive) endShortcutCapture(); else close(); return true;
    }
    return QWidget::eventFilter(watched, event);
}

void SettingsWindow::closeEvent(QCloseEvent* event)
{
    event->accept(); const QPointer<SettingsWindow> lifetime(this);
    endShortcutCapture(); if (!lifetime) return;
    state_->language->hidePopup(); state_->pendingToken = 0; Q_EMIT viewClosed();
}
void SettingsWindow::resizeEvent(QResizeEvent* event)
{
    if (state_->language) state_->language->hidePopup();
    if (state_->footer) state_->footer->setVisible(height() >= 460);
    if (state_->archiveDescription) state_->archiveDescription->setVisible(height() >= 460);
    QWidget::resizeEvent(event);
}
void SettingsWindow::moveEvent(QMoveEvent* event)
{ if (state_->language) state_->language->hidePopup(); QWidget::moveEvent(event); }
void SettingsWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange && state_->about) refreshLanguage();
    if (event->type() == QEvent::FontChange && state_->language) state_->language->hidePopup();
    QWidget::changeEvent(event);
}

} // namespace HuhuCapture::UI::Qt
