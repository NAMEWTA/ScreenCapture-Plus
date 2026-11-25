#pragma once

#include "services/desktop/DesktopResult.h"
#include <QByteArray>
#include <QObject>
#include <memory>

class QMimeData;
class QWindow;

namespace HuhuCapture::Platform::Linux {

// Qt remains the data owner and provider. This adapter only verifies actual
// platform acceptance: X11's server owner via Qt, or this publication's MIME
// marker in a compositor-generated Wayland selection offer on the same seat.
class DesktopClipboard final : public QObject {
    Q_OBJECT
public:
    explicit DesktopClipboard(QObject* parent = nullptr);
    ~DesktopClipboard() override;
    Desktop::Failure begin(quint64 token, QWindow* origin, QMimeData* mime, const QByteArray& marker);
    void submitted();
    void cancel();
Q_SIGNALS:
    void verified(quint64 token, const HuhuCapture::Desktop::Failure& failure);
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Platform::Linux
