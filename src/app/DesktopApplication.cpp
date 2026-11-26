#include "app/DesktopApplication.h"
#include "app/Brand.h"
#include "app/LanController.h"
#include "app/MediaWorkspace.h"
#include "app/RecordingController.h"
#include "app/ScrollController.h"
#include "app/SettingsController.h"
#include "app/SingleInstance.h"
#include "app/Version.h"
#include "capture/CaptureSession.h"
#include "services/capture/DesktopCapture.h"
#include "services/config/SettingsStore.h"
#include "services/desktop/AutoStart.h"
#include "services/desktop/DesktopCaptureAction.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/GlobalShortcut.h"
#include "services/desktop/LaunchCommand.h"
#include "services/desktop/SystemClipboard.h"
#include "services/localization/LanguageCatalog.h"
#include "services/localization/QtTranslations.h"
#include "services/media/GifInspection.h"
#include "services/media/VideoInspection.h"
#include "services/recognition/QrService.h"
#include "services/update/UpdateService.h"
#include "services/update/Installation.h"
#include "platform/WindowActivation.h"
#include "ui/qt/CapturePlacement.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/DesktopTheme.h"
#include "ui/qt/EditorWindow.h"
#include "ui/qt/HistoryWindow.h"
#include "ui/qt/UpdatePresentation.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTextEdit>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <utility>
#include <vector>

namespace HuhuCapture::Application {
namespace {
namespace UI = HuhuCapture::UI::Qt;
QString versionText() { return QString::fromLatin1(Version::text.data(), Version::text.size()); }

std::optional<Updates::PackageKind> installedPackage()
{
#ifdef Q_OS_WIN
    return Updates::readInstallation(QCoreApplication::applicationDirPath(), versionText(), u"windows", u"x64").packageKind;
#else
    return Updates::readInstallation(QCoreApplication::applicationDirPath(), versionText(), u"linux", u"amd64").packageKind;
#endif
}

struct QrWork {
    Core::QrError error{};
    Recognition::QrResult result;
};

class Launcher final : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void()> closeRequested;
protected:
    void closeEvent(QCloseEvent* event) override
    {
        event->ignore();
        if (closeRequested) closeRequested();
    }
};
}

struct DesktopApplication::State {
    DesktopApplication& owner;
    DesktopApplicationPaths paths;
    // This is destroyed last: the old process retains its primary lock until
    // every recording/transfer worker and desktop owner has actually exited.
    std::unique_ptr<SingleInstance> instance;
    std::unique_ptr<Config::SettingsStore> settings;
    std::unique_ptr<Localization::LanguageCatalog> language;
    std::unique_ptr<Localization::QtTranslations> qtTranslations;
    std::unique_ptr<Desktop::DesktopCaptureAction> desktopAction;
    std::shared_ptr<Media::MediaArchiveService> archive;
    std::unique_ptr<Desktop::SystemClipboard> clipboard;
    std::unique_ptr<Desktop::FileRevealer> revealer;
    std::unique_ptr<MediaWorkspace> media;
    std::unique_ptr<LanController> lan;
    std::unique_ptr<SettingsController> settingsUi;
    std::unique_ptr<Capture::DesktopCapture> capture;
    std::unique_ptr<Capture::CaptureSession> selection;
    std::unique_ptr<UI::CaptureWindow> captureUi;
    std::unique_ptr<RecordingController> recording;
    std::unique_ptr<ScrollController> scroll;
    std::unique_ptr<Updates::UpdateService> updates;
    std::unique_ptr<QMenu> trayMenu;
    std::unique_ptr<QSystemTrayIcon> tray;
    std::unique_ptr<Launcher> launcher;
    QPointer<QDialog> permission;
    QPointer<QMessageBox> errorDialog;
    QPointer<QMessageBox> updatePrompt;
    QPointer<QDialog> clipboardOwnerWindow;
    std::vector<QPointer<QDialog>> dialogs;
    struct TextCopy {
        quint64 actionToken{};
        std::function<void(const Desktop::ClipboardResult&)> completed;
    };
    std::map<quint64, TextCopy> textCopies;
    std::map<quint64, std::stop_source> textPreparations;
    std::set<quint64> directReveals;
    QThreadPool qrPool;
    std::stop_source qrStop;
    QString activationToken;
    quint64 qrToken{}, captureToken{}, captureAttempt{};
    quint64 desktopActionVerification{};
    QTimer stopRouteWatch, updateCheckTimer, updatePromptTimer;
    QString promptedUpdateVersion;
    QStringList shutdownFailures;
    qint64 updateAttemptedDay{};
    std::optional<Core::StartupRequest> pendingCapture;
    Core::StartupRequest initial;
    bool started{}, servicesReady{}, captureInitialized{}, stopping{}, exitReported{};
    bool mediaStopped{}, lanStopped{}, recordingStopped{}, scrollStopped{}, updatesStopped{}, settingsStopped{}, captureStopped{};
    bool clipboardStopped{}, revealerStopped{};
    bool mediaShutdownStarted{}, desktopShutdownStarted{};
    bool desktopActionStopped{true}, retainingClipboard{}, releaseClipboard{};
    bool capturingShortcut{}, qrBusy{}, captureActive{};
    bool configurationWritable{};
    int exitCode{};

    State(DesktopApplication& application, DesktopApplicationPaths locations)
        : owner(application), paths(std::move(locations))
    {
        qrPool.setMaxThreadCount(1); qrPool.setExpiryTimeout(-1);
        stopRouteWatch.setInterval(500);
        updateCheckTimer.setSingleShot(true); updateCheckTimer.setInterval(15000);
        updatePromptTimer.setSingleShot(true); updatePromptTimer.setInterval(5000);
    }
    QString text(QStringView key) const { return language ? language->text(key) : key.toString(); }

