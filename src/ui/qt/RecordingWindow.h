#pragma once

#include "capture/CaptureTypes.h"
#include "recording/RecordingSession.h"
#include "ui/qt/DesktopTypes.h"

#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

struct RecordingOptions {
    Recording::Format format{Recording::Format::Mp4};
    bool systemAudio{true};
    bool microphone{};
};
struct RecordingCapabilities {
    Capture::Capability mp4, gif, systemAudio, microphone;
};

// Shared recording toolbar. It presents only actual service state: a click is
// not a prepared encoder, and a completed recording is not a clipboard receipt.
// The application owns capture, timestamps, finalization and durable media.
class RecordingWindow final : public QWidget {
    Q_OBJECT
public:
    explicit RecordingWindow(QWidget* parent = nullptr);
    ~RecordingWindow() override;
    void setTranslator(DesktopTranslator translator);
    void setCapabilities(RecordingCapabilities capabilities);
    void setState(Recording::SessionState state);
    void setElapsed(Core::MediaTime elapsed);
    void setOutputPending(bool pending);
    void setCaptureExcluded(bool excluded);
    void showFailure(const QString& detail, const QString& retainedPath, bool canRetryOutput);
    // An explicit, already converted screen binding. No global DPR conversion.
    // Used only for first placement; language/scale refresh never recenters.
    void setPlacement(QScreen* screen, QRectF selection, QRectF workArea);
    RecordingOptions options() const;

Q_SIGNALS:
    void startRequested(const HuhuCapture::UI::Qt::RecordingOptions& options);
    void stopRequested(HuhuCapture::Recording::StopIntent intent);
    void cancelRequested();
    void retryOutputRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void refresh();
    void stop(Recording::StopIntent intent);
    void reflow();
};

} // namespace HuhuCapture::UI::Qt

Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::RecordingOptions)
