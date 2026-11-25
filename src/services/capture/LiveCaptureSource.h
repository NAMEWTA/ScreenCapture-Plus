#pragma once

#include "capture/CaptureTypes.h"
#include "core/LiveSource.h"
#include <QObject>
#include <memory>
#include <optional>

class QWindow;

namespace HuhuCapture::Capture {

struct LiveCaptureOptions {
    bool monitors{true};
    bool windows{true};
    unsigned maximumFrameRate{30};
    // Two queued frames + one consumer-held frame + one producer write slot.
    // A queue cannot consume every slot or prevent the producer from replacing
    // an old frame. Pool bytes include every retained format generation.
    Core::FramePoolLimits pool{4, 128u * 1024u * 1024u, 16384};
};

// Shared ongoing Portal/PipeWire owner for scrolling and recording. The real
// first stream frame becomes an authorized-source Snapshot; callers must use
// the existing CaptureSession/Canvas to select it before claiming a consumer.
// An earlier Screenshot does not establish this stream's pixel coordinates.
class LiveCaptureSource final : public QObject {
    Q_OBJECT
public:
    explicit LiveCaptureSource(QObject* parent = nullptr);
    ~LiveCaptureSource() override;
    void initialize();
    Capability capability() const;
    bool busy() const noexcept;
    quint64 request(QWindow* parent, LiveCaptureOptions options = {});
    bool isCurrent(quint64 sourceId, quint64 generation) const;
    // GUI-thread, once per source. Queue frames retain their original native
    // crop/transform/color/timing. FrameNormalizer applies those plus the user
    // selection on the consumer worker; no per-frame GUI signals are used.
    // Transfers the actual first native frame once, so a static source stays
    // usable without manufacturing new frames from its QImage preview.
    std::optional<Core::LiveSourceConsumer> claim(quint64 sourceId, quint64 generation,
        Core::FrameCrop selection);
    // Call on the GUI thread only after the claimed consumer actually returned
    // and released its leases. Native/Portal teardown waits for this handshake.
    void consumerFinished(quint64 sourceId, quint64 generation);
    // Stop preserves queued complete frames for a claimed consumer to drain;
    // cancel is explicit abandonment and releases pending frames. Both retain
    // the Portal until the native producer and claimed consumer have finished.
    void stop(quint64 token);
    void cancel(quint64 token);
    void shutdown();

Q_SIGNALS:
    void initialized(const HuhuCapture::Capture::Capability& capability,
        const HuhuCapture::Capture::Failure& failure);
    void capabilityChanged(const HuhuCapture::Capture::Capability& capability);
    // Exactly one preparation terminal for every nonzero request token.
    void prepared(quint64 token, const HuhuCapture::Capture::Snapshot& source,
        const HuhuCapture::Capture::Failure& failure);
    void activityChanged(quint64 sourceId, quint64 generation,
        HuhuCapture::Core::LiveSourceActivity activity);
    void sourceClosed(quint64 sourceId, quint64 generation,
        const HuhuCapture::Capture::Failure& failure);
    // Real producer, preview, consumer and Portal callbacks have drained.
    void stopped(quint64 token, const HuhuCapture::Capture::Failure& failure);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Capture

Q_DECLARE_METATYPE(HuhuCapture::Core::LiveSourceActivity)
