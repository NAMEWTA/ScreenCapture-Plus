#include "app/SettingsController.h"

#include "services/desktop/AutoStart.h"
#include "services/desktop/GlobalShortcut.h"
#include "services/desktop/LaunchCommand.h"
#include <QApplication>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <utility>
#include <vector>

namespace HuhuCapture::Application {
namespace {
namespace UI = HuhuCapture::UI::Qt;
QString shortcutMessage(Localization::LanguageCatalog& catalog, Desktop::ShortcutError error)
{
    using Error = Desktop::ShortcutError;
    switch (error) {
    case Error::None: case Error::Cancelled: return {};
    case Error::Busy: return catalog.text(u"shortcut.busy");
    case Error::InvalidShortcut: return catalog.text(u"shortcut.invalid");
    case Error::Conflict: return catalog.text(u"shortcut.conflict");
    case Error::Denied: return catalog.text(u"shortcut.permissionDenied");
    case Error::SaveFailed: return catalog.text(u"setting.saveFailed");
    case Error::RestoreFailed: return catalog.text(u"shortcut.restoreFailed");
    case Error::Unavailable: case Error::Unsupported: return catalog.text(u"shortcut.unavailable");
    }
    return catalog.text(u"shortcut.unavailable");
}
QString startupMessage(Localization::LanguageCatalog& catalog, Desktop::AutoStartError error)
{
    using Error = Desktop::AutoStartError;
    switch (error) {
    case Error::None: return {};
    case Error::InvalidExecutable: return catalog.text(u"setting.autoStartPathInvalid");
    case Error::ForeignEntry: return catalog.text(u"setting.autoStartForeign");
    case Error::Unavailable: case Error::Read: return catalog.text(u"setting.autoStartUnavailable");
    case Error::SaveFailed: return catalog.text(u"setting.saveFailed");
    case Error::RollbackFailed: return catalog.text(u"setting.autoStartRollbackFailed");
    case Error::Write: case Error::ChangedOnDisk: return catalog.text(u"setting.autoStartFailed");
    }
    return catalog.text(u"setting.autoStartFailed");
}
}

struct SettingsController::State {
    Config::SettingsStore& store;
    Localization::LanguageCatalog& catalog;
    Desktop::AutoStart autoStart;
    Desktop::GlobalShortcut shortcut;
    Desktop::AutoStartSnapshot startup;
    QString executable, archive, version;
    UI::AboutUpdateSnapshot update;
    QPointer<UI::SettingsWindow> window, requestView;
    UI::SettingsWindow* requestIdentity{};
    UI::SettingsWindow* captureIdentity{};
    std::vector<QPointer<UI::SettingsWindow>> views;
    quint64 shortcutRequest{}, viewToken{};
    bool ready{}, initialized{}, initializingShortcut{}, canOpenArchive{}, canOpenLinks{}, lanAvailable{};
    bool stopping{}, shutdownNotified{}, captureActive{};
    bool standardDialogLanguageAvailable{true};

