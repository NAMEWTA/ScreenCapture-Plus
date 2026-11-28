#pragma once

#include "core/Startup.h"
#include <QObject>
#include <QString>
#include <memory>

namespace HuhuCapture::Application {

struct DesktopApplicationPaths {
    QString settingsFile;
    QString dataDirectory;
    QString runtimeDirectory;
    QString persistentExecutable;
    QString archiveDirectory;
    QString portableDirectory;
};

// The single production composition root. Settings/media/transfer/recording
// and desktop services belong to the process; closing a view never owns their
// lifetime. Tests may supply isolated real directories, not substitute services.
class DesktopApplication final : public QObject {
    Q_OBJECT
public:
    explicit DesktopApplication(DesktopApplicationPaths paths, QObject* parent = nullptr);
    ~DesktopApplication() override;
    bool start(Core::StartupRequest request);
    bool activate(Core::StartupRequest request);
    bool ready() const noexcept;
    bool shuttingDown() const noexcept;

public Q_SLOTS:
    void showSettings();
    void showHistory();
    void requestExit();

Q_SIGNALS:
    void started();
    void exitReady(int code);
    void operationFailed(const QString& detail);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
