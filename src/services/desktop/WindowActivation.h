#pragma once

#include "services/desktop/DesktopResult.h"
#include <QObject>
#include <memory>
class QWindow;

namespace HuhuCapture::Desktop {
struct WindowActivationResult {
    bool requested{}; // Native activation was requested, not necessarily granted.
    bool active{};    // Observed from Qt's actual platform focus state.
    Failure failure;
};

// One GUI-thread activation service can be reused by application windows.
// It never fabricates an input serial, changes a process-wide token environment,
// or treats sending a compositor request as proof of focus. Superseding a
// request cancels its wait; every live token has one queued terminal result.
class WindowActivation final : public QObject {
    Q_OBJECT
public:
    explicit WindowActivation(QObject* parent = nullptr);
    ~WindowActivation() override;
    quint64 request(QWindow* window, const QString& activationToken = {});
    void cancel(quint64 token);
Q_SIGNALS:
    void finished(quint64 token, const HuhuCapture::Desktop::WindowActivationResult& result);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct State;
    std::unique_ptr<State> state_;
    void attempt();
    void complete(Failure failure = {}, bool confirmed = false);
};
}
Q_DECLARE_METATYPE(HuhuCapture::Desktop::WindowActivationResult)
