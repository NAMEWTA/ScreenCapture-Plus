#pragma once

#include <libportal/parent.h>
#include <gio/gio.h>
#include "platform/linux/portal/PortalTypes.h"
#include <QObject>
#include <memory>

class QWindow;

namespace HuhuCapture::Platform::Linux {

// Internal parent seam. Keep this object until the Portal request's terminal
// callback, then release it before QApplication tears down the Wayland QPA.
// All methods and cancellation run on the QApplication thread.
class PortalParent final : public QObject, public std::enable_shared_from_this<PortalParent> {
    Q_OBJECT
public:
    static std::shared_ptr<PortalParent> create();
    ~PortalParent() override;
    void prepare(QWindow* window, GCancellable* cancellable);
    XdpParent* nativeParent() const;

Q_SIGNALS:
    void prepared(const PortalFailure& failure);
    void invalidated(const PortalFailure& failure);

private:
    PortalParent();
    void start();
    void complete(PortalFailure failure);
    void invalidate(PortalFailure failure);
    void tryExport();
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace HuhuCapture::Platform::Linux
