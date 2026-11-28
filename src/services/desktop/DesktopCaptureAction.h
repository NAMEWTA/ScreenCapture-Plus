#pragma once

#include "core/Startup.h"
#include "services/desktop/DesktopResult.h"

#include <QObject>
#include <QString>
#include <memory>

namespace HuhuCapture::Desktop {

// A verified route through the desktop's effective FinishCapture action and
// the existing primary-instance IPC. File presence and a GIO launch call alone
// never make this route available. This service does not stop a capture itself.
class DesktopCaptureAction final : public QObject {
    Q_OBJECT
public:
    DesktopCaptureAction(QString persistentExecutable, QString portableDirectory = {},
        QObject* parent = nullptr);
    ~DesktopCaptureAction() override;

    // Converts only a finish request from this service's scoped launch context
    // to a one-use verification. The environment value is removed immediately;
    // malformed values fail without executing the real finish action.
    static Failure applyProbeEnvironment(Core::StartupRequest& request);

    // Run while all capture controls remain visible and before starting work.
    // Zero means admission is closed. Each admitted token has one queued result.
    quint64 verify();
    void cancel(quint64 token);
    // Called by the same primary SingleInstance admission handler. Only the
    // current unconsumed challenge is accepted; no capture action is executed.
    bool acceptVerification(const Core::StartupRequest& request);
    bool available() const noexcept;
    QString actionName() const;
    void shutdown();

Q_SIGNALS:
    void verified(quint64 token, const HuhuCapture::Desktop::Failure& failure);
    void availabilityChanged(bool available);
    void shutdownFinished();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace HuhuCapture::Desktop