    void showError(const QString& detail)
    {
        if (stopping) {
            if (!detail.isEmpty() && !shutdownFailures.contains(detail)) shutdownFailures.push_back(detail);
            exitCode = 1;
            queueIdleCheck();
            Q_EMIT owner.operationFailed(detail);
            return;
        }
        if (errorDialog) {
            if (errorDialog->text() != detail) errorDialog->setText(detail);
            errorDialog->show(); errorDialog->raise();
            Q_EMIT owner.operationFailed(detail);
            return;
        }
        std::erase_if(dialogs, [](const auto& view) { return view.isNull(); });
        auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("huhucapture"),
            detail, QMessageBox::Ok, launcher && launcher->isVisible() ? launcher.get() : nullptr);
        box->setObjectName(QStringLiteral("applicationError"));
        box->setTextFormat(Qt::PlainText); box->setAttribute(Qt::WA_DeleteOnClose);
        errorDialog = box;
        dialogs.push_back(box);
        QObject::connect(box, &QDialog::finished, &owner, [this] { queueIdleCheck(); });
        box->open();
        Q_EMIT owner.operationFailed(detail);
    }
    bool captureBusy() const
    {
        return capturingShortcut || captureActive || captureToken || pendingCapture || qrBusy ||
            (recording && recording->busy()) || (scroll && scroll->busy());
    }
    void refreshShortcutSuspension()
    {
        if (settingsUi && !stopping) settingsUi->setApplicationCaptureActive(captureActive);
        refreshCaptureStopActions();
    }
    void queueIdleCheck()
    {
        QTimer::singleShot(0, &owner, [this] {
            if (stopping) { finishExit(); return; }
            considerAutomaticUpdate();
            if (!initial.oneShot || !servicesReady || captureBusy() || !textPreparations.empty() || !textCopies.empty() ||
                !directReveals.empty() ||
                media->busy() || clipboard->busy() || revealer->busy() ||
                lan->busy() || lan->service().hasActiveTransfers() || (updates && updates->busy())) return;
            for (const auto* widget : QApplication::topLevelWidgets())
                if (widget->isVisible() && widget->windowType() != Qt::ToolTip) return;
            owner.requestExit();
        });
    }
    void finishExit()
    {
        if (stopping && servicesReady && recordingStopped && scrollStopped && !mediaShutdownStarted) {
            mediaShutdownStarted = true;
            media->shutdown();
            return;
        }
        if (stopping && servicesReady && mediaStopped && !desktopShutdownStarted) {
            desktopShutdownStarted = true;
            clipboard->shutdown(); revealer->shutdown();
            return;
        }
        if (!stopping || exitReported || qrBusy || captureToken || !textPreparations.empty() ||
            !mediaStopped || !lanStopped || !recordingStopped || !scrollStopped || !updatesStopped ||
            !settingsStopped || !captureStopped || !clipboardStopped || !revealerStopped || !textCopies.empty() ||
            !directReveals.empty() || !desktopActionStopped ||
            (capture && capture->busy())) return;
        if (errorDialog && errorDialog->isVisible()) return;
        if (!shutdownFailures.isEmpty()) {
            auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("huhucapture"),
                shutdownFailures.join(QStringLiteral("\n\n")), QMessageBox::Ok);
            shutdownFailures.clear();
            box->setObjectName(QStringLiteral("applicationError"));
            box->setTextFormat(Qt::PlainText); box->setAttribute(Qt::WA_DeleteOnClose);
            errorDialog = box; dialogs.push_back(box);
            QObject::connect(box, &QDialog::finished, &owner, [this] {
                errorDialog.clear(); queueIdleCheck();
            });
            box->open();
            return;
        }
#ifndef Q_OS_WIN
        if (!releaseClipboard && QGuiApplication::clipboard()->ownsClipboard()) {
            // X11/Wayland ownership is not a clipboard-manager retention
            // receipt. Keep an explicit visible owner until replacement or
            // an explicit Exit action; never silently discard copied data.
            if (!retainingClipboard) {
                retainingClipboard = true;
                auto* dialog = new QDialog;
                dialog->setObjectName(QStringLiteral("applicationClipboardOwner"));
                dialog->setWindowTitle(QStringLiteral("huhucapture"));
                UI::DesktopTheme::apply(dialog);
                auto* layout = new QVBoxLayout(dialog);
                auto* explanation = UI::DesktopTheme::label(dialog);
                explanation->setWordWrap(true); explanation->setText(text(u"app.clipboardExit"));
                layout->addWidget(explanation);
                auto* buttons = new QDialogButtonBox(dialog);
                auto* keep = buttons->addButton(text(u"app.keepClipboard"), QDialogButtonBox::ActionRole);
                auto* leave = buttons->addButton(text(u"tray.exit"), QDialogButtonBox::RejectRole);
                keep->setObjectName(QStringLiteral("applicationKeepClipboard"));
                leave->setObjectName(QStringLiteral("applicationExit"));
                layout->addWidget(buttons);
                QObject::connect(keep, &QPushButton::clicked, dialog, [this, keep, explanation] {
                    keep->setEnabled(false); explanation->setText(text(u"app.clipboardKeeping"));
                });
                QObject::connect(leave, &QPushButton::clicked, dialog, &QDialog::reject);
                QObject::connect(dialog, &QDialog::rejected, &owner, [this] {
                    releaseClipboard = true; finishExit();
                });
                clipboardOwnerWindow = dialog; dialogs.push_back(dialog);
                dialog->resize(410, 170); dialog->show();
            }
            return;
        }
