#include "ui/qt/AboutPanel.h"
#include "ui/qt/HistoryWindow.h"
#include "ui/qt/SettingsWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFontInfo>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture;
namespace UIQ = HuhuCapture::UI::Qt;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void waitUntil(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 1500) { settle(); QThread::msleep(5); }
    require(predicate(), message);
}
void show(QWidget& widget) { widget.show(); widget.raise(); widget.activateWindow(); widget.setFocus(); settle(); }
void click(QAbstractButton& button)
{
    const QPointF local = button.rect().center(); const QPointF global = button.mapToGlobal(local.toPoint());
    QMouseEvent down(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&button, &down);
    QMouseEvent up(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&button, &up); settle();
}
void key(QWidget& widget, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {})
{
    QKeyEvent shortcutOverride(QEvent::ShortcutOverride, value, modifiers, text); QApplication::sendEvent(&widget, &shortcutOverride);
    QKeyEvent down(QEvent::KeyPress, value, modifiers, text); QApplication::sendEvent(&widget, &down);
    QKeyEvent up(QEvent::KeyRelease, value, modifiers, text); QApplication::sendEvent(&widget, &up); settle();
}
template<class T> T* child(QWidget& widget, const char* name)
{
    auto* value = widget.findChild<T*>(QString::fromLatin1(name)); require(value, name); return value;
}
QString translated(QStringView key)
{
    const std::pair<const char16_t*, const char16_t*> entries[]{
        {u"setting.title", u"huhucapture Settings"}, {u"setting.common", u"General"}, {u"setting.shortcut", u"Shortcuts"},
        {u"setting.about", u"About"}, {u"setting.commonDescription", u"Startup, language and local history"},
        {u"setting.autoStart", u"Launch at startup"}, {u"setting.autoStartDescription", u"Run in the tray when you sign in"},
        {u"setting.language", u"Language"}, {u"setting.getMoreLang", u"Get more languages"},
        {u"setting.archiveTitle", u"Capture history"}, {u"setting.archiveDescription", u"Saved and copied captures stay in your history."},
        {u"setting.archiveNaming", u"Named YYYYMMDD-HHMMSS"}, {u"setting.openArchive", u"Open folder"},
        {u"setting.savedImmediately", u"Changes are saved automatically"}, {u"setting.saveFailed", u"The setting could not be saved."},
        {u"lan.settings", u"LAN transfer"},
        {u"shortcut.cap", u"Start capture"}, {u"shortcut.description", u"Click a shortcut, then press new keys. Esc cancels."},
        {u"shortcut.pressKey", u"Press keys..."}, {u"shortcut.reset", u"Reset to F1"},
        {u"shortcut.invalid", u"Choose a supported shortcut combination."}, {u"shortcut.saved", u"Shortcut saved"},
        {u"shortcut.systemManaged", u"Choose or approve the shortcut in your desktop’s dialog."},
        {u"about.version", u"Version"}, {u"about.project", u"Project"}, {u"about.author", u"Author"},
        {u"about.description", u"Capture, annotate and record."}, {u"about.releases", u"View releases"},
        {u"update.download", u"Download latest"}, {u"update.retry", u"Retry download"}, {u"update.checking", u"Checking…"},
        {u"update.downloading", u"Downloading"}, {u"update.idle", u"The latest version will be saved to Downloads."},
        {u"update.downloaded", u"Downloaded. Exit the app before replacing it."}, {u"update.openFolder", u"Open folder"},
        {u"update.saveError", u"Cannot save. Check disk space and permissions."}, {u"update.integrityError", u"File verification failed. Please download again."},
        {u"clipboard.title", u"Clipboard history"}, {u"clipboard.openFolder", u"Open folder"}, {u"clipboard.refresh", u"Refresh"},
        {u"clipboard.clear", u"Clear all"}, {u"clipboard.clearConfirm", u"Move all saved captures to the Recycle Bin?"},
        {u"clipboard.permanentConfirm", u"Permanently delete these confirmed history files? This cannot be undone."},
        {u"clipboard.empty", u"No captures yet"}, {u"clipboard.emptyHint", u"Save or copy a capture to keep it here."},
        {u"clipboard.loading", u"Loading history…"}, {u"clipboard.copy", u"Copy"}, {u"clipboard.reveal", u"Show in folder"},
        {u"clipboard.send", u"Send"}, {u"clipboard.copied", u"Copied"}, {u"clipboard.failed", u"Could not complete this action."},
        {u"clipboard.loadFailed", u"Could not read history."}, {u"clipboard.previous", u"Previous"}, {u"clipboard.next", u"Next"},
        {u"clipboard.items", u"items"}, {u"clipboard.image", u"Image"}, {u"clipboard.video", u"Video"},
        {u"clipboard.preview", u"Open preview"}, {u"clipboard.close", u"Close"},
        {u"clipboard.shortcuts", u"↑↓ Select · Ctrl+C Copy · Enter Open · F5 Refresh"}
    };
    for (const auto& [name, value] : entries) if (key == name) return QString::fromUtf16(value);
    return key.toString();
}
void save(QWidget& widget, const QString& directory, const QString& name)
{ if (!directory.isEmpty()) require(widget.grab().save(QDir(directory).filePath(name)), "real desktop component PNG output"); }

