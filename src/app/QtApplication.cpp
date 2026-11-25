#include "app/QtApplication.h"

#include "app/Brand.h"
#include "app/CaptureWorkspace.h"
#include "app/LanController.h"
#include "app/MediaWorkspace.h"
#include "app/RecordingWorkspace.h"
#include "app/ScrollWorkspace.h"
#include "app/SettingsController.h"
#include "app/Version.h"
#include "services/config/SettingsStore.h"
#include "services/desktop/DesktopCaptureAction.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/desktop/WindowActivation.h"
#include "services/files/QtPaths.h"
#include "services/localization/LanguageCatalog.h"
#include "services/localization/QtTranslations.h"
#include "services/media/GifInspection.h"
#include "services/media/VideoInspection.h"
#include "services/update/Installation.h"
#include "services/update/UpdateService.h"
#include "ui/qt/DesktopTheme.h"
#include "ui/qt/HistoryWindow.h"
#include "ui/qt/TransferWindow.h"
#include "ui/qt/UpdatePresentation.h"
#ifdef Q_OS_WIN
#include "platform/windows/DesktopSession.h"
#endif

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace HuhuCapture::Application {

ApplicationArguments parseApplicationArguments(const QStringList& arguments)
{
    ApplicationArguments result;
    std::vector<QByteArray> encoded; encoded.reserve(static_cast<std::size_t>(arguments.size()));
    for (const auto& argument : arguments) {
        encoded.push_back(argument.toUtf8());
        if (argument == QStringLiteral("--portable-dir")) {
            if (result.failure.isEmpty())
                result.failure = QStringLiteral("Use --portable-dir=/absolute/path to select a portable profile");
            continue;
        }
        if (argument.startsWith(QStringLiteral("--portable-dir="))) {
            result.portableDirectory = argument.mid(15);
            if (result.portableDirectory.isEmpty()) {
                if (result.failure.isEmpty()) result.failure = QStringLiteral("The portable profile directory cannot be empty");
            }
        }
    }
    std::vector<std::string_view> values; values.reserve(encoded.size());
    for (const auto& argument : encoded) values.emplace_back(argument.constData(), static_cast<std::size_t>(argument.size()));
    result.startup = Core::parseStartupArguments<char>(values);
    if (result.startup.action == Core::StartupAction::Invalid && result.failure.isEmpty())
        result.failure = QStringLiteral("The requested application control action is not supported");
    return result;
}

namespace {
namespace ui = HuhuCapture::UI::Qt;

QString brand() { return QString::fromWCharArray(Brand::name.data(), static_cast<qsizetype>(Brand::name.size())); }
QString versionText() { return QString::fromUtf8(Version::text.data(), static_cast<qsizetype>(Version::text.size())); }
qint64 updateDay()
{ return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()).time_since_epoch().count(); }

struct Paths {
    QString settings, data, runtime, scope, executable;
    QString failure;
};

QString normalized(const QString& path)
{
    if (path.isEmpty() || path.contains(QChar(u'\0'))) return {};
    std::error_code error;
    const auto result = std::filesystem::weakly_canonical(Files::nativePath(path), error);
    return error ? QString() : Files::qtPath(result);
}

Paths applicationPaths(const QString& portable, bool independent)
{
    Paths result;
    const auto binaryDirectory = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    // Qt's AppConfigLocation is LocalAppData on Windows. The inherited product
    // used RoamingAppData, which is the actual AppDataLocation in fixed Qt.
    const auto config = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    constexpr auto policy = Config::PathPolicy::WindowsLegacy;
    result.data = config;
#else
    const auto config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    constexpr auto policy = Config::PathPolicy::Linux;
    result.data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
    QString explicitDirectory;
    if (!portable.isEmpty()) {
        if (!QDir::isAbsolutePath(portable) || portable.contains(QChar(u'\0')) ||
            portable.contains(QChar(u'\n')) || portable.contains(QChar(u'\r'))) {
            result.failure = QStringLiteral("The portable profile requires an absolute directory path"); return result;
        }
        explicitDirectory = normalized(portable);
        if (explicitDirectory.isEmpty() || (QFileInfo::exists(explicitDirectory) && !QFileInfo(explicitDirectory).isDir())) {
            result.failure = QStringLiteral("The portable profile directory is unavailable"); return result;
        }
#ifndef Q_OS_WIN
        const auto mount = normalized(qEnvironmentVariable("APPDIR"));
        if (!mount.isEmpty() && (explicitDirectory == mount || explicitDirectory.startsWith(mount + QChar(u'/')))) {
            result.failure = QStringLiteral("The AppImage mount cannot hold a persistent portable profile"); return result;
        }
#endif
        result.data = explicitDirectory;
    }
    result.settings = Config::settingsPath(policy, binaryDirectory, config, explicitDirectory);
    result.settings = normalized(result.settings);
    if (result.settings.isEmpty() || result.data.isEmpty()) {
        result.failure = QStringLiteral("The user configuration directory is unavailable"); return result;
    }
    result.executable = QCoreApplication::applicationFilePath();
#ifndef Q_OS_WIN
    // Keep an invalid provided APPIMAGE path visible to AutoStart's capability
    // checks instead of silently registering a mounted inner executable.
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")) result.executable = qEnvironmentVariable("APPIMAGE");
#endif
    if (independent) return result;
#ifdef Q_OS_WIN
    result.runtime = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const auto session = Platform::Windows::desktopSession();
    if (session.failure) { result.failure = session.failure.detail; return result; }
    const auto profile = result.settings.toCaseFolded();
    const QJsonArray scope{QStringLiteral("huhucapture"), profile, session.key};
#else
    result.runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QJsonArray scope{QStringLiteral("huhucapture"), result.settings,
        qEnvironmentVariable("XDG_SESSION_ID"), qEnvironmentVariable("WAYLAND_DISPLAY"), qEnvironmentVariable("DISPLAY")};
#endif
    result.scope = QString::fromUtf8(QJsonDocument(scope).toJson(QJsonDocument::Compact));
    if (result.runtime.isEmpty() || !QDir::isAbsolutePath(result.runtime) || !QFileInfo(result.runtime).isDir())
        result.failure = QStringLiteral("The user runtime directory is unavailable for single-instance coordination");
    return result;
}

QWindow* activeParent()
{
    auto* widget = QApplication::activeWindow();
    return widget ? widget->windowHandle() : nullptr;
}

// The no-tray entry is a small shared Qt view, not another settings/editor UI.
// Native window chrome provides moving and resizing on either platform.
class EntryWindow final : public QWidget {
public:
    enum Action : std::size_t { Capture, History, Settings, Lan, KeepClipboard, Exit, Count };
    EntryWindow()
    {
        setObjectName(QStringLiteral("applicationEntry")); setWindowTitle(brand());
        resize(420, 380); setMinimumWidth(340); ui::DesktopTheme::apply(this);
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(24, 20, 24, 20); layout->setSpacing(12);
        auto* title = ui::DesktopTheme::label(this, 22); title->setFont(ui::DesktopTheme::font(22, QFont::DemiBold));
        title->setText(brand()); layout->addWidget(title);
        message = ui::DesktopTheme::label(this, 13, true); message->setWordWrap(true);
        message->setObjectName(QStringLiteral("applicationEntryMessage")); layout->addWidget(message);
        constexpr std::array names{"applicationCapture", "applicationHistory", "applicationSettings", "applicationLan",
            "applicationKeepClipboard", "applicationExit"};
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            buttons[index] = ui::DesktopTheme::button(this, names[index]);
            buttons[index]->setMinimumHeight(32); layout->addWidget(buttons[index]);
        }
        layout->addStretch();
    }
    QLabel* message{};
    std::array<QPushButton*, Count> buttons{};
    std::function<void()> closeRequested;
protected:
    void closeEvent(QCloseEvent* event) override
    {
        event->ignore();
        if (closeRequested) closeRequested();
    }
};
}

