#pragma once

#include "core/Startup.h"

#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace HuhuCapture::Application {

enum class InstanceRole { Pending, Primary, Independent, Forwarded, Failed };
enum class InstanceFailure {
    None, InvalidScope, RuntimeDirectory, Lock, Listen, PrimaryUnavailable,
    Rejected, DeliveryUnknown, InvalidReply,
};

struct InstanceResult {
    InstanceRole role{InstanceRole::Pending};
    InstanceFailure failure{InstanceFailure::None};
    QString detail;
};

// GUI-thread startup coordination using Qt's local IPC and crash-aware lock.
// The host supplies its actual per-user runtime directory and a stable scope
// for the current desktop/profile. The admission callback must return quickly;
// true means the application owns the action, not that capture has completed.
// No media, files, arbitrary commands, or platform handles cross this channel.
class SingleInstance final : public QObject {
    Q_OBJECT
public:
    using AdmitActivation = std::function<bool(const Core::StartupRequest&)>;
    SingleInstance(QString runtimeDirectory, QString scope, AdmitActivation admit,
        QObject* parent = nullptr);
    ~SingleInstance() override;
    // One asynchronous attempt per object. Independent --auto-quit requests do
    // not read/write a lock, bind an endpoint, or contact an existing instance.
    // Finish/verification requests only contact an existing primary. They never
    // elect a new primary and never fall back to the default capture action.
    bool start(Core::StartupRequest request);
    // Called before application shutdown. Keep the primary lock until this
    // coordinator is destroyed, so a second app cannot overlap resource exit.
    void stopAccepting();
    InstanceRole role() const noexcept;
    QString endpoint() const;

Q_SIGNALS:
    void startupFinished(const HuhuCapture::Application::InstanceResult& result);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application

Q_DECLARE_METATYPE(HuhuCapture::Application::InstanceResult)