Media::ListResult fixtureEntries(int count)
{
    // Deliberately nonexistent paths: these are component DTO fixtures, not
    // simulated service success. If widgets start scanning/decoding paths they
    // cannot manufacture this list or successfully complete a real command.
    Media::ListResult result;
    for (int i = 0; i < count; ++i) {
        Media::Entry entry;
        entry.name = QStringLiteral("20261125-120000-%1.%2").arg(i, 3, 10, QChar(u'0')).arg(i % 3 == 0 ? QStringLiteral("png") : i % 3 == 1 ? QStringLiteral("gif") : QStringLiteral("mp4"));
        entry.path = QStringLiteral("/component-fixtures/截图/") + entry.name;
        entry.kind = i % 3 == 0 ? Media::Kind::Png : i % 3 == 1 ? Media::Kind::Gif : Media::Kind::Mp4;
        entry.snapshot.identity = {7, static_cast<std::uint64_t>(i + 1)}; entry.snapshot.bytes = 2048 + i;
        result.entries.push_back(entry);
    }
    return result;
}

void historyContracts(const QString& directory)
{
    UIQ::HistoryWindow window; window.setTranslator(translated); show(window);
    require(!window.requestReload() && !window.requestCommand(UIQ::HistoryCommand::Copy), "unconnected services cannot report fake history success");
    window.setArchiveDirectory(QStringLiteral("/home/测试用户/Documents/huhucapture"));
    window.setCapabilities({true, true, true, true, true, true, true});
    quint64 listing{}; QVector<Media::Entry> thumbnails; quint64 generation{};
    QObject::connect(&window, &UIQ::HistoryWindow::reloadRequested, &window, [&](quint64 token) { listing = token; });
    QObject::connect(&window, &UIQ::HistoryWindow::thumbnailsRequested, &window,
        [&](quint64 value, const QVector<Media::Entry>& entries, QSize maximum) {
            require(entries.size() <= 20 && maximum == QSize(320, 320), "bounded thumbnail page request");
            generation = value; thumbnails = entries;
        });
    struct Command { quint64 token; UIQ::HistoryCommand command; Media::Entry entry; };
    std::vector<Command> commands;
    QObject::connect(&window, &UIQ::HistoryWindow::commandRequested, &window,
        [&](quint64 token, UIQ::HistoryCommand command, const Media::Entry& entry) { commands.push_back({token, command, entry}); });
    require(window.requestReload() && listing && window.loading(), "real async listing token");
    const auto entries = fixtureEntries(25); window.completeReload(listing + 1, entries);
    require(window.loading() && window.entryCount() == 0, "stale listing ignored");
    window.completeReload(listing, entries); settle();
    require(!window.loading() && window.entryCount() == 25 && thumbnails.size() == 20, "first bounded page snapshot");
    const auto* compactSend = child<QPushButton>(window, "historySend");
    require(compactSend->size() == QSize(32, 32) && compactSend->text().isEmpty() && !compactSend->icon().isNull() &&
        compactSend->toolTip() == translated(u"clipboard.send") && compactSend->accessibleName().contains(entries.entries[0].name),
        "History Send is a 32-DIP native icon with complete localized accessibility");
    QImage cover(160, 80, QImage::Format_ARGB32); cover.fill(QColor(35, 132, 176));
    require(window.completeThumbnail(generation, thumbnails.front(), cover), "actual image accepted for the exact media snapshot");
    auto changed = thumbnails.front(); ++changed.snapshot.revision[0];
    require(!window.completeThumbnail(generation, changed, cover), "changed file snapshot cannot replace an existing cover");
    require(!window.completeThumbnail(generation, thumbnails.front(), QImage(321, 20, QImage::Format_ARGB32)), "oversized cover rejected before display caching");
    const auto oldGeneration = generation; const auto oldEntry = thumbnails.front();
    key(window, Qt::Key_End);
    require(window.selectedEntry() == 24 && window.page() == 1 && thumbnails.size() == 5, "End navigates across pages using the full value snapshot");
    require(!window.completeThumbnail(oldGeneration, oldEntry, cover), "late previous-page cover ignored");
    key(window, Qt::Key_C, Qt::ControlModifier);
    require(window.commandPending() && commands.back().command == UIQ::HistoryCommand::Copy && commands.back().entry.path == entries.entries[24].path,
        "copy request contains the selected identity and waits for the real host");
    const auto copy = commands.back().token;
    window.completeCommand(copy + 1, UIQ::DesktopResult::Succeeded);
    require(window.commandPending(), "unrelated command completion ignored");
    window.completeCommand(copy, UIQ::DesktopResult::Failed, QStringLiteral("Clipboard busy"));
    require(!window.commandPending() && window.entryCount() == 25 && child<QLabel>(window, "historyStatus")->text() == QStringLiteral("Clipboard busy"),
        "copy failure preserves history and reports the actual failure");
    window.completeCommand(copy, UIQ::DesktopResult::Succeeded);
    require(child<QLabel>(window, "historyStatus")->text() == QStringLiteral("Clipboard busy"), "late success cannot overwrite failed operation");
    key(window, Qt::Key_Home);
    auto* focusedPreview = QApplication::focusWidget(); require(focusedPreview, "row preview has keyboard focus");
    key(*focusedPreview, Qt::Key_Tab);
    auto* focusedCopy = QApplication::focusWidget();
    require(focusedCopy && focusedCopy->objectName() == QStringLiteral("historyCopy"), "native Tab reaches the real row copy button");
    key(*focusedCopy, Qt::Key_Return);
    require(window.commandPending() && commands.back().command == UIQ::HistoryCommand::Copy && commands.back().entry.path == entries.entries[0].path,
        "Enter activates the focused action, not an unrelated preview");
    window.completeCommand(commands.back().token, UIQ::DesktopResult::Cancelled);
    key(window, Qt::Key_PageDown);
    require(child<QScrollArea>(window, "historyScroll")->verticalScrollBar()->value() > 0, "PageDown scrolls the current bounded page");
    key(window, Qt::Key_Home); key(window, Qt::Key_Down);
    require(window.selectedEntry() == 1, "keyboard row selection");
    require(window.requestCommand(UIQ::HistoryCommand::Send) && commands.back().entry.copyAsFile(), "GIF is sent as the original file-valued entry");
    window.completeCommand(commands.back().token, UIQ::DesktopResult::Cancelled);
    auto* contextTarget = child<QPushButton>(window, "historySend");
    QContextMenuEvent context(QContextMenuEvent::Mouse, contextTarget->rect().center(), contextTarget->mapToGlobal(contextTarget->rect().center()));
    QApplication::sendEvent(contextTarget, &context); settle();
    auto* sendMenu = child<QMenu>(window, "historyContextMenu");
    require(sendMenu->actions().size() == 1 && sendMenu->actions().front()->isEnabled(), "Actual history row context menu exposes Send");
    const auto beforeContext = commands.size(); sendMenu->actions().front()->trigger(); sendMenu->close(); settle();
    require(commands.size() == beforeContext + 1 && commands.back().command == UIQ::HistoryCommand::Send &&
        commands.back().entry.snapshot == entries.entries[0].snapshot, "Context Send dispatches the exact clicked media identity");
    window.completeCommand(commands.back().token, UIQ::DesktopResult::Cancelled);
    window.selectEntry(0); require(window.requestCommand(UIQ::HistoryCommand::Copy) && !commands.back().entry.copyAsFile(), "PNG copy keeps its image-kind contract");
    window.completeCommand(commands.back().token, UIQ::DesktopResult::Succeeded);
    require(child<QLabel>(window, "historyStatus")->text() == translated(u"clipboard.copied"), "copy success is shown only after host completion");
    const auto beforeMove = window.pos(); window.refreshLanguage(); window.completeThumbnail(generation, thumbnails.front(), cover); settle();
    require(window.pos() == beforeMove, "refresh and thumbnail completion never recenter the history window");
    save(window, directory, QStringLiteral("history-window.png"));

    quint64 removal{}; QVector<Media::Entry> removalEntries;
    QObject::connect(&window, &UIQ::HistoryWindow::removalRequested, &window,
        [&](quint64 token, const QVector<Media::Entry>& snapshot) { removal = token; removalEntries = snapshot; });
    require(window.requestClear() && !removal, "clear cannot start before actual UI confirmation");
    auto* question = child<QMessageBox>(window, "historyClearConfirmation");
    require(question->defaultButton() == question->button(QMessageBox::No), "clear confirmation defaults to keeping files");
    click(*question->button(QMessageBox::No));
    require(!removal && window.entryCount() == 25, "declined clear retains all snapshots");
    require(window.requestClear(), "clear can be retried after cancellation");
    question = child<QMessageBox>(window, "historyClearConfirmation"); click(*question->button(QMessageBox::Yes));
    require(removal && removalEntries.size() == 25 && window.commandPending(), "confirmed clear carries the entire original list, not only the page");
    auto unrelated = removalEntries.front(); unrelated.path += QStringLiteral(".other");
    require(!window.confirmPermanentRemoval(removal, {unrelated}), "permanent deletion cannot admit an unconfirmed path");
    bool permanentApproved = true; int permanentAnswers = 0;
    QObject::connect(&window, &UIQ::HistoryWindow::permanentRemovalConfirmed, &window,
        [&](quint64 token, const QVector<Media::Entry>& subset, bool approved) {
            require(token == removal && subset.size() == 1, "second confirmation retains the removal identity");
            ++permanentAnswers; permanentApproved = approved;
        });
    require(window.confirmPermanentRemoval(removal, {removalEntries.front()}), "recycle failure requires a distinct second confirmation");
    question = child<QMessageBox>(window, "historyPermanentConfirmation"); click(*question->button(QMessageBox::No));
    require(permanentAnswers == 1 && !permanentApproved, "declining permanent removal is an explicit host result");
    window.completeRemoval(removal, UIQ::DesktopResult::Cancelled);
    require(window.loading(), "clear completion refreshes remaining files even after cancellation");
    window.completeReload(listing, fixtureEntries(1)); settle();
    require(child<QPushButton>(window, "historyNext")->isHidden() && child<QPushButton>(window, "historyPrevious")->isHidden(), "pagination is absent for a single page");
    require(window.requestCommand(UIQ::HistoryCommand::Send), "host-owned transfer request");
    int closed = 0; QObject::connect(&window, &UIQ::HistoryWindow::viewClosed, &window, [&] { ++closed; });
    window.close(); settle();
    require(closed == 1 && !window.isVisible() && !window.commandPending(), "closing detaches view state without issuing a transfer cancellation");
    window.completeCommand(commands.back().token, UIQ::DesktopResult::Succeeded);
    require(!window.isVisible(), "closed history ignores late transfer completion");
    std::cout << "PASS shared history controls, immutable snapshots, page bounds, async failures and both removal confirmations\n";
}