struct QtApplication::State {
    QtApplication* owner;
    QApplication& application;
    ApplicationOptions options;
    Core::StartupRequest initial;
    ApplicationPhase phase{ApplicationPhase::Dormant};
    Paths paths;
    // Declared first so the primary lock is released after all other services
    // and their resources have been destroyed, including exceptional teardown.
    std::unique_ptr<SingleInstance> instance;
    std::unique_ptr<Config::SettingsStore> store;
    std::unique_ptr<Localization::LanguageCatalog> catalog;
    std::unique_ptr<Localization::QtTranslations> qtTranslations;
    std::shared_ptr<Media::MediaArchiveService> archive;
    std::unique_ptr<Desktop::SystemClipboard> clipboard;
    std::unique_ptr<Desktop::FileRevealer> revealer;
    std::unique_ptr<MediaWorkspace> media;
    std::unique_ptr<SettingsController> settings;
    std::unique_ptr<LanController> lan;
    std::unique_ptr<CaptureWorkspace> capture;
    std::unique_ptr<RecordingWorkspace> recording;
    std::unique_ptr<ScrollWorkspace> scroll;
    std::unique_ptr<Desktop::WindowActivation> activation;
    std::unique_ptr<Desktop::DesktopCaptureAction> desktopAction;
    std::unique_ptr<Updates::UpdateService> updates;
    std::unique_ptr<QMenu> trayMenu;
    std::unique_ptr<QSystemTrayIcon> tray;
    std::array<QAction*, 5> menuActions{};
    std::unique_ptr<EntryWindow> entry;
    QPointer<QMessageBox> errorDialog;
    QPointer<QMessageBox> updatePrompt;
    std::deque<QString> errors;
    QSet<quint64> directoryRequests;
    QTimer trayWatch;
    QTimer updateCheckTimer, updatePromptTimer;
    qint64 updateAttemptedDay{(std::numeric_limits<qint64>::min)()};
    QString promptedUpdateVersion;
    quint64 activationRequest{};
    quint64 desktopActionVerification{};
    bool checkQueued{}, journeyFinished{}, trayAvailable{};
    bool entryAfterCapture{}, entryAfterCaptureActivation{};
    bool settingsStopped{true}, lanStopped{true}, recordingStopped{true}, scrollStopped{true}, captureStopped{true}, mediaStopping{};
    bool updatesStopped{true};
    bool desktopActionStopped{true}, desktopActionRetry{};
    bool clipboardStopping{}, clipboardStopped{true};
    bool errorQueued{}, exitEmitted{}, keepClipboard{};
    int exitCode{};

    State(QtApplication* parent, QApplication& app, ApplicationOptions configuration)
        : owner(parent), application(app), options(std::move(configuration))
    {
        trayWatch.setInterval(2000);
        updateCheckTimer.setSingleShot(true); updateCheckTimer.setInterval(15000);
        updatePromptTimer.setSingleShot(true); updatePromptTimer.setInterval(5000);
    }

    QString text(QStringView key) const { return catalog ? catalog->text(key) : key.toString(); }
    bool stopping() const { return phase == ApplicationPhase::Draining || phase == ApplicationPhase::ClipboardOwner || phase == ApplicationPhase::Stopped; }
    void setPhase(ApplicationPhase value)
    {
        if (phase == value) return;
        phase = value;
        QTimer::singleShot(0, owner, [target = owner, value] { Q_EMIT target->phaseChanged(value); });
    }
    ui::DesktopTranslator translator()
    { return [language = catalog.get()](QStringView key) { return language->text(key); }; }

