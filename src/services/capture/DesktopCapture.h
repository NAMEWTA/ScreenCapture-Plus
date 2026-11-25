#pragma once

#include "capture/CaptureTypes.h"
#include <QObject>
#include <memory>

class QWindow;

namespace HuhuCapture::Capture {

// GUI-owned asynchronous snapshot service. Native desktop capture and Portal
// decoding use one bounded worker; Portal requests stay on the Qt dispatcher.
class DesktopCapture final : public QObject {
    Q_OBJECT
public:
    explicit DesktopCapture(QObject* parent = nullptr);
    ~DesktopCapture() override;
    void initialize();
    Capability capability() const;
    bool busy() const noexcept;
    // A zero token rejects admission. Every accepted token has exactly one
    // queued finished delivery, including cancellation. Even a terminal error
    // during admission returns first; late workers never publish a new image.
    quint64 request(QWindow* parent = nullptr);
    void cancel(quint64 token);
    void shutdown();

Q_SIGNALS:
    void initialized(const HuhuCapture::Capture::Capability& capability);
    void capabilityChanged(const HuhuCapture::Capture::Capability& capability);
    void busyChanged(bool busy);
    void shutdownFinished();
    void finished(quint64 token, const HuhuCapture::Capture::Snapshot& snapshot,
        const HuhuCapture::Capture::Failure& failure);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Capture

Q_DECLARE_METATYPE(HuhuCapture::Capture::Capability)
