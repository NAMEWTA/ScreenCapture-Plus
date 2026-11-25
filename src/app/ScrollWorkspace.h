#pragma once

#include "ui/qt/DesktopTypes.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Application {
class CaptureWorkspace;
class MediaWorkspace;

// Application composition for the shared scrolling session/view. Authorization
// and live-source ownership remain exclusively in CaptureWorkspace; completed
// images use MediaWorkspace's existing archive/clipboard/editor pipeline.
class ScrollWorkspace final : public QObject {
    Q_OBJECT
public:
    ScrollWorkspace(MediaWorkspace& media, CaptureWorkspace& capture, QObject* parent = nullptr);
    ~ScrollWorkspace() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    void setExternalStopAvailable(bool available);
    bool canStop() const noexcept;
    bool stopActive();
    bool controlsHiddenForCapture() const noexcept;
    bool busy() const noexcept;
    void shutdown();

Q_SIGNALS:
    void busyChanged(bool busy);
    void operationFailed(const QString& detail);
    void finished(quint64 captureToken);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace HuhuCapture::Application