    void report(QString message)
    {
        if (message.isEmpty() || phase == ApplicationPhase::Stopped) return;
        // Repeated capability notifications cannot create an unbounded stack
        // of modal windows. Each distinct retained error has a real OK action.
        if (errorDialog && errorDialog->text() == message) return;
        if (std::find(errors.begin(), errors.end(), message) == errors.end()) {
            if (errors.size() == 8) errors.pop_front();
            errors.push_back(std::move(message));
        }
        if (errorQueued) return;
        errorQueued = true;
        QTimer::singleShot(0, owner, [this] { errorQueued = false; showError(); });
    }
    void showError()
    {
        if (errorDialog || errors.empty() || phase == ApplicationPhase::Stopped) return;
        auto message = std::move(errors.front()); errors.pop_front();
        auto* dialog = new QMessageBox(QMessageBox::Warning, brand(), message, QMessageBox::Ok);
        dialog->setObjectName(QStringLiteral("applicationError")); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setTextFormat(Qt::PlainText); dialog->setWindowModality(Qt::NonModal);
        errorDialog = dialog;
        QObject::connect(dialog, &QDialog::finished, owner, [this] {
            const QPointer<QtApplication> lifetime(owner);
            errorDialog.clear(); showError(); if (lifetime) scheduleCheck();
        });
        QObject::connect(dialog, &QObject::destroyed, owner, [this] { scheduleCheck(); });
        dialog->open();
        Q_EMIT owner->operationFailed(message);
    }
    void fatal(const QString& message)
    {
        // A desktop control process owns no capture, settings or UI journey.
        // Failure must not open a new window over the primary's live pixels.
        if (initial.action != Core::StartupAction::Launch) {
            qWarning().noquote() << message;
            exitCode = 2; finishExit(true); return;
        }
        exitCode = 1;
        report(message);
        owner->requestQuit(exitCode);
    }

    bool admit(const Core::StartupRequest& request)
    {
        if (phase != ApplicationPhase::Running) return false;
        switch (request.action) {
        case Core::StartupAction::VerifyCaptureAction:
            return desktopAction && desktopAction->acceptVerification(request);
        case Core::StartupAction::FinishCapture:
            if (recording && recording->canStop()) return recording->stopActive(Recording::StopIntent::Save);
            return scroll && scroll->canStop() && scroll->stopActive();
        case Core::StartupAction::Invalid:
            return false;
        case Core::StartupAction::Launch:
            break;
        }
        if (!capture) return false;
        // A second explicit background launch is a successful no-capture
        // activation, matching --auto-start and --enter=tray semantics.
        if (request.background) { if (!trayAvailable) showEntry(); return true; }
        return startJourney(request.entry);
    }
    bool startJourney(Core::CaptureEntry entryPoint, const QString& token = {})
    {
        if (phase != ApplicationPhase::Running || !capture) return false;
        verifyDesktopCaptureAction();
        const bool wasVisible = entry && entry->isVisible();
        if (wasVisible) entry->hide();
        const bool accepted = capture->start(entryPoint, token);
        if (!accepted && wasVisible) showEntry(false);
        return accepted;
    }
    void startCapture(const QString& token = {})
    {
        if (phase != ApplicationPhase::Running || !capture) return;
        if (recording && recording->busy()) {
            // The existing desktop-global capture action is an actual stop
            // route while recording. It must not open another selection or
            // a busy dialog over a source whose controls have been hidden.
            recording->stopActive(Recording::StopIntent::Save);
            return;
        }
        if (scroll && scroll->busy()) {
            scroll->stopActive();
            return;
        }
        if (!startJourney(Core::CaptureEntry::Capture, token)) report(text(u"app.captureBusy"));
    }
    void openHistory()
    {
        if (phase != ApplicationPhase::Running || !media) return;
        if (auto* window = media->openHistory()) activate(window);
    }
    void openSettings()
    {
        if (phase != ApplicationPhase::Running || !settings) return;
        if (auto* window = settings->openSettings()) activate(window);
    }
    void openLan()
    {
        if (phase != ApplicationPhase::Running || !lan) return;
        if (auto* window = lan->openSettings()) activate(window);
    }
    void activate(QWidget* window)
    {
        if (!window || !activation) return;
        window->showNormal();
        activationRequest = activation->request(window->windowHandle());
    }
    void openDirectory(const QString& path, bool create = false)
    {
        if (phase != ApplicationPhase::Running || !revealer || path.isEmpty()) return;
        const auto token = revealer->openDirectory(path, activeParent(), create);
        if (token) directoryRequests.insert(token);
    }

    void refreshLanguage()
    {
        if (qtTranslations) {
            const bool standardDialogsAvailable = qtTranslations->load(catalog->effectiveCode());
            if (settings) settings->setStandardDialogLanguageAvailable(standardDialogsAvailable);
        }
        const auto translate = translator();
        if (media) media->setTranslator(translate);
        if (capture) capture->setTranslator(translate);
        if (recording) recording->setTranslator(translate);
        if (scroll) scroll->setTranslator(translate);
        if (lan) lan->setTranslator(translate);
        if (trayMenu) {
            constexpr std::array keys{u"shortcut.runCapture", u"clipboard.title", u"tray.setting", u"lan.settings", u"tray.exit"};
            for (std::size_t index = 0; index < keys.size(); ++index) menuActions[index]->setText(text(keys[index]));
        }
        if (updatePrompt && updates) updatePrompt->setText(updatePromptText());
        refreshCaptureStopActions();
        refreshEntry();
    }

