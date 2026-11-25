#pragma once

#include "capture/CaptureTypes.h"
#include "capture/SelectionModel.h"

#include <QObject>
#include <memory>

namespace HuhuCapture::Capture {

// One source-pixel selection and action lifecycle shared by all screen views.
// This object has no platform enumeration, capture API, clipboard, archive or
// widgets. The host connects real services and explicitly reports completion.
class CaptureSession final : public QObject {
    Q_OBJECT
public:
    explicit CaptureSession(QObject* parent = nullptr);
    ~CaptureSession() override;
    Failure load(Snapshot snapshot, StartOptions options = {});
    const Snapshot& snapshot() const noexcept;
    const SelectionModel& selection() const noexcept;
    Stage stage() const noexcept;
    QPoint cursor() const noexcept;
    bool pointerPressed() const noexcept;
    quint64 activeRequest() const noexcept;
    QString failureText() const;
    void setCapability(Action action, Capability capability);
    Capability capability(Action action) const;
    void pointerMove(QPoint source);
    void pointerPress(QPoint source);
    void pointerRelease(QPoint source, bool control);
    void pointerCaptureLost();
    void nudgeCursor(QPoint delta);
    quint64 request(Action action);
    void complete(quint64 token, ActionResult result, QString detail = {});
    void cancel();

Q_SIGNALS:
    void changed();
    void actionRequested(const HuhuCapture::Capture::ActionRequest& request);
    void actionCancelled(quint64 token);
    // Native backends may move a real cursor; a Wayland authorized-image view
    // simply renders this exact source-pixel position without pretending a warp.
    void cursorNudged(QPoint source);
    void closed();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Capture
