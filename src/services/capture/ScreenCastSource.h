#pragma once

#include "capture/CaptureTypes.h"
#include "core/FrameSource.h"
#include <QObject>
#include <memory>

class QWindow;

namespace HuhuCapture::Capture {

// One authorized Portal/PipeWire source for both recording and manual scroll
// capture. Its GUI owner outlives the reader and keeps the Portal session alive
// until the producer has returned every buffer and released its remote FD.
class ScreenCastSource final : public QObject {
    Q_OBJECT
public:
    explicit ScreenCastSource(QObject* parent = nullptr);
    ~ScreenCastSource() override;
    bool start(QWindow* parent, bool cursor = true, quint32 maximumFrameRate = 0);
    bool busy() const noexcept;
    // Transfer exactly once after ready. Reads and stopAndWait are worker-only.
    // An unused selection may destroy its reader on the GUI thread: destruction
    // requests stop without waiting. Normal consumers call stopAndWait before
    // codec flushing/output so no native producer survives that transition.
    std::unique_ptr<Core::FrameSource> takeFrameSource();
    void stop();

Q_SIGNALS:
    void ready(const HuhuCapture::Core::FrameLease& preview);
    void failed(const HuhuCapture::Capture::Failure& failure);
    void stopped();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Capture
