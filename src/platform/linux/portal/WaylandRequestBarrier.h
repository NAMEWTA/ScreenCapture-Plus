#pragma once

#include "platform/linux/portal/PortalTypes.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Platform::Linux {

// Acknowledges earlier requests on Qt's actual Wayland display without a
// blocking roundtrip, worker access to QWindow or a second event dispatcher.
// This is request ordering only: it is explicitly not presentation feedback
// and makes no claim about compositor animation or the contents of a frame.
class WaylandRequestBarrier final : public QObject {
    Q_OBJECT
public:
    explicit WaylandRequestBarrier(QObject* parent = nullptr);
    ~WaylandRequestBarrier() override;
    void start();
    void cancel();

Q_SIGNALS:
    void finished(const HuhuCapture::Platform::Linux::PortalFailure& failure);

private:
    struct Private;
    std::unique_ptr<Private> d;
    void complete(PortalFailure failure);
};

} // namespace HuhuCapture::Platform::Linux
