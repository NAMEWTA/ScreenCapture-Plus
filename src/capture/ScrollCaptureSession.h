#pragma once

#include "capture/CaptureTypes.h"
#include "capture/ScrollCaptureModel.h"
#include "core/LiveSource.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Capture {
class ScreenCaptureService;
class LiveCaptureSource;

enum class ScrollPhase { Empty, Ready, Starting, Capturing, Stopping, Finished, Closed };
enum class ScrollNotice { Ready, ManualReady, Capturing, ManualCapturing, PointerPaused, SourcePaused, Finished };
struct ScrollSelection {
    quint64 sourceId{}, generation{};
    CoordinateSpace coordinates{CoordinateSpace::AuthorizedSourcePixels};
    QRect sourceRect;
    std::optional<QRect> desktopRect;
    QVector<ScreenBinding> screens;
    QImage preview;
};
struct ScrollProgress {
    QImage preview; // bounded visible tail, never the accumulated output
    QSize imageSize;
    ScrollNotice notice{ScrollNotice::Ready};
};
struct ScrollCompletion {
    ScrollStopReason reason{ScrollStopReason::None};
    Failure failure;
    QSize imageSize;
};

// One worker runs the retained CapLong matching/timing with the native adapter
// or the one already-authorized live consumer. Qt receives bounded previews and
// terminal state. Output QImages retain the completed immutable model allocation
// without copying the accumulated image or exposing the worker's mutable buffer.
class ScrollCaptureSession final : public QObject {
    Q_OBJECT
public:
    explicit ScrollCaptureSession(QObject* parent = nullptr);
    ~ScrollCaptureSession() override;
    Failure prepareNative(const ActionRequest& request, const Snapshot& source, ScreenCaptureService& owner);
    // A successful call adopts claim. On rejection the caller retains it and
    // remains responsible for releasing the source's consumer handshake.
    Failure prepareLive(const ActionRequest& request, LiveCaptureSource& owner, Core::LiveSourceConsumer& claim);
    const ScrollSelection& selection() const noexcept;
    ScrollPhase phase() const noexcept;
    ScrollProgress progress() const;
    ScrollCompletion completion() const;
    bool busy() const noexcept;
    bool hasCompleteImage() const noexcept;
    QImage image() const;
    quint64 start();
    void stop();
    // Actual Qt controls acknowledge each native readback after exclusion or
    // hiding; a missing/rejected acknowledgement fails within a bounded wait.
    void acknowledgeNativeCapture(quint64 token, quint64 sample, bool ready);
    void shutdown();

Q_SIGNALS:
    void changed();
    void previewChanged(const HuhuCapture::Capture::ScrollProgress& progress);
    void nativeCaptureRequested(quint64 token, quint64 sample);
    void nativeCaptureCompleted(quint64 token, quint64 sample);
    void finished(quint64 token, const HuhuCapture::Capture::ScrollCompletion& result);
    // Emitted only after the worker has released its source leases, immediately
    // before the owned LiveCaptureSource consumerFinished handshake.
    void consumerReleased(quint64 sourceId, quint64 generation);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace HuhuCapture::Capture

Q_DECLARE_METATYPE(HuhuCapture::Capture::ScrollPhase)
Q_DECLARE_METATYPE(HuhuCapture::Capture::ScrollProgress)
Q_DECLARE_METATYPE(HuhuCapture::Capture::ScrollCompletion)