#endif
        exitReported = true;
        if (clipboardOwnerWindow) clipboardOwnerWindow->hide();
        Q_EMIT owner.exitReady(exitCode);
    }
    void refreshCaptureStopActions()
    {
        const bool journey = (recording && recording->busy()) || (scroll && scroll->busy());
        if (tray) {
            tray->setContextMenu(journey ? nullptr : trayMenu.get());
            tray->setToolTip(journey ? QString() : QStringLiteral("huhucapture"));
        }
        const bool nativeTray = tray && tray->isVisible() && QSystemTrayIcon::isSystemTrayAvailable();
        const bool nativeShortcut = settingsUi && settingsUi->captureShortcutAvailable();
        const bool nativeAction = desktopAction && desktopAction->available();
        const bool available = !stopping && (nativeTray || nativeShortcut || nativeAction);
        const auto hint = !available ? QString() : nativeTray ? text(u"capture.trayFinishHint") :
            nativeShortcut ? text(u"capture.shortcutFinishHint") :
            text(u"capture.desktopFinishHint").arg(desktopAction->actionName());
        if (recording) recording->setExternalStopAvailable(available, hint);
        if (scroll) scroll->setExternalStopAvailable(available, hint);
    }
    void verifyDesktopCaptureAction()
    {
        if (!desktopAction || desktopActionVerification || stopping || desktopAction->available() ||
            (recording && recording->busy()) || (scroll && scroll->busy())) return;
        desktopActionVerification = desktopAction->verify();
    }
    void setupDesktopCaptureAction()
    {
        if (initial.oneShot || !instance || instance->role() != InstanceRole::Primary ||
            !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) return;
        desktopActionStopped = false;
        desktopAction = std::make_unique<Desktop::DesktopCaptureAction>(paths.persistentExecutable, paths.portableDirectory);
        QObject::connect(desktopAction.get(), &Desktop::DesktopCaptureAction::availabilityChanged, &owner,
            [this] { refreshCaptureStopActions(); });
        QObject::connect(desktopAction.get(), &Desktop::DesktopCaptureAction::verified, &owner,
            [this](quint64 token, const Desktop::Failure&) {
                if (token != desktopActionVerification) return;
                desktopActionVerification = 0; refreshCaptureStopActions(); queueIdleCheck();
            });
        QObject::connect(desktopAction.get(), &Desktop::DesktopCaptureAction::shutdownFinished, &owner,
            [this] { desktopActionStopped = true; queueIdleCheck(); });
        verifyDesktopCaptureAction();
    }
    bool updateIdle() const
    {
        if (stopping || initial.oneShot || !servicesReady || captureBusy() || errorDialog ||
            QApplication::activeModalWidget() || QApplication::activePopupWidget() ||
            media->busy() || clipboard->busy() || revealer->busy() || settingsUi->busy() ||
            lan->busy() || lan->service().hasActiveTransfers()) return false;
        for (const auto* window : QApplication::topLevelWidgets())
            if (window->isVisible() && window != launcher.get() && window->windowType() != Qt::ToolTip) return false;
        return true;
    }
    bool updateCheckDue() const
    {
        if (!updates || !configurationWritable || initial.oneShot) return false;
        const auto today = QDateTime::currentSecsSinceEpoch() / 86400;
        return updateAttemptedDay < today &&
            settings->value({QStringLiteral("common"), QStringLiteral("updateCheckDay")}).toInteger() < today;
    }
    void rememberUpdateAttempt()
    {
        updateAttemptedDay = QDateTime::currentSecsSinceEpoch() / 86400;
        const auto saved = settings->setValue({QStringLiteral("common"), QStringLiteral("updateCheckDay")},
            QJsonValue(updateAttemptedDay));
        if (!saved.ok()) showError(text(u"setting.saveFailed") + QChar(u'\n') + saved.detail);
    }
    void considerAutomaticUpdate()
    {
        if (!updateCheckDue() || !updateIdle() || updates->busy()) { updateCheckTimer.stop(); return; }
        if (!updateCheckTimer.isActive()) updateCheckTimer.start();
    }
    void downloadUpdate()
    {
        if (stopping || !updates || updates->busy()) return;
        updateCheckTimer.stop(); updatePromptTimer.stop();
        if (updatePrompt) updatePrompt->reject();
        if (updates->download() && configurationWritable) rememberUpdateAttempt();
    }
    void promptForUpdate()
    {
        if (stopping || !updates || updates->snapshot().stage != Updates::Stage::Available ||
            updatePrompt || promptedUpdateVersion == updates->snapshot().version) return;
        if (!updateIdle()) { updatePromptTimer.start(); return; }
        promptedUpdateVersion = updates->snapshot().version;
        auto* prompt = new QMessageBox(QMessageBox::Information, QStringLiteral("huhucapture"),
            text(u"update.found") + QStringLiteral(" v") + promptedUpdateVersion + QStringLiteral("\n\n") +
                text(u"update.promptPackageDownload"), QMessageBox::Yes | QMessageBox::No);
        prompt->setObjectName(QStringLiteral("applicationUpdateAvailable"));
        prompt->setTextFormat(Qt::PlainText); prompt->setAttribute(Qt::WA_DeleteOnClose);
        prompt->setDefaultButton(QMessageBox::No); prompt->setEscapeButton(QMessageBox::No);
        updatePrompt = prompt; dialogs.push_back(prompt);
        QObject::connect(prompt, &QDialog::finished, &owner, [this](int answer) {
            updatePrompt.clear();
            if (answer == QMessageBox::Yes && !stopping) downloadUpdate();
            queueIdleCheck();
        });
        prompt->open();
    }
    void openDirectory(const QString& path, bool createIfMissing = false)
    {
        if (stopping) return;
        const auto token = revealer->openDirectory(path, nullptr, createIfMissing);
        if (token) directReveals.insert(token);
        else showError(text(u"clipboard.failed"));
    }
    void cancelTextAction(quint64 actionToken)
    {
        if (const auto found = textPreparations.find(actionToken); found != textPreparations.end())
            found->second.request_stop();
        std::vector<quint64> tokens;
        for (const auto& [token, copy] : textCopies)
            if (copy.actionToken == actionToken) tokens.push_back(token);
        for (const auto token : tokens) clipboard->cancel(token);
    }
    void rebuildTray()
    {
        if (!trayMenu) return;
        trayMenu->clear();
        const auto add = [&](QString label, auto action) {
            auto* item = trayMenu->addAction(std::move(label));
            QObject::connect(item, &QAction::triggered, &owner, action);
        };
        add(text(u"app.capture"), [this] { owner.activate({}); });
        add(text(u"clipboard.title"), [this] { owner.showHistory(); });
        add(text(u"lan.title"), [this] { lan->openTransfers(); });
        trayMenu->addSeparator();
        add(text(u"tray.setting"), [this] { owner.showSettings(); });
        add(text(u"tray.exit"), [this] { owner.requestExit(); });
    }
    void showLauncher()
    {
        if (initial.oneShot || stopping) return;
        if (!launcher) {
            launcher = std::make_unique<Launcher>();
            launcher->setObjectName(QStringLiteral("applicationLauncher"));
            launcher->setWindowTitle(QStringLiteral("huhucapture"));
            UI::DesktopTheme::apply(launcher.get());
            launcher->closeRequested = [this] { owner.requestExit(); };
            auto* layout = new QVBoxLayout(launcher.get());
            layout->setContentsMargins(24, 24, 24, 24); layout->setSpacing(12);
            auto* title = UI::DesktopTheme::label(launcher.get(), 22);
            title->setText(QStringLiteral("huhucapture")); layout->addWidget(title);
            const auto add = [&](QStringView key, const char* name, auto action, bool primary = false) {
                auto* button = UI::DesktopTheme::button(launcher.get(), name, primary);
                button->setProperty("languageKey", key.toString()); button->setText(text(key));
                layout->addWidget(button); QObject::connect(button, &QPushButton::clicked, &owner, action);
            };
            add(u"app.capture", "launcherCapture", [this] { owner.activate({}); }, true);
            add(u"clipboard.title", "launcherHistory", [this] { owner.showHistory(); });
            add(u"lan.title", "launcherTransfers", [this] { lan->openTransfers(); });
            add(u"tray.setting", "launcherSettings", [this] { owner.showSettings(); });
            add(u"tray.exit", "launcherExit", [this] { owner.requestExit(); });
            launcher->resize(300, 340);
        }
        launcher->show(); launcher->raise(); launcher->activateWindow();
    }
    void refreshLanguage()
    {
        if (qtTranslations && settingsUi)
            settingsUi->setStandardDialogLanguageAvailable(qtTranslations->load(language->effectiveCode()));
        const auto translate = [this](QStringView key) { return text(key); };
        media->setTranslator(translate); lan->setTranslator(translate);
        captureUi->setTranslator(translate); recording->setTranslator(translate); scroll->setTranslator(translate);
        rebuildTray();
        if (launcher) for (auto* button : launcher->findChildren<QPushButton*>()) {
            const auto key = button->property("languageKey").toString();
            if (!key.isEmpty()) button->setText(text(key));
        }
        refreshCaptureStopActions();
    }
    void initializeServices()
    {
        archive = std::make_shared<Media::MediaArchiveService>(paths.archiveDirectory,
            [](const QString& path, Media::Kind kind, std::stop_token stop) {
                return kind == Media::Kind::Gif ? Media::inspectGif(path, {}, stop) : Media::inspectVideo(path, {}, stop);
            });
        clipboard = std::make_unique<Desktop::SystemClipboard>();
        revealer = std::make_unique<Desktop::FileRevealer>();
        media = std::make_unique<MediaWorkspace>(archive, *clipboard, *revealer,
            [](const QString& path, unsigned edge, const Media::Limits& limits, std::stop_token stop) {
                return Media::videoThumbnail(path, edge, limits, stop);
            });
        if (!media->setEditorSettings(*settings))
            showError(QStringLiteral("The editor preferences could not be attached before opening an editor"));
        lan = std::make_unique<LanController>(archive, *settings, QFileInfo(paths.settingsFile).absolutePath());
        settingsUi = std::make_unique<SettingsController>(*settings, *language, paths.persistentExecutable);
        settingsUi->setArchiveDirectory(paths.archiveDirectory);
        settingsUi->setLinksAvailable(true);
        settingsUi->setApplicationVersion(versionText());
        selection = std::make_unique<Capture::CaptureSession>();
        captureUi = std::make_unique<UI::CaptureWindow>(*selection);
        capture = std::make_unique<Capture::DesktopCapture>();
        recording = std::make_unique<RecordingController>(archive, *media);
        scroll = std::make_unique<ScrollController>(archive, *media);
        const auto kind = initial.oneShot ? std::optional<Updates::PackageKind>{} : installedPackage();
        if (kind) {
            Updates::Options options;
            options.downloadsDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            options.currentVersion = versionText(); options.packageKind = *kind;
            updates = std::make_unique<Updates::UpdateService>(std::move(options));
            QObject::connect(updates.get(), &Updates::UpdateService::changed, &owner, [this](const auto& status) {
                settingsUi->setUpdateSnapshot(UI::aboutUpdateSnapshot(status));
                if (!stopping && status.stage == Updates::Stage::Available && !updatePromptTimer.isActive())
                    updatePromptTimer.start();
                else if (status.stage != Updates::Stage::Available) updatePromptTimer.stop();
                queueIdleCheck();
            });
            QObject::connect(updates.get(), &Updates::UpdateService::shutdownFinished, &owner,
                [this] { updatesStopped = true; queueIdleCheck(); });
            settingsUi->setUpdateSnapshot(UI::aboutUpdateSnapshot(updates->snapshot()));
        } else {
            updatesStopped = true;
            UI::AboutUpdateSnapshot unavailable; unavailable.stage = UI::UpdateStage::Unavailable;
            settingsUi->setUpdateSnapshot(unavailable);
        }
        QObject* services[]{clipboard.get(), revealer.get(), media.get(), lan.get(), settingsUi.get(),
            selection.get(), captureUi.get(), capture.get(), recording.get(), scroll.get()};
        for (auto* service : services) service->setParent(&owner);
        if (updates) updates->setParent(&owner);
        QObject::connect(&updateCheckTimer, &QTimer::timeout, &owner, [this] {
            if (updateCheckDue() && updateIdle() && !updates->busy() && updates->check()) {
                promptedUpdateVersion.clear(); rememberUpdateAttempt();
            }
        });
        QObject::connect(&updatePromptTimer, &QTimer::timeout, &owner, [this] { promptForUpdate(); });
        QObject::connect(&stopRouteWatch, &QTimer::timeout, &owner, [this] {
            refreshCaptureStopActions(); queueIdleCheck();
        });

        QObject::connect(settingsUi.get(), &SettingsController::shortcutCaptureActive, &owner,
            [this](bool active) { capturingShortcut = active; refreshShortcutSuspension(); });
        QObject::connect(settingsUi.get(), &SettingsController::captureRequested, &owner,
            [this](const QString& token) {
                if (capturingShortcut || stopping) return;
                activationToken = token;
                if (!owner.activate({})) activationToken.clear();
            });
        QObject::connect(settingsUi.get(), &SettingsController::shutdownFinished, &owner,
            [this] { settingsStopped = true; queueIdleCheck(); });
        QObject::connect(settingsUi.get(), &SettingsController::stateChanged, &owner,
            [this] { refreshCaptureStopActions(); });
        QObject::connect(settingsUi.get(), &SettingsController::openArchiveRequested, &owner,
            [this] { openDirectory(paths.archiveDirectory, true); });
        QObject::connect(settingsUi.get(), &SettingsController::lanSettingsRequested, &owner,
            [this] { lan->openSettings(); });
        QObject::connect(settingsUi.get(), &SettingsController::openLinkRequested, &owner,
            [this](const QUrl& url) { if (!QDesktopServices::openUrl(url)) showError(text(u"app.openLinkFailed")); });
        QObject::connect(settingsUi.get(), &SettingsController::downloadRequested, &owner,
            [this] { downloadUpdate(); });
        QObject::connect(settingsUi.get(), &SettingsController::openUpdateFolderRequested, &owner,
            [this](const QString& file) {
                if (updates && !file.isEmpty() && file == updates->snapshot().file)
                    openDirectory(QFileInfo(file).absolutePath());
            });
        QObject::connect(settingsUi.get(), &SettingsController::operationFailed, &owner,
            [this](const QString& detail) { showError(detail); });
        settingsUi->setLanAvailable(configurationWritable && !initial.oneShot);

        QObject::connect(media.get(), &MediaWorkspace::sendRequested, &owner,
            [this](const QVector<Media::Entry>& entries) { lan->openTransfers(entries); });
        QObject::connect(lan.get(), &LanController::availabilityChanged, &owner,
            [this](bool available) { media->setSendAvailable(available); });
        QObject::connect(lan.get(), &LanController::archiveChanged, media.get(), &MediaWorkspace::notifyArchiveChanged);
        QObject::connect(lan.get(), &LanController::revealFileRequested, media.get(), &MediaWorkspace::revealArchivedFile);
        for (auto* service : {media.get()}) {
            QObject::connect(service, &MediaWorkspace::operationFailed, &owner, [this](const QString& detail) { showError(detail); });
            QObject::connect(service, &MediaWorkspace::busyChanged, &owner, [this] { queueIdleCheck(); });
        }
        QObject::connect(lan.get(), &LanController::operationFailed, &owner, [this](const QString& detail) { showError(detail); });
        QObject::connect(lan.get(), &LanController::busyChanged, &owner, [this] { queueIdleCheck(); });
        QObject::connect(&lan->service(), &Transfer::TransferService::eventReady, &owner,
            [this] { queueIdleCheck(); });
        QObject::connect(media.get(), &MediaWorkspace::editorOpened, &owner, [this](UI::EditorWindow* view) {
            QObject::connect(view, &QObject::destroyed, &owner, [this] { queueIdleCheck(); });
        });
        QObject::connect(media.get(), &MediaWorkspace::shutdownFinished, &owner, [this] { mediaStopped = true; queueIdleCheck(); });
        QObject::connect(lan.get(), &LanController::shutdownFinished, &owner, [this] { lanStopped = true; queueIdleCheck(); });
        QObject::connect(recording.get(), &RecordingController::shutdownFinished, &owner, [this] { recordingStopped = true; queueIdleCheck(); });
        QObject::connect(scroll.get(), &ScrollController::shutdownFinished, &owner, [this] { scrollStopped = true; queueIdleCheck(); });
        QObject::connect(recording.get(), &RecordingController::archiveChanged, media.get(), &MediaWorkspace::notifyArchiveChanged);
        QObject::connect(scroll.get(), &ScrollController::archiveChanged, media.get(), &MediaWorkspace::notifyArchiveChanged);
        QObject::connect(recording.get(), &RecordingController::operationFailed, &owner, [this](const QString& detail) { showError(detail); });
        QObject::connect(scroll.get(), &ScrollController::operationFailed, &owner, [this](const QString& detail) { showError(detail); });
        QObject::connect(recording.get(), &RecordingController::busyChanged, &owner,
            [this] { refreshCaptureStopActions(); restoreLauncher(); queueIdleCheck(); });
        QObject::connect(scroll.get(), &ScrollController::busyChanged, &owner,
            [this] { refreshCaptureStopActions(); restoreLauncher(); queueIdleCheck(); });
        QObject::connect(language.get(), &Localization::LanguageCatalog::changed, &owner, [this] { refreshLanguage(); });
        QObject::connect(qApp, &QApplication::lastWindowClosed, &owner, [this] { queueIdleCheck(); });
        QObject::connect(QGuiApplication::clipboard(), &QClipboard::changed, &owner, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard && retainingClipboard &&
                !QGuiApplication::clipboard()->ownsClipboard()) finishExit();
        });
        QObject::connect(clipboard.get(), &Desktop::SystemClipboard::finished, &owner,
            [this](quint64 token, const Desktop::ClipboardResult& result) {
                const auto found = textCopies.find(token);
                if (found == textCopies.end()) return;
                auto done = std::move(found->second.completed); textCopies.erase(found);
                if (!stopping) done(result);
                queueIdleCheck();
            });
        QObject::connect(clipboard.get(), &Desktop::SystemClipboard::shutdownFinished, &owner,
            [this] { clipboardStopped = true; queueIdleCheck(); });
        QObject::connect(clipboard.get(), &Desktop::SystemClipboard::busyChanged, &owner,
            [this] { queueIdleCheck(); });
        QObject::connect(revealer.get(), &Desktop::FileRevealer::shutdownFinished, &owner,
            [this] { revealerStopped = true; queueIdleCheck(); });
        QObject::connect(revealer.get(), &Desktop::FileRevealer::finished, &owner,
            [this](quint64 token, const Desktop::RevealResult& result) {
                if (directReveals.erase(token) && !stopping && !result.completed() &&
                    result.failure.code != Desktop::Error::Cancelled)
                    showError(result.failure.detail.isEmpty() ? text(u"clipboard.failed") : result.failure.detail);
                queueIdleCheck();
            });
        QObject::connect(capture.get(), &Capture::DesktopCapture::initialized, &owner, [this](const Capture::Capability&) {
            captureInitialized = true;
            if (pendingCapture) {
                const auto request = *pendingCapture; pendingCapture.reset();
                if (!stopping) beginCapture(request);
            }
        });
        QObject::connect(capture.get(), &Capture::DesktopCapture::busyChanged, &owner, [this] { queueIdleCheck(); });
        QObject::connect(capture.get(), &Capture::DesktopCapture::shutdownFinished, &owner,
            [this] { captureStopped = true; queueIdleCheck(); });
        QObject::connect(capture.get(), &Capture::DesktopCapture::finished, &owner,
            [this](quint64 token, const Capture::Snapshot& snapshot, const Capture::Failure& error) {
                if (token != captureToken) return;
                captureToken = 0;
                if (permission) { delete permission; permission.clear(); }
                if (stopping) { finishExit(); return; }
                if (error) {
                    captureActive = false; refreshShortcutSuspension();
                    if (error.code != Capture::Failure::Code::Cancelled) showError(error.detail);
                    restoreLauncher(); queueIdleCheck(); return;
                }
                Capture::StartOptions options;
                switch (currentEntry) {
                case Core::CaptureEntry::Pin: options.enter = Capture::Action::Pin; break;
                case Core::CaptureEntry::Scroll: options.enter = Capture::Action::Scroll; break;
                case Core::CaptureEntry::Recording: options.enter = Capture::Action::Record; break;
                case Core::CaptureEntry::Qr: options.enter = Capture::Action::Qr; break;
                default: break;
                }
                for (int action = 0; action < static_cast<int>(Capture::Action::Count); ++action)
                    selection->setCapability(static_cast<Capture::Action>(action), {Capture::CapabilityState::Available, {}});
                if (const auto failure = captureUi->showSnapshot(snapshot, options)) {
                    captureActive = false; refreshShortcutSuspension(); showError(failure.detail); restoreLauncher();
                }
            });
        QObject::connect(selection.get(), &Capture::CaptureSession::actionRequested, &owner,
            [this](const Capture::ActionRequest& request) { captureAction(request); });
        QObject::connect(selection.get(), &Capture::CaptureSession::actionCancelled, &owner, [this](quint64 token) {
            media->cancelOutput(selection.get(), token);
            cancelTextAction(token);
            if (qrToken == token) qrStop.request_stop();
        });
        QObject::connect(captureUi.get(), &UI::CaptureWindow::closed, &owner, [this] {
            captureActive = false; refreshShortcutSuspension(); restoreLauncher(); queueIdleCheck();
        });
        QObject::connect(captureUi.get(), &UI::CaptureWindow::sourceInvalidated, &owner,
            [this](const Capture::Failure& failure) { showError(failure.detail); });

        servicesReady = true;
        refreshLanguage();
        if (configurationWritable && !initial.oneShot) lan->initialize();
        if (!initial.oneShot) {
            trayMenu = std::make_unique<QMenu>(); rebuildTray();
            tray = std::make_unique<QSystemTrayIcon>(QApplication::windowIcon());
            tray->setToolTip(QStringLiteral("huhucapture")); tray->setContextMenu(trayMenu.get());
            QObject::connect(tray.get(), &QSystemTrayIcon::activated, &owner, [this](QSystemTrayIcon::ActivationReason reason) {
                if ((recording && recording->busy()) || (scroll && scroll->busy())) {
                    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick ||
                        reason == QSystemTrayIcon::MiddleClick || reason == QSystemTrayIcon::Context)
                        owner.activate(Core::StartupRequest{.action = Core::StartupAction::FinishCapture});
                    return;
                }
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) owner.activate({});
            });
            if (QSystemTrayIcon::isSystemTrayAvailable()) tray->show(); else showLauncher();
        }
        settingsUi->initialize(!initial.oneShot);
        setupDesktopCaptureAction();
        refreshCaptureStopActions(); stopRouteWatch.start();
        capture->initialize();
        queueIdleCheck();
    }
    Core::CaptureEntry currentEntry{Core::CaptureEntry::Capture};
    void restoreLauncher()
    {
        if (!stopping && !initial.oneShot && !captureBusy() && !QSystemTrayIcon::isSystemTrayAvailable())
            showLauncher();
    }
    void beginCapture(Core::StartupRequest request)
    {
        if (stopping) return;
        if (!captureInitialized) { pendingCapture = request; return; }
        verifyDesktopCaptureAction();
        const auto capability = capture->capability();
        if (!capability.canRequest()) { showError(capability.reason); queueIdleCheck(); return; }
        currentEntry = request.entry; captureActive = true; refreshShortcutSuspension();
        const auto attempt = ++captureAttempt;
        if (launcher) launcher->hide();
        if (capability.state == Capture::CapabilityState::RequiresPermission) {
            auto* parent = new QDialog;
            parent->setObjectName(QStringLiteral("capturePermissionWindow")); parent->setWindowTitle(text(u"app.capture"));
            UI::DesktopTheme::apply(parent);
            auto* layout = new QVBoxLayout(parent);
            auto* label = UI::DesktopTheme::label(parent); label->setWordWrap(true); label->setText(text(u"app.capturePermission")); layout->addWidget(label);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, parent); layout->addWidget(buttons);
            QObject::connect(buttons, &QDialogButtonBox::rejected, parent, &QDialog::reject);
            QObject::connect(parent, &QDialog::rejected, &owner, [this] {
                if (captureToken) capture->cancel(captureToken);
                else {
                    if (permission) { permission->deleteLater(); permission.clear(); }
                    captureActive = false; refreshShortcutSuspension(); restoreLauncher(); queueIdleCheck();
                }
            });
            permission = parent; parent->resize(360, 140); parent->show();
        }
        QTimer::singleShot(0, &owner, [this, attempt] {
            if (stopping || !captureActive || captureAttempt != attempt) return;
            if (permission && !activationToken.isEmpty()) {
                const auto attached = Platform::applyActivationToken(permission->windowHandle(), activationToken);
                if (attached == Platform::ActivationTokenResult::Attached) permission->windowHandle()->requestActivate();
            }
            activationToken.clear();
            captureToken = capture->request(permission ? permission->windowHandle() : nullptr);
            if (!captureToken) {
                if (permission) { delete permission; permission.clear(); }
                captureActive = false; refreshShortcutSuspension(); restoreLauncher();
                showError(text(u"app.captureUnavailable")); queueIdleCheck();
            }
        });
    }
    void publishText(Desktop::ClipboardPreparation prepared, QWidget* parent, quint64 actionToken,
        std::function<void(const Desktop::ClipboardResult&)> done)
    {
        if (prepared.failure) { done({prepared.failure, {}}); return; }
        const auto token = clipboard->publish(std::move(prepared.content), parent ? parent->windowHandle() : nullptr);
        if (!token) { done({{Desktop::Error::Unavailable, 0, QStringLiteral("Clipboard admission failed")}, {}}); return; }
        textCopies.emplace(token, TextCopy{actionToken, std::move(done)});
    }
    void publishText(const QString& text, QWidget* parent, quint64 actionToken,
        std::function<void(const Desktop::ClipboardResult&)> done)
    {
        const auto [pending, admitted] = textPreparations.try_emplace(actionToken);
        if (!admitted) { done({{Desktop::Error::Unavailable, 0, QStringLiteral("Text preparation is already active")}, {}}); return; }
        const auto stop = pending->second.get_token();
        auto* watcher = new QFutureWatcher<Desktop::ClipboardPreparation>(&owner);
        const QPointer<QWidget> origin(parent);
        QObject::connect(watcher, &QFutureWatcher<Desktop::ClipboardPreparation>::finished, &owner,
            [this, watcher, origin, actionToken, stop, done = std::move(done)]() mutable {
                auto prepared = watcher->future().takeResult(); watcher->deleteLater(); textPreparations.erase(actionToken);
                if (stopping) { finishExit(); return; }
                if (!origin || stop.stop_requested() || selection->activeRequest() != actionToken)
                    done({{Desktop::Error::Cancelled, 0, {}}, {}});
                else publishText(std::move(prepared), origin, actionToken, std::move(done));
                queueIdleCheck();
            });
        watcher->setFuture(QtConcurrent::run(&qrPool, [text, stop] {
            return Desktop::prepareText(text, {}, stop);
        }));
    }
    void qr(const Capture::ActionRequest& request)
    {
        captureUi->hideForAction(); qrBusy = true; qrToken = request.token; qrStop = {};
        auto* watcher = new QFutureWatcher<QrWork>(&owner);
        QObject::connect(watcher, &QFutureWatcher<QrWork>::finished, &owner, [this, watcher, token = request.token] {
            auto result = watcher->future().takeResult(); watcher->deleteLater(); qrBusy = false;
            if (stopping) { finishExit(); return; }
            if (qrStop.stop_requested() || selection->activeRequest() != token) { queueIdleCheck(); return; }
            if (result.error != Core::QrError::None) {
                selection->complete(token, result.error == Core::QrError::Cancelled ? Capture::ActionResult::Cancelled :
                    Capture::ActionResult::Failed, text(u"app.qrFailed"));
                queueIdleCheck(); return;
            }
            auto* dialog = new QDialog;
            dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setObjectName(QStringLiteral("qrResultWindow"));
            dialog->setWindowTitle(text(u"about.sysTip")); UI::DesktopTheme::apply(dialog);
            auto* layout = new QVBoxLayout(dialog);
            auto* output = new QTextEdit(dialog); output->setReadOnly(true); output->setAcceptRichText(false);
            output->setPlainText(result.result.text.isEmpty() ? text(u"cap.qrcodeEmpty") : result.result.text);
            layout->addWidget(output);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
            auto* copy = buttons->addButton(text(u"tool.copy"), QDialogButtonBox::ActionRole);
            copy->setObjectName(QStringLiteral("qrCopy")); copy->setEnabled(!result.result.text.isEmpty());
            layout->addWidget(buttons);
            std::erase_if(dialogs, [](const auto& view) { return view.isNull(); }); dialogs.push_back(dialog);
            QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
            QObject::connect(dialog, &QDialog::finished, &owner, [this, token] {
                cancelTextAction(token);
                selection->complete(token, Capture::ActionResult::Completed); queueIdleCheck();
            });
            QObject::connect(copy, &QPushButton::clicked, &owner,
                [this, view = QPointer(dialog), copy, token, text = std::move(result.result.text)] {
                    if (!view || stopping) return;
                    copy->setEnabled(false);
                    publishText(text, view, token, [this, view, copy](const auto& completion) {
                        if (!view) return;
                        if (completion.completed()) view->accept();
                        else {
                            copy->setEnabled(true);
                            if (completion.failure.code != Desktop::Error::Cancelled) showError(completion.failure.detail);
                        }
                    });
                });
            dialog->resize(520, 340); dialog->open();
        });
        const auto stop = qrStop.get_token();
        watcher->setFuture(QtConcurrent::run(&qrPool, [image = request.image, stop] {
            QrWork work;
            try {
                work.error = Recognition::recognizeImage(image, work.result, {}, stop);
            } catch (const std::bad_alloc&) { work.error = Core::QrError::Allocation; }
            catch (...) { work.error = Core::QrError::Decoder; }
            return work;
        }));
    }
    void captureAction(const Capture::ActionRequest& request)
    {
        if (stopping) return;
        const auto token = request.token;
        const auto complete = [this, token](UI::OutputResult status, const QString& detail = {}) {
            selection->complete(token, status == UI::OutputResult::Succeeded ? Capture::ActionResult::Completed :
                status == UI::OutputResult::Cancelled ? Capture::ActionResult::Cancelled : Capture::ActionResult::Failed, detail);
            queueIdleCheck();
        };
        switch (request.action) {
        case Capture::Action::Edit: case Capture::Action::Pin: {
            captureUi->hideForAction();
            auto* editor = media->openEditor(request.image, request.action == Capture::Action::Pin);
            if (editor) UI::placeCapturedEditor(editor, request);
            complete(editor ? UI::OutputResult::Succeeded : UI::OutputResult::Failed); break;
        }
        case Capture::Action::Scroll: case Capture::Action::Record: {
            captureUi->hideForAction();
            const bool accepted = request.action == Capture::Action::Record
                ? recording->begin(request, captureUi->dialogParent()) : scroll->begin(request, captureUi->dialogParent());
            complete(accepted ? UI::OutputResult::Succeeded : UI::OutputResult::Failed); break;
        }
        case Capture::Action::Qr: qr(request); break;
        case Capture::Action::History:
            captureUi->hideForAction(); owner.showHistory(); complete(UI::OutputResult::Succeeded); break;
        case Capture::Action::SaveAs: case Capture::Action::Clipboard:
            if (!media->outputImage(selection.get(), captureUi->dialogParent(), token,
                request.action == Capture::Action::SaveAs ? UI::OutputIntent::SaveAs : UI::OutputIntent::Clipboard,
                request.image, complete)) complete(UI::OutputResult::Failed, text(u"app.outputUnavailable"));
            break;
        case Capture::Action::Hex: case Capture::Action::Rgb: case Capture::Action::Cmyk: case Capture::Action::Position:
            publishText(request.text, captureUi->dialogParent(), token, [complete](const auto& result) {
                complete(result.completed() ? UI::OutputResult::Succeeded :
                    result.failure.code == Desktop::Error::Cancelled ? UI::OutputResult::Cancelled : UI::OutputResult::Failed,
                    result.failure.detail);
            }); break;
        default: complete(UI::OutputResult::Failed, text(u"app.outputUnavailable")); break;
        }
    }
};

