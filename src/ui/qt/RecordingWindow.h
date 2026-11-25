#pragma once

#include "recording/RecordingSession.h"
#include "ui/qt/DesktopTypes.h"

#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

// One shared view for both platforms. It requests intentions and displays the
// application-owned session's real state; closing it does not destroy a worker.
class RecordingWindow final : public QWidget {
    Q_OBJECT
public:
    explicit RecordingWindow(QWidget* parent = nullptr);
    ~RecordingWindow() override;
    void setTranslator(DesktopTranslator translator);
    void setRegion(Recording::PixelRegion region);
    void setState(Recording::SessionState state, Core::MediaTime elapsed = {},
        const QString& message = {}, const QString& savedPath = {});
    void setOutputPending(bool pending);
    void setControlsMayAppear(bool mayAppear);
    void showPrepared(const Recording::PreparedRecording& prepared);
    bool requestStart();
    bool requestStop(Recording::StopIntent intent);
    Recording::BackendRequest request() const;
    Recording::SessionState state() const noexcept;

Q_SIGNALS:
    void startRequested(const HuhuCapture::Recording::BackendRequest& request);
    void stopRequested(HuhuCapture::Recording::StopIntent intent);
    void fileRequested(const QString& path);
    void viewClosed();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void refresh();
};

} // namespace HuhuCapture::UI::Qt

Q_DECLARE_METATYPE(HuhuCapture::Recording::BackendRequest)
Q_DECLARE_METATYPE(HuhuCapture::Recording::StopIntent)
