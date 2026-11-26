#pragma once

#include "capture/CaptureSession.h"
#include "ui/qt/DesktopTypes.h"
#include <QObject>
#include <memory>

class QWidget;

namespace HuhuCapture::UI::Qt {

// A single shared selection model drives one native-DIP surface per verified
// monitor. Portal screenshots use an ordinary source-image window instead.
// The wrapper is not a hidden master window and does not invent a global DPR.
class CaptureWindow final : public QObject {
    Q_OBJECT
public:
    explicit CaptureWindow(QObject* parent = nullptr);
    CaptureWindow(Capture::CaptureSession& session, QObject* parent = nullptr);
    ~CaptureWindow() override;
    Capture::CaptureSession* session() const noexcept;
    Capture::Failure showSnapshot(Capture::Snapshot snapshot, Capture::StartOptions options = {});
    void setTranslator(DesktopTranslator translator);
    QWidget* dialogParent() const noexcept;
    void hide();
    void hideForAction();
    void showAfterCancelledAction();
    void cancel();
    void shutdown();
    bool visible() const noexcept;

Q_SIGNALS:
    void closed();
    void sourceInvalidated(const HuhuCapture::Capture::Failure& failure);

private:
    struct State;
    std::unique_ptr<State> state_;
    void connectSession();
};

} // namespace HuhuCapture::UI::Qt
