#pragma once

#include "capture/CaptureTypes.h"
#include "recording/RecordingSession.h"
#include "ui/qt/DesktopTypes.h"

#include <QWidget>
#include <QVector>
#include <memory>
#include <optional>

namespace HuhuCapture::UI::Qt {

struct RecordingOptions {
    Recording::Format format{Recording::Format::Mp4};
    bool systemAudio{true};
    bool microphone{};
};
struct RecordingCapabilities {
    Capture::Capability mp4, gif, systemAudio, microphone;
    // Supplied only by a backend that verifies native control geometry/shape
    // on every readback. It is not inferred from a Qt platform name by the UI.
    bool guardedOutsideControls{};
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
    void setExternalStopHint(const QString& hint);
    void showFailure(const QString& detail, const QString& retainedPath, bool canRetryOutput);
    // An explicit, already converted screen binding. No global DPR conversion.
    // Used only for first placement; language/scale refresh never recenters.
    void setPlacement(QScreen* screen, QRectF selection, QRectF workArea);
    // Verified desktop DIP areas outside the selected physical pixels. While
    // enabled, dragging/reflow stays inside one of these areas. Empty areas
    // clear the constraint; native readback still enforces actual geometry.
    bool setCaptureSafeAreas(QVector<QRect> areas);
    void setHiddenForCapture(bool hidden);
    bool hiddenForCapture() const noexcept;
    RecordingOptions options() const;

Q_SIGNALS:
    void startRequested(const HuhuCapture::UI::Qt::RecordingOptions& options);
    void stopRequested(HuhuCapture::Recording::StopIntent intent);
    void cancelRequested();
    void retryOutputRequested();
    void capturePlacementLost();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void refresh();
    void stop(Recording::StopIntent intent);
    void reflow();
    std::optional<QRect> safeGeometry(QSize size, QPoint preferred) const;
};

} // namespace HuhuCapture::UI::Qt

Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::RecordingOptions)
