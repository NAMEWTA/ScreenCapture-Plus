#pragma once

#include "capture/CaptureTypes.h"
#include "recording/RecordingBackend.h"
#include "services/media/MediaArchiveService.h"
#include "ui/qt/DesktopTypes.h"

#include <QObject>
#include <memory>

class QWidget;

namespace HuhuCapture::Application {
class MediaWorkspace;

// Owns the complete recording journey independently of the capture/toolbar
// view. Capture permission, region selection, recording and output have one
// owner and one archive service; views never close native media resources.
class RecordingController final : public QObject {
    Q_OBJECT
public:
    RecordingController(std::shared_ptr<Media::MediaArchiveService> archive,
        MediaWorkspace& workspace, QObject* parent = nullptr);
    ~RecordingController() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    bool begin(const Capture::ActionRequest& selection, QWidget* parent = nullptr);
    void activate(const QString& activationToken = {});
    void setExternalStopAvailable(bool available, const QString& hint = {});
    bool finish(Recording::StopIntent intent);
    bool busy() const noexcept;
    void shutdown();

Q_SIGNALS:
    void archiveChanged();
    void operationFailed(const QString& message);
    void busyChanged(bool busy);
    void shutdownFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
