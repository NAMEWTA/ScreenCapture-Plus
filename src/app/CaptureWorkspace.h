#pragma once

#include "core/Startup.h"
#include "services/capture/ScreenCaptureService.h"
#include "ui/qt/DesktopTypes.h"

#include <QObject>
#include <memory>

class QWidget;

namespace HuhuCapture::Desktop { class SystemClipboard; }
namespace HuhuCapture::Capture { class CaptureSession; class LiveCaptureSource; }
namespace HuhuCapture::UI::Qt { class CaptureWindow; }

namespace HuhuCapture::Application {
class MediaWorkspace;

// Application-owned capture journey. The native service supplies pixels, the
// session owns selection, and the existing editor/media/clipboard services own
// their respective operations. This controller never implements another editor
// or writes a second archive. Only live recording/scrolling need a separate
// application service to accept the explicitly identified selected source.
class CaptureWorkspace final : public QObject {
    Q_OBJECT
public:
    CaptureWorkspace(MediaWorkspace& media, Desktop::SystemClipboard& clipboard,
        QObject* parent = nullptr);
    ~CaptureWorkspace() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    void initialize();
    Capture::CaptureCapabilities capabilities() const;
    // Application lifetime owners. Record/scroll claim the single authorized
    // live source only after this workspace reselects its actual first frame.
    Capture::LiveCaptureSource& liveSource() noexcept;
    Capture::ScreenCaptureService& nativeSource() noexcept;
    bool sourceIsCurrent(quint64 sourceId, quint64 generation) const;
    bool start(Core::CaptureEntry entry = Core::CaptureEntry::Capture,
        const QString& activationToken = {});
    void setLiveCapability(Capture::Action action, Capture::Capability capability);
    void completeLiveAction(quint64 token, Capture::ActionResult result,
        const QString& detail = {});
    void closeLiveAction(quint64 token);
    Capture::CaptureSession& session() noexcept;
    UI::Qt::CaptureWindow& window() noexcept;
    bool busy() const noexcept;
    void shutdown();

Q_SIGNALS:
    void initialized();
    void capabilitiesChanged();
    void busyChanged(bool busy);
    void captureFinished();
    void operationFailed(const QString& message);
    void liveActionRequested(const HuhuCapture::Capture::ActionRequest& request, QWidget* origin);
    void liveActionCancelled(quint64 token);
    void liveActionActivationRequested(quint64 token, const QString& activationToken);
    void sourceInvalidated(quint64 sourceId, quint64 generation, const HuhuCapture::Capture::Failure& failure);
    void shutdownFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace HuhuCapture::Application
