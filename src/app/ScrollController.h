#pragma once

#include "capture/CaptureTypes.h"
#include "services/media/MediaArchiveService.h"
#include "ui/qt/DesktopTypes.h"
#include <QObject>
#include <memory>

class QWidget;
namespace HuhuCapture::Application {
class MediaWorkspace;

class ScrollController final : public QObject {
    Q_OBJECT
public:
    ScrollController(std::shared_ptr<Media::MediaArchiveService> archive,
        MediaWorkspace& workspace, QObject* parent = nullptr);
    ~ScrollController() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    bool begin(const Capture::ActionRequest& request, QWidget* parent = nullptr);
    void activate(const QString& activationToken = {});
    void setExternalStopAvailable(bool available, const QString& hint = {});
    bool finish();
    bool busy() const noexcept;
    void shutdown();

Q_SIGNALS:
    void busyChanged(bool busy);
    void archiveChanged();
    void operationFailed(const QString& message);
    void shutdownFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