UIQ::SettingsSnapshot settingsSnapshot()
{
    UIQ::SettingsSnapshot snapshot;
    snapshot.canChangeAutoStart = true; snapshot.canChangeLanguage = true; snapshot.canOpenArchive = true; snapshot.canOpenLinks = true;
    snapshot.languageCode = snapshot.effectiveLanguageCode = QStringLiteral("en-US");
    snapshot.languages = {{QStringLiteral("English"), QStringLiteral("en-US"), {}},
        {QStringLiteral("简体中文"), QStringLiteral("zh-CN"), {}},
        {QStringLiteral("日本語"), QStringLiteral("ja-JP"), {}}};
    snapshot.archiveDirectory = QStringLiteral("/home/测试用户/Documents/very-long-directory-name/截图与视频/huhucapture");
    snapshot.captureShortcut = QStringLiteral("F1"); snapshot.shortcutEditing = UIQ::ShortcutEditing::LocalCapture;
    return snapshot;
}

void settingsContracts(const QString& directory)
{
    UIQ::SettingsWindow window; window.setTranslator(translated); auto effective = settingsSnapshot(); window.setSnapshot(effective);
    window.setApplicationVersion(QStringLiteral("1.2.3")); show(window);
    auto* lan = child<QPushButton>(window, "settingsLan"); require(!lan->isVisible(), "Unconnected LAN settings entry is absent");
    unsigned lanRequests{}; QObject::connect(&window, &UIQ::SettingsWindow::lanSettingsRequested, &window, [&] { ++lanRequests; });
    effective.canOpenLanSettings = true; window.setSnapshot(effective); settle(); click(*lan);
    require(lanRequests == 1 && lan->text() == translated(u"lan.settings"), "General settings opens the shared LAN view through its real host boundary");
    effective.canOpenLanSettings = false; window.setSnapshot(effective); settle();
    struct Change { quint64 token; UIQ::SettingChange value; }; std::vector<Change> changes;
    QObject::connect(&window, &UIQ::SettingsWindow::changeRequested, &window,
        [&](quint64 token, const UIQ::SettingChange& value) { changes.push_back({token, value}); });
    auto* startup = child<QCheckBox>(window, "settingsAutoStart"); click(*startup);
    require(changes.size() == 1 && changes.back().value.autoStart && window.changePending() && !window.snapshot().autoStart,
        "startup requests a real change without preemptively changing effective state");
    window.completeChange(changes.back().token + 1, UIQ::DesktopResult::Succeeded, effective);
    require(window.changePending(), "settings stale completion ignored");
    window.completeChange(changes.back().token, UIQ::DesktopResult::Failed, effective, QStringLiteral("Read-only configuration"));
    require(!startup->isChecked() && !window.changePending() && child<QLabel>(window, "settingsStatus")->text() == QStringLiteral("Read-only configuration"),
        "failed settings write rolls back to the truthful effective snapshot");
    click(*startup); effective.autoStart = true;
    window.completeChange(changes.back().token, UIQ::DesktopResult::Succeeded, effective);
    require(startup->isChecked() && window.snapshot().autoStart, "effective startup updates only after success");
    auto* language = child<QComboBox>(window, "settingsLanguage"); const auto beforeLanguage = changes.size();
    language->setFocus(); key(*language, Qt::Key_Down);
    waitUntil([&] { return language->view()->isVisible(); }, "real language popup opens");
    require(language->view()->isVisible() && changes.size() == beforeLanguage && language->currentData().toString() == QStringLiteral("en-US"),
        "language arrows highlight inside the real Qt popup without saving");
    key(*language->view(), Qt::Key_Escape);
    require(!language->view()->isVisible() && changes.size() == beforeLanguage, "language Escape cancels without writing configuration");
    language->showPopup(); settle(); language->view()->setCurrentIndex(language->model()->index(1, 0));
    key(*language->view(), Qt::Key_Return);
    require(changes.size() == beforeLanguage + 1 && changes.back().value.languageCode == QStringLiteral("zh-CN"), "Enter commits a selected language once");
    effective.languageCode = effective.effectiveLanguageCode = QStringLiteral("zh-CN");
    window.completeChange(changes.back().token, UIQ::DesktopResult::Succeeded, effective);
    require(language->currentData().toString() == QStringLiteral("zh-CN"), "language button reflects host-confirmed selection");
    int links = 0; QObject::connect(&window, &UIQ::SettingsWindow::openLinkRequested, &window, [&](const QUrl& url) { require(url.scheme() == QStringLiteral("https"), "real project HTTPS link"); ++links; });
    language->showPopup(); settle(); language->view()->setCurrentIndex(language->model()->index(language->count() - 1, 0));
    key(*language->view(), Qt::Key_Return);
    require(links == 1 && changes.size() == beforeLanguage + 1 && language->currentData().toString() == QStringLiteral("zh-CN"), "more-languages is an action, never a persisted language code");
    save(window, directory, QStringLiteral("settings-general.png"));

    window.setPage(UIQ::SettingsWindow::Page::Shortcut);
    std::vector<bool> capture;
    QObject::connect(&window, &UIQ::SettingsWindow::shortcutCaptureActive, &window, [&](bool active) { capture.push_back(active); });
    auto* start = child<QPushButton>(window, "settingsCaptureShortcut"); auto* recorder = child<QKeySequenceEdit>(window, "settingsShortcutRecorder");
    click(*start); require(window.capturingShortcut() && capture.back(), "explicit shortcut capture suspends the host binding");
    key(*recorder, Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    require(!window.capturingShortcut() && !capture.back() && !window.changePending() && child<QLabel>(window, "settingsStatus")->text() == translated(u"shortcut.invalid"),
        "original reserved editing shortcuts are rejected before host registration");
    click(*start); key(*recorder, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("a"));
    require(window.changePending() && changes.back().value.shortcutKeys == QStringList({QStringLiteral("Ctrl"), QStringLiteral("Alt"), QStringLiteral("A")}),
        "Qt key recorder emits one canonical legacy chord");
    effective.captureShortcut = QStringLiteral("Ctrl+Alt+A"); window.completeChange(changes.back().token, UIQ::DesktopResult::Succeeded, effective);
    require(start->text() == effective.captureShortcut && child<QLabel>(window, "settingsStatus")->text() == translated(u"shortcut.saved"), "shortcut saved status waits for registration and persistence");
    click(*start); key(*recorder, Qt::Key_Escape);
    require(!window.capturingShortcut() && window.isVisible(), "Escape cancels capture without closing settings");
    click(*start); window.setPage(UIQ::SettingsWindow::Page::General);
    require(!window.capturingShortcut() && !capture.back(), "page navigation restores global shortcut handling");
    window.setPage(UIQ::SettingsWindow::Page::Shortcut); effective.shortcutEditing = UIQ::ShortcutEditing::SystemDialog;
    window.setSnapshot(effective); click(*start);
    require(window.changePending() && changes.back().value.shortcutKeys.isEmpty() && !window.capturingShortcut(),
        "same shortcut control requests system-managed authorization without pretending a local binding exists");
    window.completeChange(changes.back().token, UIQ::DesktopResult::Cancelled, effective);
    require(!child<QPushButton>(window, "settingsResetShortcut")->isEnabled(), "local reset is disabled for system-controlled shortcuts");
    save(window, directory, QStringLiteral("settings-shortcuts.png"));

    window.setPage(UIQ::SettingsWindow::Page::About);
    auto* download = child<QPushButton>(window, "settingsDownloadUpdate"); int downloads = 0;
    QObject::connect(&window, &UIQ::SettingsWindow::downloadRequested, &window, [&] { ++downloads; });
    require(!download->isEnabled() && child<QLabel>(window, "settingsVersion")->text() == QStringLiteral("1.2.3"), "application version comes from the host and updates require a real capability");
    UIQ::AboutUpdateSnapshot update; update.canDownload = true; window.setUpdateSnapshot(update); click(*download);
    require(downloads == 1, "download begins only after an explicit user action");
    update.stage = UIQ::UpdateStage::Downloading; update.version = QStringLiteral("1.2.4"); update.received = 40; update.total = 100;
    window.setUpdateSnapshot(update);
    require(!download->isEnabled() && child<QProgressBar>(window, "settingsUpdateProgress")->value() == 40, "actual known-length download progress");
    update.total = 0; window.setUpdateSnapshot(update);
    require(child<QProgressBar>(window, "settingsUpdateProgress")->maximum() == 0, "unknown-length progress stays indeterminate");
    update.stage = UIQ::UpdateStage::Failed; update.error = UIQ::UpdateError::Integrity; window.setUpdateSnapshot(update);
    require(download->isEnabled() && child<QLabel>(window, "settingsUpdateStatus")->text() == translated(u"update.integrityError"), "real verification failure enables explicit retry");
    update.stage = UIQ::UpdateStage::Downloaded; update.file = QStringLiteral("/home/测试用户/Downloads/huhucapture.AppImage"); update.canOpenFolder = true;
    window.setUpdateSnapshot(update); QString folder;
    QObject::connect(&window, &UIQ::SettingsWindow::openUpdateFolderRequested, &window, [&](const QString& file) { folder = file; });
    click(*child<QPushButton>(window, "settingsUpdateFolder")); require(folder == update.file, "update folder action retains the exact host download path");
    const auto position = window.pos(); window.refreshLanguage(); settle(); require(window.pos() == position, "settings refresh never moves a user-placed window");
    save(window, directory, QStringLiteral("settings-about.png"));
    window.close(); settle(); require(downloads == 1, "closing settings never restarts or cancels an application-owned download");
    std::cout << "PASS settings snapshots, explicit language commit, mature shortcut policy and shared About update state\n";
}

void closeWhileWaiting()
{
    auto* window = new UIQ::HistoryWindow; window->setTranslator(translated);
    window->setCapabilities({true, false, false, false, false, false, false});
    QObject::connect(window, &UIQ::HistoryWindow::reloadRequested, window, [window](quint64 token) {
        QTimer::singleShot(0, window, [window, token] { window->completeReload(token, fixtureEntries(1)); });
    });
    show(*window); require(window->requestReload(), "lifetime test reload");
    QPointer<UIQ::HistoryWindow> lifetime(window); delete window; settle(); require(!lifetime, "queued completion is dropped with its window context");
}

void destroyOnCaptureDeactivation()
{
    {
        UIQ::SettingsWindow retained;
        retained.setTranslator(translated); retained.setSnapshot(settingsSnapshot());
        retained.setPage(UIQ::SettingsWindow::Page::Shortcut); show(retained);
        int cancellations = 0;
        QObject::connect(&retained, &UIQ::SettingsWindow::shortcutCaptureActive, &retained,
            [&cancellations](bool active) { if (!active) ++cancellations; });
        click(*child<QPushButton>(retained, "settingsCaptureShortcut"));
        require(retained.capturingShortcut(), "retained-window deactivation starts with actual shortcut capture");
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&retained, &deactivate);
        require(!retained.capturingShortcut() && cancellations == 1 && retained.isVisible(),
            "WindowDeactivate restores the binding once while preserving an open settings view");
        QApplication::sendEvent(&retained, &deactivate);
        require(cancellations == 1, "repeated WindowDeactivate cannot restore the same capture twice");
        retained.close(); settle();
    }
    auto* window = new UIQ::SettingsWindow;
    window->setTranslator(translated); window->setSnapshot(settingsSnapshot());
    window->setPage(UIQ::SettingsWindow::Page::Shortcut); show(*window);
    click(*child<QPushButton>(*window, "settingsCaptureShortcut"));
    require(window->capturingShortcut(), "deactivation lifetime check starts with actual shortcut capture");
    QPointer<UIQ::SettingsWindow> lifetime(window);
    int cancellations = 0;
    QObject::connect(window, &UIQ::SettingsWindow::shortcutCaptureActive, qApp,
        [window, &cancellations](bool active) {
            if (!active) { ++cancellations; delete window; }
        });
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(window, &deactivate);
    require(!lifetime && cancellations == 1,
        "a synchronous host may destroy settings when deactivation restores its shortcut without a second widget dispatch");
    settle();
    std::cout << "PASS actual WindowDeactivate dispatch, idempotent capture release and synchronous host destruction\n";
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    try {
        if (!directory.isEmpty()) require(QDir().mkpath(directory), "create desktop evidence directory");
        const QFontInfo font(app.font());
        std::cout << "FONT desktop platform default: " << font.family().toStdString() << ", pixel=" << font.pixelSize() << '\n';
        historyContracts(directory); settingsContracts(directory); closeWhileWaiting(); destroyOnCaptureDeactivation();
        std::cout << "PASS real Qt desktop component contracts (service assembly and physical desktop acceptance remain separate)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
