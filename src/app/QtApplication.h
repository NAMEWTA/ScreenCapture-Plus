#pragma once

#include "app/SingleInstance.h"
#include "recording/RecordingBackend.h"
#include "ui/qt/RecordingWindow.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class QApplication;

namespace HuhuCapture::Application {

enum class ApplicationPhase { Dormant, Starting, Running, Draining, ClipboardOwner, Stopped };

struct ApplicationArguments {
    Core::StartupRequest startup;
    QString portableDirectory;
    QString failure;
};
// Arguments exclude argv[0]. Uses the inherited Core startup parser; the one
// new explicit profile argument is shared by the real entry point and tests.
ApplicationArguments parseApplicationArguments(const QStringList& arguments);

struct ApplicationOptions {
    // An explicit persistent profile; never an AppImage mount. Without this
    // override Windows honors an existing config.json beside the executable.
    QString portableDirectory;
    Recording::Backend recordingBackend;
    Recording::LiveBackendFactory liveRecordingBackend;
    UI::Qt::RecordingCapabilities recordingCapabilities;
};

// The sole Qt production assembler. It owns the archive, desktop services and
// controllers for this process; no view owns a worker/runtime. start() resolves
// the real profile/session and uses SingleInstance before opening services.
class QtApplication final : public QObject {
    Q_OBJECT
public:
    explicit QtApplication(QApplication& application, ApplicationOptions options = {},
        QObject* parent = nullptr);
    ~QtApplication() override;
    bool start(Core::StartupRequest request);
    void requestQuit(int exitCode = 0);
    ApplicationPhase phase() const noexcept;
    bool oneShot() const noexcept;
    QString settingsPath() const;

Q_SIGNALS:
    void started(HuhuCapture::Application::InstanceRole role);
    void phaseChanged(HuhuCapture::Application::ApplicationPhase phase);
    void operationFailed(const QString& message);
    // Emitted only after real service callbacks/workers have drained (or a
    // forwarded process created none). The entry point then exits the event loop.
    void exitReady(int exitCode);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application

Q_DECLARE_METATYPE(HuhuCapture::Application::ApplicationPhase)
