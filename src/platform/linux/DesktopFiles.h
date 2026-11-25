#pragma once

#include "services/desktop/DesktopResult.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Platform::Linux {

// GUI-thread adapter. FileManager1 and GIO own the actual file-manager launch;
// every callback is asynchronous on Qt's existing GLib event dispatcher.
class DesktopFiles final : public QObject {
    Q_OBJECT
public:
    explicit DesktopFiles(QObject* parent = nullptr);
    ~DesktopFiles() override;
    Desktop::Failure start(quint64 token, const QString& absolutePath, bool selectFile);
    void cancel();
Q_SIGNALS:
    void completed(quint64 token, const HuhuCapture::Desktop::Failure& failure, bool openedParentFallback);
private:
    struct State;
    std::unique_ptr<State> state_;
    void finish(quint64 token, Desktop::Failure failure, bool parentFallback);
};

} // namespace HuhuCapture::Platform::Linux
