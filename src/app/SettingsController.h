#pragma once

#include "services/config/SettingsStore.h"
#include "services/localization/LanguageCatalog.h"
#include "ui/qt/SettingsWindow.h"

#include <QObject>
#include <memory>

namespace HuhuCapture::Application {

// The application's single settings/runtime assembler. Store and catalog are
// shared with every other service. It owns native shortcut/startup adapters,
// never an archive, clipboard, update service or a second application runtime.
class SettingsController final : public QObject {
    Q_OBJECT
public:
    SettingsController(Config::SettingsStore& store, Localization::LanguageCatalog& catalog,
                       QString persistentLaunchExecutable, QObject* parent = nullptr);
    ~SettingsController() override;
    // Call after SettingsStore::load succeeds. It only reads OS autostart.
    // Windows/X11 request the saved shortcut; Wayland restores a previously
    // configured Portal session and reports whatever authorization really does.
    bool initialize();
    UI::Qt::SettingsWindow* openSettings(UI::Qt::SettingsWindow::Page page = UI::Qt::SettingsWindow::Page::General);
    UI::Qt::SettingsSnapshot snapshot() const;
    void setArchiveDirectory(const QString& path, bool canOpen = true);
    void setLanAvailable(bool available);
    void setLinksAvailable(bool available);
    void setStandardDialogLanguageAvailable(bool available);
    void setApplicationVersion(const QString& version);
    void setUpdateSnapshot(const UI::Qt::AboutUpdateSnapshot& update);
    // Actual desktop registration, excluding a temporarily suspended key
    // editor. A stored shortcut string alone is not an available stop action.
    bool captureShortcutAvailable() const;
    bool busy() const;
    void shutdown();

Q_SIGNALS:
    void captureRequested(const QString& activationToken);
    void openArchiveRequested();
    void lanSettingsRequested();
    void openLinkRequested(const QUrl& url);
    void downloadRequested();
    void openUpdateFolderRequested(const QString& path);
    void operationFailed(const QString& localizedMessage);
    void stateChanged();
    void busyChanged(bool busy);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
    void refreshViews();
    void changeRequested(UI::Qt::SettingsWindow* source, quint64 token, const UI::Qt::SettingChange& change);
    void releaseView(UI::Qt::SettingsWindow* view);
    void finishShutdown();
};

} // namespace HuhuCapture::Application