    bool updateIdle() const
    {
        return phase == ApplicationPhase::Running && !initial.oneShot &&
            !errorDialog && errors.empty() && !QApplication::activeModalWidget() && !QApplication::activePopupWidget() &&
            !hasVisibleJourney() && (!capture || !capture->busy()) && (!recording || !recording->busy()) &&
            (!scroll || !scroll->busy()) &&
            (!media || !media->busy()) && (!clipboard || !clipboard->busy()) && (!revealer || !revealer->busy()) &&
            (!settings || !settings->busy()) && (!lan || (!lan->busy() && !lan->service().hasActiveTransfers()));
    }
    bool updateCheckDue() const
    {
        if (!updates || !store || initial.oneShot) return false;
        const auto today = updateDay();
        return updateAttemptedDay < today &&
            store->value({QStringLiteral("common"), QStringLiteral("updateCheckDay")}).toInteger(0) < today;
    }
    void rememberUpdateAttempt()
    {
        updateAttemptedDay = updateDay();
        const auto saved = store->setValue({QStringLiteral("common"), QStringLiteral("updateCheckDay")},
            QJsonValue(static_cast<double>(updateAttemptedDay)));
        // The task may continue after a preference-write failure, but cannot
        // rewrite stale settings or retry silently on every idle notification.
        if (!saved.ok()) report(text(u"setting.saveFailed") + QStringLiteral("\n") + saved.detail);
    }
    void considerAutomaticUpdate()
    {
        if (!updateCheckDue() || !updateIdle() || updates->busy()) { updateCheckTimer.stop(); return; }
        if (!updateCheckTimer.isActive()) updateCheckTimer.start();
    }
    void checkUpdateWhenIdle()
    {
        if (!updateCheckDue() || !updateIdle() || updates->busy()) return;
        if (updates->check()) { promptedUpdateVersion.clear(); rememberUpdateAttempt(); }
    }
    QString updatePromptText() const
    {
        return text(u"update.found") + QStringLiteral(" v") + updates->snapshot().version +
            QStringLiteral("\n\n") + text(u"update.promptPackageDownload");
    }
    void promptForUpdate()
    {
        if (!updates || stopping() || updates->snapshot().stage != Updates::Stage::Available || updatePrompt ||
            promptedUpdateVersion == updates->snapshot().version) return;
        if (!updateIdle()) { updatePromptTimer.start(); return; }
        promptedUpdateVersion = updates->snapshot().version;
        auto* dialog = new QMessageBox(QMessageBox::Information, brand(), updatePromptText(), QMessageBox::Yes | QMessageBox::No);
        dialog->setObjectName(QStringLiteral("applicationUpdateAvailable")); dialog->setTextFormat(Qt::PlainText);
        dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::NonModal);
        dialog->setDefaultButton(QMessageBox::No); dialog->setEscapeButton(QMessageBox::No);
        updatePrompt = dialog;
        QObject::connect(dialog, &QDialog::finished, owner, [this](int answer) {
            updatePrompt.clear();
            if (answer == QMessageBox::Yes && phase == ApplicationPhase::Running) downloadUpdate();
            scheduleCheck();
        });
        dialog->open();
    }
    void downloadUpdate()
    {
        if (phase != ApplicationPhase::Running || !updates || updates->busy()) return;
        updateCheckTimer.stop(); updatePromptTimer.stop();
        if (updatePrompt) updatePrompt->reject();
        if (updates->download()) rememberUpdateAttempt();
    }
    void setupUpdates()
    {
#ifdef Q_OS_WIN
        const auto installation = Updates::readInstallation(QCoreApplication::applicationDirPath(), versionText(), u"windows", u"x64");
#else
        const auto installation = Updates::readInstallation(QCoreApplication::applicationDirPath(), versionText(), u"linux", u"amd64");
#endif
        if (!installation.packageKind) {
            ui::AboutUpdateSnapshot unavailable; unavailable.stage = ui::UpdateStage::Unavailable;
            settings->setUpdateSnapshot(unavailable); return;
        }
        Updates::Options configuration;
        configuration.downloadsDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        configuration.currentVersion = versionText(); configuration.packageKind = *installation.packageKind;
        updates = std::make_unique<Updates::UpdateService>(std::move(configuration), owner);
        QObject::connect(updates.get(), &Updates::UpdateService::changed, owner, [this](const Updates::Snapshot& value) {
            settings->setUpdateSnapshot(ui::aboutUpdateSnapshot(value));
            if (value.stage == Updates::Stage::Available && !stopping() && !updatePromptTimer.isActive()) updatePromptTimer.start();
            else if (value.stage != Updates::Stage::Available) updatePromptTimer.stop();
            scheduleCheck();
        });
        QObject::connect(updates.get(), &Updates::UpdateService::shutdownFinished, owner, [this] { updatesStopped = true; scheduleCheck(); });
        QObject::connect(&updateCheckTimer, &QTimer::timeout, owner, [this] { checkUpdateWhenIdle(); });
        QObject::connect(&updatePromptTimer, &QTimer::timeout, owner, [this] { promptForUpdate(); });
        QObject::connect(settings.get(), &SettingsController::downloadRequested, owner, [this] { downloadUpdate(); });
        QObject::connect(settings.get(), &SettingsController::openUpdateFolderRequested, owner, [this](const QString& path) {
            if (updates && !path.isEmpty() && path == updates->snapshot().file) openDirectory(QFileInfo(path).absolutePath());
        });
        settings->setUpdateSnapshot(ui::aboutUpdateSnapshot(updates->snapshot()));
    }
    void refreshEntry()
    {
        if (!entry) return;
        constexpr std::array keys{u"shortcut.runCapture", u"clipboard.title", u"tray.setting", u"lan.settings", u"app.keepClipboard", u"tray.exit"};
        const bool holding = phase == ApplicationPhase::ClipboardOwner;
        for (std::size_t index = 0; index < keys.size(); ++index) {
            auto* button = entry->buttons[index]; button->setText(text(keys[index]));
            button->setAccessibleName(button->text());
            button->setVisible(holding ? index >= EntryWindow::KeepClipboard : index != EntryWindow::KeepClipboard);
            button->setEnabled(holding || phase == ApplicationPhase::Running);
        }
        entry->buttons[EntryWindow::Settings]->setEnabled(phase == ApplicationPhase::Running && bool(settings));
        entry->buttons[EntryWindow::Lan]->setEnabled(phase == ApplicationPhase::Running && bool(lan));
        entry->buttons[EntryWindow::KeepClipboard]->setEnabled(holding && !keepClipboard);
        entry->message->setText(text(holding ? (keepClipboard ? u"app.clipboardKeeping" : u"app.clipboardExit") :
            phase == ApplicationPhase::Draining ? u"app.shuttingDown" : trayAvailable ? u"app.ready" : u"app.noTray"));
    }
    void showEntry(bool requestActivation = true)
    {
        // A disappearing tray must not put our fallback window into the
        // recording or scrolling source. Loss of the last stop route first
        // stops its worker; check() presents this entry after the source drains.
        if (phase == ApplicationPhase::Running &&
            ((recording && recording->busy()) || (scroll && scroll->busy()))) {
            entryAfterCapture = true;
            entryAfterCaptureActivation = entryAfterCaptureActivation || requestActivation;
            return;
        }
        if (!entry) {
            entry = std::make_unique<EntryWindow>();
            QObject::connect(entry->buttons[EntryWindow::Capture], &QPushButton::clicked, owner, [this] { startCapture(); });
            QObject::connect(entry->buttons[EntryWindow::History], &QPushButton::clicked, owner, [this] { openHistory(); });
            QObject::connect(entry->buttons[EntryWindow::Settings], &QPushButton::clicked, owner, [this] { openSettings(); });
            QObject::connect(entry->buttons[EntryWindow::Lan], &QPushButton::clicked, owner, [this] { openLan(); });
            QObject::connect(entry->buttons[EntryWindow::KeepClipboard], &QPushButton::clicked, owner,
                [this] { keepClipboard = true; refreshEntry(); });
            QObject::connect(entry->buttons[EntryWindow::Exit], &QPushButton::clicked, owner, [this] { owner->requestQuit(); });
            entry->closeRequested = [this] {
                if (phase == ApplicationPhase::Running && trayAvailable) entry->hide();
                else if (phase != ApplicationPhase::Draining) owner->requestQuit();
            };
        }
        refreshEntry();
        entry->setAttribute(Qt::WA_ShowWithoutActivating, !requestActivation);
        entry->show();
        if (requestActivation) activate(entry.get());
    }
    void inspectTray()
    {
        const bool available = QSystemTrayIcon::isSystemTrayAvailable();
        if (available == trayAvailable) return;
        trayAvailable = available; refreshCaptureStopActions(); refreshEntry();
        if (!available && phase == ApplicationPhase::Running) showEntry();
    }
    void refreshCaptureStopActions()
    {
        const bool journey = (recording && recording->busy()) || (scroll && scroll->busy());
        if (tray) {
            // Menus and tooltips are native surfaces of their own. A direct
            // icon click stops without mapping either over a full-screen
            // capture. Restore the ordinary menu after the journey ends.
            tray->setContextMenu(journey ? nullptr : trayMenu.get());
            tray->setToolTip(journey ? QString() : brand());
        }
        const bool nativeTray = tray && tray->isVisible() && trayAvailable &&
            QSystemTrayIcon::isSystemTrayAvailable();
        const bool nativeShortcut = settings && settings->captureShortcutAvailable();
        const bool nativeAction = desktopAction && desktopAction->available();
        const bool stopAvailable = phase == ApplicationPhase::Running && (nativeTray || nativeShortcut || nativeAction);
        const auto stopHint = !stopAvailable ? QString() : nativeTray ? text(u"capture.trayFinishHint") :
            nativeShortcut ? text(u"capture.shortcutFinishHint") :
            text(u"capture.desktopFinishHint").arg(desktopAction->actionName());
        if (recording) recording->setExternalStopAvailable(stopAvailable, stopHint);
        if (scroll) scroll->setExternalStopAvailable(stopAvailable, stopHint);
    }
    void setupTray()
    {
        trayMenu = std::make_unique<QMenu>(); ui::DesktopTheme::apply(trayMenu.get());
        for (std::size_t index = 0; index < menuActions.size(); ++index) menuActions[index] = trayMenu->addAction(QString());
        QObject::connect(menuActions[0], &QAction::triggered, owner, [this] { startCapture(); });
        QObject::connect(menuActions[1], &QAction::triggered, owner, [this] { openHistory(); });
        QObject::connect(menuActions[2], &QAction::triggered, owner, [this] { openSettings(); });
        QObject::connect(menuActions[3], &QAction::triggered, owner, [this] { openLan(); });
        QObject::connect(menuActions[4], &QAction::triggered, owner, [this] { owner->requestQuit(); });
        tray = std::make_unique<QSystemTrayIcon>(QApplication::windowIcon(), owner);
        tray->setToolTip(brand()); tray->setContextMenu(trayMenu.get());
        QObject::connect(tray.get(), &QSystemTrayIcon::activated, owner, [this](QSystemTrayIcon::ActivationReason reason) {
            if (recording && recording->busy()) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick ||
                    reason == QSystemTrayIcon::MiddleClick || reason == QSystemTrayIcon::Context)
                    recording->stopActive(Recording::StopIntent::Save);
                return;
            }
            if (scroll && scroll->busy()) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick ||
                    reason == QSystemTrayIcon::MiddleClick || reason == QSystemTrayIcon::Context)
                    scroll->stopActive();
                return;
            }
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) startCapture();
        });
        trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
        tray->show();
        refreshCaptureStopActions();
        QObject::connect(&trayWatch, &QTimer::timeout, owner, [this] { inspectTray(); scheduleCheck(); });
        trayWatch.start();
        if (!trayAvailable) showEntry();
    }

    void verifyDesktopCaptureAction()
    {
        if (!desktopAction || desktopActionVerification || phase != ApplicationPhase::Running ||
            desktopAction->available() || (recording && recording->busy()) || (scroll && scroll->busy())) return;
        desktopActionRetry = false;
        desktopActionVerification = desktopAction->verify();
    }
    void setupDesktopCaptureAction()
    {
        // A one-shot process deliberately has no primary IPC endpoint. Only
        // the normal persistent owner may offer this desktop completion route.
        if (initial.oneShot || !instance || instance->role() != InstanceRole::Primary ||
            !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) return;
        desktopAction = std::make_unique<Desktop::DesktopCaptureAction>(paths.executable,
            options.portableDirectory, owner);
        QObject::connect(desktopAction.get(), &Desktop::DesktopCaptureAction::availabilityChanged, owner,
            [this](bool available) {
                desktopActionRetry = !available;
                refreshCaptureStopActions(); scheduleCheck();
            });
        QObject::connect(desktopAction.get(), &Desktop::DesktopCaptureAction::verified, owner,
            [this](quint64 token, const Desktop::Failure&) {
                if (token != desktopActionVerification) return;
                desktopActionVerification = 0;
                refreshCaptureStopActions(); scheduleCheck();
            });
        QObject::connect(desktopAction.get(), &Desktop::DesktopCaptureAction::shutdownFinished, owner,
            [this] { desktopActionStopped = true; scheduleCheck(); });
        verifyDesktopCaptureAction();
    }

    bool loadServices()
    {
#ifdef Q_OS_WIN
        constexpr auto resourcePolicy = Localization::ResourcePolicy::WindowsDirectoryFallback;
#else
        constexpr auto resourcePolicy = Localization::ResourcePolicy::LinuxUserResources;
#endif
        catalog = std::make_unique<Localization::LanguageCatalog>(Localization::LanguagePaths{
            resourcePolicy, QCoreApplication::applicationDirPath(), paths.data}, owner);
        const auto english = catalog->load(QStringLiteral("en-US"));
        if (!english.usable) { fatal(QStringLiteral("The embedded application language resources could not be loaded")); return false; }
        store = std::make_unique<Config::SettingsStore>(paths.settings);
        const auto loaded = store->load();
        if (!loaded.ok()) { fatal(text(u"setting.invalidConfiguration") + QStringLiteral("\n") + loaded.detail); return false; }
        const auto selectedLanguage = store->value({QStringLiteral("common"), QStringLiteral("language")}).toString();
        const auto language = catalog->load(selectedLanguage.isEmpty() ? QStringLiteral("en-US") : selectedLanguage);
        if (!language.usable) { fatal(text(u"setting.languageFailed")); return false; }
        if (language.fellBack) report(text(u"setting.languageFallback"));
        qtTranslations = std::make_unique<Localization::QtTranslations>();
        archive = std::make_shared<Media::MediaArchiveService>(Media::MediaArchiveService::defaultDirectory(),
            [](const QString& path, Media::Kind kind, std::stop_token stop) {
                if (kind == Media::Kind::Gif) return Media::inspectGif(path, {}, stop);
                return Media::inspectVideo(path, {}, stop);
            });
        clipboard = std::make_unique<Desktop::SystemClipboard>(owner);
        QObject::connect(clipboard.get(), &Desktop::SystemClipboard::finished, owner,
            [this](quint64, const Desktop::ClipboardResult&) { scheduleCheck(); });
        QObject::connect(clipboard.get(), &Desktop::SystemClipboard::busyChanged, owner, [this] { scheduleCheck(); });
        QObject::connect(clipboard.get(), &Desktop::SystemClipboard::shutdownFinished, owner,
            [this] { clipboardStopped = true; scheduleCheck(); });
        revealer = std::make_unique<Desktop::FileRevealer>(owner);
        QObject::connect(revealer.get(), &Desktop::FileRevealer::finished, owner,
            [this](quint64 token, const Desktop::RevealResult& result) {
                if (directoryRequests.remove(token) && result.failure && result.failure.code != Desktop::Error::Cancelled)
                    report(result.failure.detail);
                scheduleCheck();
            });
        media = std::make_unique<MediaWorkspace>(archive, *clipboard, *revealer, Media::videoThumbnail, owner);
        QObject::connect(catalog.get(), &Localization::LanguageCatalog::changed, owner, [this] { refreshLanguage(); });
        QObject::connect(media.get(), &MediaWorkspace::operationFailed, owner, [this](const QString& message) { report(message); });
        QObject::connect(media.get(), &MediaWorkspace::busyChanged, owner, [this] { scheduleCheck(); });
        if (!media->setEditorSettings(*store)) {
            fatal(QStringLiteral("The editor could not bind the application's loaded settings"));
            return false;
        }
        capture = std::make_unique<CaptureWorkspace>(*media, *clipboard, owner);
        QObject::connect(capture.get(), &CaptureWorkspace::operationFailed, owner, [this](const QString& message) { report(message); });
        QObject::connect(capture.get(), &CaptureWorkspace::busyChanged, owner, [this] { scheduleCheck(); });
        QObject::connect(capture.get(), &CaptureWorkspace::captureFinished, owner, [this] {
            journeyFinished = true;
            if (!initial.oneShot && !trayAvailable && phase == ApplicationPhase::Running) showEntry(false);
            scheduleCheck();
        });
        QObject::connect(capture.get(), &CaptureWorkspace::shutdownFinished, owner, [this] { captureStopped = true; scheduleCheck(); });
        recording = std::make_unique<RecordingWorkspace>(archive, *media, *capture,
            std::move(options.recordingBackend), options.recordingCapabilities,
            std::move(options.liveRecordingBackend), owner);
        QObject::connect(recording.get(), &RecordingWorkspace::busyChanged, owner, [this] {
            refreshCaptureStopActions(); scheduleCheck();
        });
        QObject::connect(recording.get(), &RecordingWorkspace::operationFailed, owner,
            [this](const QString& message) { report(message); });
        QObject::connect(recording.get(), &RecordingWorkspace::shutdownFinished, owner, [this] {
            recordingStopped = true; stopMedia(); scheduleCheck();
        });
        scroll = std::make_unique<ScrollWorkspace>(*media, *capture, owner);
        QObject::connect(scroll.get(), &ScrollWorkspace::busyChanged, owner, [this] {
            refreshCaptureStopActions(); scheduleCheck();
        });
        QObject::connect(scroll.get(), &ScrollWorkspace::operationFailed, owner,
            [this](const QString& message) { report(message); });
        QObject::connect(scroll.get(), &ScrollWorkspace::shutdownFinished, owner, [this] {
            scrollStopped = true; stopMedia(); scheduleCheck();
        });
        activation = std::make_unique<Desktop::WindowActivation>(owner);
        QObject::connect(QGuiApplication::clipboard(), &QClipboard::changed, owner, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard && phase == ApplicationPhase::ClipboardOwner &&
                !QGuiApplication::clipboard()->ownsClipboard()) finishExit(false);
        });
        if (!initial.oneShot) {
            settings = std::make_unique<SettingsController>(*store, *catalog, paths.executable, owner);
            QObject::connect(settings.get(), &SettingsController::captureRequested, owner,
                [this](const QString& token) { startCapture(token); });
            QObject::connect(settings.get(), &SettingsController::openArchiveRequested, owner,
                [this] { openDirectory(archive->directory(), true); });
            QObject::connect(settings.get(), &SettingsController::lanSettingsRequested, owner, [this] { openLan(); });
            QObject::connect(settings.get(), &SettingsController::openLinkRequested, owner, [this](const QUrl& url) {
                if (phase == ApplicationPhase::Running && !QDesktopServices::openUrl(url)) report(text(u"app.linkFailed"));
            });
            QObject::connect(settings.get(), &SettingsController::operationFailed, owner, [this](const QString& message) { report(message); });
            QObject::connect(settings.get(), &SettingsController::shutdownFinished, owner, [this] { settingsStopped = true; scheduleCheck(); });
            QObject::connect(settings.get(), &SettingsController::busyChanged, owner, [this] { scheduleCheck(); });
            QObject::connect(settings.get(), &SettingsController::stateChanged, owner, [this] {
                refreshCaptureStopActions(); scheduleCheck();
            });
            lan = std::make_unique<LanController>(archive, *store, QFileInfo(store->path()).absolutePath(), owner);
            QObject::connect(lan.get(), &LanController::operationFailed, owner, [this](const QString& message) { report(message); });
            QObject::connect(lan.get(), &LanController::availabilityChanged, owner, [this](bool available) { media->setSendAvailable(available); });
            QObject::connect(lan.get(), &LanController::archiveChanged, media.get(), &MediaWorkspace::notifyArchiveChanged);
            QObject::connect(lan.get(), &LanController::revealFileRequested, media.get(), &MediaWorkspace::revealArchivedFile);
            QObject::connect(lan.get(), &LanController::shutdownFinished, owner, [this] { lanStopped = true; scheduleCheck(); });
            QObject::connect(lan.get(), &LanController::busyChanged, owner, [this] { scheduleCheck(); });
            QObject::connect(media.get(), &MediaWorkspace::sendRequested, owner, [this](const QVector<Media::Entry>& entries) {
                if (phase == ApplicationPhase::Running && lan) lan->openTransfers(entries);
            });
            settings->setArchiveDirectory(archive->directory()); settings->setLanAvailable(true); settings->setLinksAvailable(true);
            settings->setApplicationVersion(versionText());
            setupUpdates();
        }
        refreshLanguage();
        return true;
    }
    void ownedStartup(InstanceRole role)
    {
        try {
            if (stopping() || !loadServices()) return;
            setPhase(ApplicationPhase::Running);
            if (!initial.oneShot) {
                settings->initialize(); lan->initialize(); media->setSendAvailable(lan->available());
                setupTray(); setupDesktopCaptureAction(); refreshLanguage();
                scheduleCheck();
            }
            if (!initial.background) {
                if (!startJourney(initial.entry)) {
                    journeyFinished = true; report(text(u"app.captureBusy")); scheduleCheck();
                }
            }
            QTimer::singleShot(0, owner, [target = owner, role] { Q_EMIT target->started(role); });
        } catch (const std::exception& error) {
            fatal(QStringLiteral("The application could not initialize: ") + QString::fromUtf8(error.what()));
        } catch (...) {
            fatal(QStringLiteral("The application could not initialize its services"));
        }
    }

    bool hasVisibleJourney() const
    {
        for (const auto* window : QApplication::topLevelWidgets()) {
            if (!window->isVisible() || window == entry.get()) continue;
            const auto type = window->windowType();
            if (type == Qt::ToolTip || type == Qt::Popup) continue;
            return true;
        }
        return false;
    }
    void scheduleCheck()
    {
        if (checkQueued || phase == ApplicationPhase::Stopped) return;
        checkQueued = true;
        QTimer::singleShot(0, owner, [this] { checkQueued = false; check(); });
    }
    void check()
    {
        if (phase == ApplicationPhase::Running) {
            if (desktopActionRetry) verifyDesktopCaptureAction();
            if (entryAfterCapture && (!recording || !recording->busy()) && (!scroll || !scroll->busy())) {
                entryAfterCapture = false;
                const bool activateEntry = std::exchange(entryAfterCaptureActivation, false);
                if (!initial.oneShot) showEntry(activateEntry);
            }
            if (!initial.oneShot) considerAutomaticUpdate();
            if (initial.oneShot && journeyFinished && capture && !capture->busy() &&
                (!recording || !recording->busy()) && (!scroll || !scroll->busy()) && media && !media->busy() &&
                !clipboard->busy() && !revealer->busy() && errors.empty() && !errorDialog && !hasVisibleJourney())
                owner->requestQuit();
            return;
        }
        if (phase != ApplicationPhase::Draining) return;
        if (!settingsStopped || !lanStopped || !recordingStopped || !scrollStopped || !captureStopped || !updatesStopped || !desktopActionStopped) return;
        if ((media && media->busy()) || (revealer && revealer->busy())) return;
        if (clipboard && !clipboardStopping) {
            clipboardStopping = true; clipboardStopped = false;
            clipboard->shutdown(); return;
        }
        if (!clipboardStopped) return;
        if (!errors.empty() || errorDialog) { showError(); return; }
        finishExit(false);
    }
    void stopMedia()
    {
        if (phase != ApplicationPhase::Draining || !recordingStopped || !scrollStopped || mediaStopping) return;
        mediaStopping = true;
        captureStopped = !capture;
        if (capture) capture->shutdown();
        if (media) media->shutdown();
        const auto requests = directoryRequests;
        for (const auto token : requests) revealer->cancel(token);
        scheduleCheck();
    }
    void beginShutdown(int code)
    {
        if (phase == ApplicationPhase::Stopped) return;
        if (phase == ApplicationPhase::ClipboardOwner) { finishExit(true); return; }
        if (phase == ApplicationPhase::Draining) return;
        if (code) exitCode = code;
        setPhase(ApplicationPhase::Draining);
        if (instance) instance->stopAccepting();
        trayWatch.stop(); if (trayMenu) trayMenu->setEnabled(false);
        updateCheckTimer.stop(); updatePromptTimer.stop();
        if (updatePrompt) updatePrompt->reject();
        if (activation && activationRequest) activation->cancel(activationRequest);
        refreshEntry();
        settingsStopped = !settings; lanStopped = !lan; recordingStopped = !recording; scrollStopped = !scroll; updatesStopped = !updates;
        desktopActionStopped = !desktopAction;
        if (desktopAction) desktopAction->shutdown();
        if (settings) settings->shutdown();
        if (lan) lan->shutdown();
        if (updates) updates->shutdown();
        // Active recording finishes or retains its owned recovery file before
        // capture cancellation can issue any live-action cancel/discard signal.
        if (recording) recording->shutdown();
        if (scroll) scroll->shutdown();
        stopMedia();
        scheduleCheck();
    }
    void finishExit(bool explicitlyReleaseClipboard)
    {
        if (exitEmitted || phase == ApplicationPhase::Stopped) return;
#ifndef Q_OS_WIN
        if (!explicitlyReleaseClipboard && QGuiApplication::clipboard()->ownsClipboard()) {
            // A selection receipt proves current ownership, not retention after
            // this process exits. Keep an actual visible owner until the user
            // chooses Exit or the desktop replaces our selection. No hidden
            // resident process or fictitious manager acknowledgment is used.
            setPhase(ApplicationPhase::ClipboardOwner); showEntry(); return;
        }
#else
        Q_UNUSED(explicitlyReleaseClipboard);
#endif
        exitEmitted = true; setPhase(ApplicationPhase::Stopped);
        if (tray) tray->hide();
        if (entry) entry->hide();
        if (errorDialog) errorDialog->close();
        QTimer::singleShot(0, owner, [target = owner, code = exitCode] { Q_EMIT target->exitReady(code); });
    }
};