    State(Config::SettingsStore& settings, Localization::LanguageCatalog& language, QString launch)
        : store(settings), catalog(language), autoStart(launch), executable(std::move(launch)) {}
    bool busy() const { return initializingShortcut || shortcutRequest != 0; }
    QStringList requestedKeys() const
    { return Desktop::parseShortcut(store.value({QStringLiteral("shortcutKey"), QStringLiteral("cap")}).toString()); }
    bool portalConfigured() const
    {
        return store.value({QStringLiteral("shortcutKey"), QStringLiteral("portal")}).toObject()
            .value(QStringLiteral("configured")).toBool(false);
    }
};

SettingsController::SettingsController(Config::SettingsStore& store, Localization::LanguageCatalog& catalog,
    QString executable, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(store, catalog, std::move(executable)))
{
    connect(&state_->catalog, &Localization::LanguageCatalog::changed, this, [this] {
        state_->shortcut.setActionDescription(QStringLiteral("huhucapture — ") + state_->catalog.text(u"shortcut.cap"));
        for (const auto& window : state_->views) if (window) window->refreshLanguage();
        refreshViews();
    });
    connect(&state_->shortcut, &Desktop::GlobalShortcut::activated, this, [this](const QString& token) {
        if (!state_->stopping && !state_->captureActive) Q_EMIT captureRequested(token);
    });
    connect(&state_->shortcut, &Desktop::GlobalShortcut::changed, this, [this] {
        if (!state_->stopping) refreshViews();
    });
    connect(&state_->shortcut, &Desktop::GlobalShortcut::initialized, this, [this] {
        state_->initializingShortcut = false;
        if (state_->stopping) { Q_EMIT busyChanged(busy()); finishShutdown(); return; }
        const auto effective = state_->shortcut.snapshot();
        const auto keys = state_->requestedKeys();
        if (effective.editing != Desktop::ShortcutEditing::Unavailable && !effective.registered &&
            !state_->shortcutRequest && (!effective.portal || state_->portalConfigured())) {
            if (keys.isEmpty()) Q_EMIT operationFailed(state_->catalog.text(u"shortcut.invalid"));
            else state_->shortcutRequest = state_->shortcut.bind(keys, nullptr, {}, effective.portal);
        } else if (effective.failure && effective.failure.code != Desktop::ShortcutError::Cancelled) {
            Q_EMIT operationFailed(shortcutMessage(state_->catalog, effective.failure.code));
        }
        Q_EMIT busyChanged(busy()); refreshViews();
    });
    connect(&state_->shortcut, &Desktop::GlobalShortcut::bindingFinished, this,
        [this](quint64 request, const Desktop::ShortcutFailure& failure) {
            if (!state_->shortcutRequest || request != state_->shortcutRequest) return;
            state_->shortcutRequest = 0;
            const auto source = state_->requestView; const auto token = std::exchange(state_->viewToken, 0);
            state_->requestView.clear(); state_->requestIdentity = nullptr;
            Q_EMIT busyChanged(busy());
            if (state_->stopping) { finishShutdown(); return; }
            const auto result = failure.code == Desktop::ShortcutError::Cancelled ? UI::DesktopResult::Cancelled :
                failure ? UI::DesktopResult::Failed : UI::DesktopResult::Succeeded;
            const auto message = shortcutMessage(state_->catalog, failure.code);
            if (source && token) source->completeChange(token, result, snapshot(), message);
            else if (failure && failure.code != Desktop::ShortcutError::Cancelled) Q_EMIT operationFailed(message);
            refreshViews();
        });
    if (qGuiApp) connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && state_->ready && !state_->stopping) {
            state_->startup = state_->autoStart.snapshot(); refreshViews();
        }
    });
}

SettingsController::~SettingsController()
{
    state_->stopping = true;
    state_->shortcut.shutdown();
    for (const auto& pointer : state_->views) if (pointer) {
        auto* window = pointer.data(); disconnect(window, nullptr, this, nullptr); delete window;
    }
}

bool SettingsController::initialize()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_->stopping) return false;
    if (state_->initialized) return state_->ready;
    state_->initialized = true;
    // Failed SettingsStore::load leaves no document. Never manufacture a new
    // store or overwrite a malformed/future-schema config from this layer.
    if (state_->store.document().isEmpty()) {
        Q_EMIT operationFailed(state_->catalog.text(u"setting.invalidConfiguration")); return false;
    }
    const auto language = state_->store.value({QStringLiteral("common"), QStringLiteral("language")});
    if (state_->catalog.effectiveCode().isEmpty() && !state_->catalog.load(QStringLiteral("en-US")).usable) {
        Q_EMIT operationFailed(state_->catalog.text(u"setting.languageFailed")); return false;
    }
    if (language.isString() && !language.toString().isEmpty() && state_->catalog.requestedCode() != language.toString()) {
        const auto loaded = state_->catalog.load(language.toString());
        if (!loaded.usable) { Q_EMIT operationFailed(state_->catalog.text(u"setting.languageFailed")); return false; }
        if (loaded.fellBack) Q_EMIT operationFailed(state_->catalog.text(u"setting.languageFallback"));
    }
    else if (!language.isString() || language.toString().isEmpty())
        Q_EMIT operationFailed(state_->catalog.text(u"setting.invalidConfiguration"));
    state_->ready = true;
    state_->startup = state_->autoStart.snapshot();
    state_->shortcut.setActionDescription(QStringLiteral("huhucapture — ") + state_->catalog.text(u"shortcut.cap"));
    state_->initializingShortcut = true; Q_EMIT busyChanged(true); state_->shortcut.initialize();
    refreshViews(); return true;
}

