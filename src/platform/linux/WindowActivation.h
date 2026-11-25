#pragma once

#include <QObject>
#include <memory>
class QWindow;

namespace HuhuCapture::Platform::Linux {
// A real token is supplied by the launcher/Portal. Qt owns the Wayland display,
// surface and dispatcher; this adapter sends the standard activation request.
class WindowActivation final : public QObject {
    Q_OBJECT
public:
    explicit WindowActivation(QObject* parent = nullptr);
    ~WindowActivation() override;
    bool submit(QWindow* window, const QString& token);
Q_SIGNALS:
    void availabilityChanged();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