DesktopApplication::DesktopApplication(DesktopApplicationPaths paths, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(*this, std::move(paths))) {}
DesktopApplication::~DesktopApplication()
{
    state_->qrStop.request_stop(); state_->qrPool.waitForDone();
    if (state_->permission) delete state_->permission;
    for (const auto& dialog : state_->dialogs) if (dialog) delete dialog;
}
bool DesktopApplication::ready() const noexcept { return state_->servicesReady; }
bool DesktopApplication::shuttingDown() const noexcept { return state_->stopping; }
bool DesktopApplication::start(Core::StartupRequest request)
{
    if (state_->started || request.action == Core::StartupAction::Invalid) return false;
    state_->started = true; state_->initial = request;
    Config::SettingsResult loaded;
    if (request.action == Core::StartupAction::Launch) {
        state_->settings = std::make_unique<Config::SettingsStore>(state_->paths.settingsFile);
        loaded = state_->settings->load(); state_->configurationWritable = loaded.ok();
#ifdef Q_OS_WIN
        constexpr auto resourcePolicy = Localization::ResourcePolicy::WindowsDirectoryFallback;
#else
        constexpr auto resourcePolicy = Localization::ResourcePolicy::LinuxUserResources;
#endif
        state_->language = std::make_unique<Localization::LanguageCatalog>(Localization::LanguagePaths{
            resourcePolicy, QCoreApplication::applicationDirPath(), state_->paths.dataDirectory});
        state_->language->setParent(this);
        const auto language = state_->language->load(state_->settings->value(
            {QStringLiteral("common"), QStringLiteral("language")}, QStringLiteral("zh-CN")).toString());
        if (!language.usable) return false;
        state_->qtTranslations = std::make_unique<Localization::QtTranslations>();
    }
    // A finish/verification child reads only its scope. It creates no media,
    // settings service, shortcut, tray or error window over an active capture.
    const QFileInfo settingsFile(state_->paths.settingsFile);
    const auto canonicalSettings = settingsFile.canonicalFilePath();
    auto scope = (canonicalSettings.isEmpty() ? settingsFile.absoluteFilePath() : canonicalSettings) + QChar(u'|') +
        qEnvironmentVariable("XDG_SESSION_ID") + QChar(u'|') + qEnvironmentVariable("WAYLAND_DISPLAY") +
        QChar(u'|') + qEnvironmentVariable("DISPLAY");
    state_->instance = std::make_unique<SingleInstance>(state_->paths.runtimeDirectory, scope,
        [this](const Core::StartupRequest& activation) { return activate(activation); });
    state_->instance->setParent(this);
    connect(state_->instance.get(), &SingleInstance::startupFinished, this, [this, loaded](const InstanceResult& result) {
        if (state_->stopping || state_->exitReported) return;
        if (result.role == InstanceRole::Forwarded) { state_->exitReported = true; Q_EMIT exitReady(0); return; }
        if (state_->initial.action != Core::StartupAction::Launch) {
            qWarning().noquote() << result.detail;
            state_->exitReported = true; Q_EMIT exitReady(2); return;
        }
        if (result.role == InstanceRole::Failed) {
            auto* box = new QMessageBox(QMessageBox::Critical, QStringLiteral("huhucapture"), result.detail, QMessageBox::Ok);
            box->setTextFormat(Qt::PlainText); box->setAttribute(Qt::WA_DeleteOnClose);
            state_->dialogs.push_back(box);
            connect(box, &QDialog::finished, this, [this] {
                if (state_->stopping || std::exchange(state_->exitReported, true)) return;
                Q_EMIT exitReady(2);
            });
            box->open(); return;
        }
        state_->initializeServices();
        if (!loaded.ok()) state_->showError(loaded.detail + QChar(u'\n') + state_->paths.settingsFile);
        if (!state_->initial.background) state_->beginCapture(state_->initial);
        Q_EMIT started();
    });
    return state_->instance->start(request);
}
bool DesktopApplication::activate(Core::StartupRequest request)
{
    if (!state_->servicesReady || state_->stopping) return false;
    switch (request.action) {
    case Core::StartupAction::VerifyCaptureAction:
        return state_->desktopAction && state_->desktopAction->acceptVerification(request);
    case Core::StartupAction::FinishCapture:
        if (state_->recording->finish(Recording::StopIntent::Save)) return true;
        return state_->scroll->finish();
    case Core::StartupAction::Invalid: return false;
    case Core::StartupAction::Launch: break;
    }
    if (request.background) return true;
    if (state_->capturingShortcut) return false;
    if (state_->recording->busy()) {
        if (state_->recording->finish(Recording::StopIntent::Save)) { state_->activationToken.clear(); return true; }
        state_->recording->activate(std::exchange(state_->activationToken, {})); return true;
    }
    if (state_->scroll->busy()) {
        if (state_->scroll->finish()) { state_->activationToken.clear(); return true; }
        state_->scroll->activate(std::exchange(state_->activationToken, {})); return true;
    }
    if (state_->captureBusy()) return false;
    state_->beginCapture(request); return true;
}
void DesktopApplication::showSettings()
{
    if (!state_->servicesReady || state_->stopping) return;
    state_->settingsUi->openSettings();
}
void DesktopApplication::showHistory()
{
    if (!state_->servicesReady || state_->stopping) return;
    state_->media->openHistory();
}
void DesktopApplication::requestExit()
{
    if (state_->retainingClipboard) {
        state_->releaseClipboard = true; state_->finishExit(); return;
    }
    if (std::exchange(state_->stopping, true)) return;
    state_->stopRouteWatch.stop(); state_->updateCheckTimer.stop(); state_->updatePromptTimer.stop();
    if (state_->instance) state_->instance->stopAccepting();
    state_->pendingCapture.reset(); state_->qrStop.request_stop();
    for (auto& [token, stop] : state_->textPreparations) { Q_UNUSED(token); stop.request_stop(); }
    if (!state_->servicesReady) {
        for (const auto& dialog : state_->dialogs) if (dialog) dialog->reject();
        if (!std::exchange(state_->exitReported, true)) Q_EMIT exitReady(0);
        return;
    }
    if (state_->tray) state_->tray->hide();
    if (state_->launcher) state_->launcher->hide();
    state_->settingsUi->shutdown();
    if (state_->desktopAction) state_->desktopAction->shutdown();
    state_->capture->shutdown(); state_->captureUi->shutdown();
    if (state_->permission) { state_->permission->deleteLater(); state_->permission.clear(); }
    // Shutdown callbacks may be synchronous. Each barrier is marked only by
    // the actual owner's terminal signal; no view disappearance implies done.
    state_->lan->shutdown(); state_->recording->shutdown(); state_->scroll->shutdown();
    if (state_->updates) state_->updates->shutdown();
    QList<quint64> clipboardTokens;
    for (const auto& [token, callback] : state_->textCopies) { Q_UNUSED(callback); clipboardTokens.push_back(token); }
    for (const auto token : clipboardTokens) state_->clipboard->cancel(token);
    // Recording/scroll own their final media result. Once both have drained,
    // finishExit stops media, then the actual clipboard/file-manager owners.
    for (const auto& dialog : state_->dialogs) if (dialog) dialog->reject();
    QTimer::singleShot(0, this, [this] { state_->finishExit(); });
}

} // namespace HuhuCapture::Application
