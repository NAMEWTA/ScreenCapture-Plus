#pragma once

#include "services/config/SettingsStore.h"
#include "services/media/MediaArchiveService.h"
#include "services/transfer/TransferService.h"
#include "ui/qt/TransferSettingsPanel.h"

#include <QObject>
#include <memory>

namespace HuhuCapture::UI::Qt { class TransferWindow; }

namespace HuhuCapture::Application {

// QApplication owns this assembler and the shared archive/configuration. It
// creates exactly one TransferService; windows never own its runtime or jobs.
class LanController final : public QObject {
    Q_OBJECT
public:
    LanController(std::shared_ptr<Media::MediaArchiveService> archive,
        Config::SettingsStore& settings, QString privateConfigDirectory,
        QObject* parent = nullptr);
    ~LanController() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    // Call after the shared SettingsStore has loaded successfully. An absent
    // LAN preference defaults to disabled; no identity/file/worker/listener is
    // created until the user has explicitly enabled LAN (now or previously).
    bool initialize();
    UI::Qt::TransferWindow* openTransfers(const QVector<Media::Entry>& exactEntries = {});
    UI::Qt::TransferSettingsPanel* openSettings();
    UI::Qt::LanSettingsSnapshot settingsSnapshot() const;
    Transfer::TransferService& service() noexcept;
    bool available() const;
    bool busy() const;
    // Asynchronous shutdown cancels preparation and joins the real service.
    // shutdownFinished follows both worker completion and Disabled. The final
    // destructor is a checked join barrier and never waits for GUI callbacks.
    void shutdown();

Q_SIGNALS:
    void availabilityChanged(bool available);
    void busyChanged(bool busy);
    void archiveChanged();
    void revealFileRequested(const QString& path);
    void operationFailed(const QString& message);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