QtApplication::QtApplication(QApplication& application, ApplicationOptions options, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, application, std::move(options)))
{
    application.setQuitOnLastWindowClosed(false); application.installEventFilter(this);
    connect(&application, &QGuiApplication::lastWindowClosed, this, [this] { state_->scheduleCheck(); });
}
QtApplication::~QtApplication()
{
    state_->application.removeEventFilter(this);
    if (state_->instance) state_->instance->stopAccepting();
    state_->trayWatch.stop();
    state_->updateCheckTimer.stop(); state_->updatePromptTimer.stop();
    for (auto* service : children()) disconnect(service, nullptr, this, nullptr);
    if (state_->errorDialog) { disconnect(state_->errorDialog, nullptr, this, nullptr); delete state_->errorDialog; }
    if (state_->updatePrompt) { disconnect(state_->updatePrompt, nullptr, this, nullptr); delete state_->updatePrompt; }
    if (state_->tray) { state_->tray->hide(); state_->tray->setContextMenu(nullptr); }
    // Normal exit has already drained the services. Their destructors retain
    // the last-resort resource barriers for abnormal owner destruction.
}
bool QtApplication::start(Core::StartupRequest request)
{
    if (QThread::currentThread() != thread() || state_->phase != ApplicationPhase::Dormant) return false;
    if (request.action == Core::StartupAction::Invalid) return false;
    if (request.action != Core::StartupAction::Launch) { request.oneShot = false; request.background = false; }
    state_->initial = request; state_->setPhase(ApplicationPhase::Starting);
    state_->paths = applicationPaths(state_->options.portableDirectory, request.oneShot);
    if (!state_->paths.failure.isEmpty()) { state_->fatal(state_->paths.failure); return true; }
    state_->instance = std::make_unique<SingleInstance>(state_->paths.runtime, state_->paths.scope,
        [this](const Core::StartupRequest& activation) { return state_->admit(activation); }, this);
    connect(state_->instance.get(), &SingleInstance::startupFinished, this, [this](const InstanceResult& result) {
        if (state_->stopping()) return;
        if (result.role == InstanceRole::Forwarded) {
            QTimer::singleShot(0, this, [this] { Q_EMIT started(InstanceRole::Forwarded); });
            state_->finishExit(false); return;
        }
        if (result.role == InstanceRole::Failed) { state_->fatal(result.detail); return; }
        if (result.role == InstanceRole::Primary || result.role == InstanceRole::Independent) state_->ownedStartup(result.role);
    });
    if (!state_->instance->start(request)) state_->fatal(QStringLiteral("The application startup request was rejected"));
    return true;
}
void QtApplication::requestQuit(int code)
{
    if (QThread::currentThread() != thread()) {
        QTimer::singleShot(0, this, [this, code] { requestQuit(code); }); return;
    }
    state_->beginShutdown(code);
}
ApplicationPhase QtApplication::phase() const noexcept { return state_->phase; }
bool QtApplication::oneShot() const noexcept { return state_->initial.oneShot; }
QString QtApplication::settingsPath() const { return state_->store ? state_->store->path() : state_->paths.settings; }
bool QtApplication::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == &state_->application && event->type() == QEvent::Quit && state_->phase != ApplicationPhase::Stopped) {
        requestQuit(); return true;
    }
    if (event->type() == QEvent::Close || event->type() == QEvent::Hide || event->type() == QEvent::Destroy || event->type() == QEvent::Show)
        state_->scheduleCheck();
    return QObject::eventFilter(watched, event);
}

} // namespace HuhuCapture::Application