UI::SettingsSnapshot SettingsController::snapshot() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    UI::SettingsSnapshot result;
    const bool active = state_->ready && !state_->stopping;
    result.autoStart = state_->startup.enabled;
    result.canChangeAutoStart = active && !busy() && state_->startup.canChange;
    result.autoStartUnavailableReason = startupMessage(state_->catalog, state_->startup.error);
    result.canChangeLanguage = active && !busy();
    result.languageCode = state_->store.value({QStringLiteral("common"), QStringLiteral("language")}).toString();
    result.effectiveLanguageCode = state_->catalog.effectiveCode();
    if (!state_->standardDialogLanguageAvailable) result.standardDialogLanguageNote = state_->catalog.text(u"setting.qtLanguageFallback");
    result.languages = state_->catalog.available();
    result.archiveDirectory = state_->archive;
    result.canOpenArchive = active && state_->canOpenArchive && !state_->archive.isEmpty();
    result.canOpenLinks = active && state_->canOpenLinks;
    result.canOpenLanSettings = active && state_->lanAvailable;
    const auto shortcut = state_->shortcut.snapshot();
    result.captureShortcut = (!shortcut.registered && !shortcut.suspended) || shortcut.actualTrigger.isEmpty() ?
        state_->catalog.text(u"shortcut.notRegistered") : shortcut.actualTrigger;
    if (active && !busy()) {
        switch (shortcut.editing) {
        case Desktop::ShortcutEditing::LocalCapture: result.shortcutEditing = UI::ShortcutEditing::LocalCapture; break;
        case Desktop::ShortcutEditing::SystemDialog: result.shortcutEditing = UI::ShortcutEditing::SystemDialog; break;
        case Desktop::ShortcutEditing::Unavailable: break;
        }
    }
    if (busy()) result.shortcutUnavailableReason = state_->catalog.text(state_->initializingShortcut ?
        u"shortcut.checking" : (shortcut.portal ? u"shortcut.authorizationPending" : u"shortcut.busy"));
    else if (shortcut.failure) result.shortcutUnavailableReason = shortcutMessage(state_->catalog, shortcut.failure.code);
    else if (!shortcut.registered && !shortcut.suspended && shortcut.editing != Desktop::ShortcutEditing::SystemDialog)
        result.shortcutUnavailableReason = state_->catalog.text(u"shortcut.unavailable");
    if (active && !shortcut.registered && !shortcut.suspended)
        result.shortcutCommand = Desktop::shortcutLaunchCommand(state_->executable);
    return result;
}

