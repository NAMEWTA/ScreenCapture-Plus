#pragma once

#include "capture/CaptureSession.h"
#include "services/desktop/WindowActivation.h"

#include <QObject>
#include <QStringView>
#include <functional>
#include <memory>

class QWidget;
class QWindow;

namespace HuhuCapture::UI::Qt {

// One presentation owner for a single CaptureSession. Native desktop mode
// creates one instance of the same canvas on each verified QScreen; an
// unmapped Portal image uses that canvas in a normal application window.
class CaptureWindow final : public QObject {
    Q_OBJECT
public:
    explicit CaptureWindow(Capture::CaptureSession& session, QObject* parent = nullptr);
    ~CaptureWindow() override;
    using Translator = std::function<QString(QStringView)>;
    void setTranslator(Translator translator);
    void refreshLanguage();
    // The session must already own a valid snapshot. Failure leaves it usable.
    Capture::Failure open();
    void activate(const QString& activationToken = {});
    void close();
    QWidget* dialogParent() const;
    QWindow* windowHandle() const;
    // The host hides the frozen capture before QR dialogs/live recording and
    // restores it if admission fails. No source pixels or selection are lost.
    void setPresentationVisible(bool visible);

Q_SIGNALS:
    void actionRequested(const HuhuCapture::Capture::ActionRequest& request, QWidget* origin);
    void closed();
    void activationFinished(const HuhuCapture::Desktop::WindowActivationResult& result);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::UI::Qt
