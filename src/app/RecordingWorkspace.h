#pragma once

#include "recording/RecordingBackend.h"
#include "services/media/MediaArchiveService.h"
#include "ui/qt/RecordingWindow.h"

#include <QObject>
#include <memory>

namespace HuhuCapture::Application {
class CaptureWorkspace;
class MediaWorkspace;

// One shared recording journey owns the toolbar and session. Native providers
// only supply the worker backend and their actual capabilities. The host must
// stop this workspace before cancelling capture or stopping the output service.
class RecordingWorkspace final : public QObject {
    Q_OBJECT
public:
    RecordingWorkspace(std::shared_ptr<Media::MediaArchiveService> archive,
        MediaWorkspace& media, CaptureWorkspace& capture, Recording::Backend backend,
        UI::Qt::RecordingCapabilities capabilities,
        Recording::LiveBackendFactory liveBackend = {}, QObject* parent = nullptr);
    ~RecordingWorkspace() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    bool busy() const noexcept;
    // The host publishes this only while an actual tray/shortcut/desktop action is
    // reachable. Losing it requests safe stop before hidden controls return.
    void setExternalStopAvailable(bool available, const QString& hint = {});
    bool canStop() const noexcept;
    bool stopActive(Recording::StopIntent intent);
    bool controlsHiddenForCapture() const noexcept;
    void shutdown();

Q_SIGNALS:
    void busyChanged(bool busy);
    // The visible capture/recording journey ended. Media success is represented
    // by the recording session and actual output receipt, not by this signal.
    void finished(quint64 captureToken);
    void operationFailed(const QString& detail);
    void shutdownFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