UI::SettingsWindow* SettingsController::openSettings(UI::SettingsWindow::Page page)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_->stopping) return nullptr;
    if (!state_->initialized) initialize();
    state_->startup = state_->autoStart.snapshot();
    const auto native = state_->shortcut.snapshot();
    if (state_->ready && !busy() && native.portal && native.editing == Desktop::ShortcutEditing::Unavailable) {
        state_->initializingShortcut = true; Q_EMIT busyChanged(true); state_->shortcut.initialize();
    }
    if (state_->window) {
        auto* window = state_->window.data(); window->setSnapshot(snapshot()); window->setPage(page);
        window->showNormal(); window->raise(); window->activateWindow(); return window;
    }
    state_->views.erase(std::remove_if(state_->views.begin(), state_->views.end(), [](const auto& view) { return view.isNull(); }), state_->views.end());
    auto* window = new UI::SettingsWindow;
    window->setAttribute(Qt::WA_DeleteOnClose); state_->window = window; state_->views.push_back(window);
    window->setTranslator([catalog = &state_->catalog](QStringView key) { return catalog->text(key); });
    window->setApplicationVersion(state_->version); window->setUpdateSnapshot(state_->update);
    window->setSnapshot(snapshot()); window->setPage(page);
    connect(window, &UI::SettingsWindow::changeRequested, this, [this, window](quint64 token, const UI::SettingChange& change) {
        changeRequested(window, token, change);
    });
    connect(window, &UI::SettingsWindow::shortcutCaptureActive, this, [this, window](bool active) {
        if (state_->stopping) return;
        if (!active && state_->captureIdentity != window) return;
        state_->captureIdentity = active ? window : nullptr; state_->captureActive = active;
        const auto failure = state_->shortcut.setCaptureActive(active);
        if (failure) {
            if (active) window->setPage(window->page());
            Q_EMIT operationFailed(shortcutMessage(state_->catalog, failure.code));
        }
    });
    connect(window, &UI::SettingsWindow::captureRequested, this, [this] {
        if (!state_->stopping) Q_EMIT captureRequested({});
    });
    connect(window, &UI::SettingsWindow::openArchiveRequested, this, &SettingsController::openArchiveRequested);
    connect(window, &UI::SettingsWindow::lanSettingsRequested, this, &SettingsController::lanSettingsRequested);
    connect(window, &UI::SettingsWindow::openLinkRequested, this, &SettingsController::openLinkRequested);
    connect(window, &UI::SettingsWindow::downloadRequested, this, &SettingsController::downloadRequested);
    connect(window, &UI::SettingsWindow::openUpdateFolderRequested, this, &SettingsController::openUpdateFolderRequested);
    connect(window, &UI::SettingsWindow::viewClosed, this, [this, window] { releaseView(window); });
    connect(window, &QObject::destroyed, this, [this, window] { releaseView(window); });
    window->show(); window->raise(); window->activateWindow(); return window;
}

void SettingsController::releaseView(UI::SettingsWindow* window)
{
    if (state_->window == window) state_->window.clear();
    if (state_->captureIdentity == window) {
        state_->captureIdentity = nullptr; state_->captureActive = false;
        const auto failure = state_->shortcut.setCaptureActive(false);
        if (!state_->stopping && failure) Q_EMIT operationFailed(shortcutMessage(state_->catalog, failure.code));
    }
    if (state_->requestIdentity == window && state_->shortcutRequest) state_->shortcut.cancel(state_->shortcutRequest);
}

void SettingsController::changeRequested(UI::SettingsWindow* source, quint64 token, const UI::SettingChange& change)
{
    if (state_->stopping || !state_->ready || source != state_->window || !source->changePending()) return;
    if (busy()) { source->completeChange(token, UI::DesktopResult::Failed, snapshot(), state_->catalog.text(u"shortcut.busy")); return; }
    if (change.field == UI::SettingField::AutoStart) {
        const auto result = state_->autoStart.setEnabled(change.autoStart, [this, enabled = change.autoStart] {
            return state_->store.setValue({QStringLiteral("common"), QStringLiteral("autoStart")}, enabled).ok();
        });
        state_->startup = result.effective;
        source->completeChange(token, result.ok() ? UI::DesktopResult::Succeeded : UI::DesktopResult::Failed,
            snapshot(), startupMessage(state_->catalog, result.error));
        refreshViews(); return;
    }
    if (change.field == UI::SettingField::Language) {
        const QPointer<UI::SettingsWindow> sourceLife(source);
        bool known{};
        for (const auto& entry : state_->catalog.available()) if (entry.code == change.languageCode) { known = true; break; }
        const auto previous = state_->store.value({QStringLiteral("common"), QStringLiteral("language")});
        if (!known || !state_->store.setValue({QStringLiteral("common"), QStringLiteral("language")}, change.languageCode).ok()) {
            source->completeChange(token, UI::DesktopResult::Failed, snapshot(), state_->catalog.text(u"setting.saveFailed")); return;
        }
        const auto loaded = state_->catalog.load(change.languageCode);
        if (!loaded.usable) {
            // Embedded English is validated in CI. If it is actually missing
            // at runtime, keep the last usable catalog and attempt config
            // rollback; never label that failed language change successful.
            const auto restored = state_->store.setValue({QStringLiteral("common"), QStringLiteral("language")}, previous);
            (void)restored;
            if (sourceLife) sourceLife->completeChange(token, UI::DesktopResult::Failed, snapshot(), state_->catalog.text(u"setting.languageFailed"));
        } else if (sourceLife) sourceLife->completeChange(token, UI::DesktopResult::Succeeded, snapshot(),
            loaded.fellBack ? state_->catalog.text(u"setting.languageFallback") : QString());
        refreshViews(); return;
    }
    const auto native = state_->shortcut.snapshot();
    if (native.editing == Desktop::ShortcutEditing::Unavailable) {
        source->completeChange(token, UI::DesktopResult::Failed, snapshot(), state_->catalog.text(u"shortcut.unavailable")); return;
    }
    const auto keys = native.portal ? state_->requestedKeys() : Desktop::canonicalShortcut(change.shortcutKeys);
    if (keys.isEmpty()) { source->completeChange(token, UI::DesktopResult::Failed, snapshot(), state_->catalog.text(u"shortcut.invalid")); return; }
    state_->requestView = source; state_->requestIdentity = source; state_->viewToken = token;
    const QPointer<UI::SettingsWindow> sourceLife(source);
    auto persist = [this, sourceLife, keys, portal = native.portal] {
        if (state_->stopping || !sourceLife || sourceLife != state_->window || !sourceLife->changePending()) return false;
        if (!portal) return state_->store.setValue({QStringLiteral("shortcutKey"), QStringLiteral("cap")}, keys.join(QChar(u'+'))).ok();
        auto state = state_->store.value({QStringLiteral("shortcutKey"), QStringLiteral("portal")}).toObject();
        state.insert(QStringLiteral("configured"), true);
        return state_->store.setValue({QStringLiteral("shortcutKey"), QStringLiteral("portal")}, state).ok();
    };
    state_->shortcutRequest = state_->shortcut.bind(keys, source->windowHandle(), std::move(persist));
    Q_EMIT busyChanged(busy()); refreshViews();
}

void SettingsController::refreshViews()
{
    const auto effective = snapshot();
    for (const auto& window : state_->views) if (window) window->setSnapshot(effective);
    Q_EMIT stateChanged();
}
void SettingsController::setArchiveDirectory(const QString& path, bool canOpen)
{ state_->archive = path; state_->canOpenArchive = canOpen; refreshViews(); }
void SettingsController::setLanAvailable(bool available)
{ state_->lanAvailable = available; refreshViews(); }
void SettingsController::setLinksAvailable(bool available)
{ state_->canOpenLinks = available; refreshViews(); }
void SettingsController::setStandardDialogLanguageAvailable(bool available)
{
    if (state_->standardDialogLanguageAvailable == available) return;
    state_->standardDialogLanguageAvailable = available; refreshViews();
}
void SettingsController::setApplicationVersion(const QString& version)
{ state_->version = version; for (const auto& window : state_->views) if (window) window->setApplicationVersion(version); }
void SettingsController::setUpdateSnapshot(const UI::AboutUpdateSnapshot& update)
{ state_->update = update; for (const auto& window : state_->views) if (window) window->setUpdateSnapshot(update); }
bool SettingsController::busy() const { return state_->busy(); }
bool SettingsController::captureShortcutAvailable() const
{
    const auto binding = state_->shortcut.snapshot();
    return state_->ready && !state_->stopping && !state_->captureActive &&
        binding.registered && !binding.suspended;
}
void SettingsController::finishShutdown()
{
    if (!state_->stopping || busy() || state_->shutdownNotified) return;
    state_->shutdownNotified = true; Q_EMIT shutdownFinished();
}
void SettingsController::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (std::exchange(state_->stopping, true)) return;
    for (const auto& window : state_->views) if (window) window->close();
    if (state_->shortcutRequest) state_->shortcut.cancel(state_->shortcutRequest);
    state_->shortcut.shutdown(); finishShutdown();
}

} // namespace HuhuCapture::Application
