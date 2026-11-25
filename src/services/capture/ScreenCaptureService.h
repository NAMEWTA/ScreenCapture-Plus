#pragma once

#include "capture/CaptureTypes.h"

#include <QObject>
#include <memory>

class QWindow;

namespace HuhuCapture::Capture {

struct CaptureCapabilities {
    Capability screenshot;
    Capability windowSelection;
    Capability desktopCoordinates;
    Capability cursorWarp;
    Capability automaticScroll;
    QString backend; // diagnostic identity; the host owns localized UI wording
};

// QApplication-owned capture entry. Native screenshots run on one reusable
// worker backend; the existing PortalCapture owns Linux authorization. No
// widget, image selection, clipboard, archive or QR policy lives in this class.
class ScreenCaptureService final : public QObject {
    Q_OBJECT
public:
    explicit ScreenCaptureService(QObject* parent = nullptr);
    ~ScreenCaptureService() override;
    void initialize();
    CaptureCapabilities capabilities() const;
    bool busy() const noexcept;
    // GUI-thread check against the published source and the current real Qt
    // screen configuration. A frozen image stays readable after invalidation,
    // but its former native coordinates cannot admit recording or scrolling.
    bool isCurrent(quint64 sourceId, quint64 generation) const;
    // Each nonzero token has one queued terminal signal while the service lives.
    // Returns zero after shutdown has closed admission.
    // A null parent is a genuine parent-less request, never a fabricated handle.
    quint64 request(QWindow* parent = nullptr);
    void cancel(quint64 token);
    // Source-pixel target from the active captured desktop. Authorized-image
    // sessions use their local cursor and never request a global Wayland warp.
    void moveCursor(quint64 sourceId, quint64 generation, QPoint source);
    // Stop accepting work, cancel authorization/preparation, then notify after
    // the worker and callbacks drain. The host keeps the GUI dispatcher alive.
    void shutdown();

Q_SIGNALS:
    void initialized(const HuhuCapture::Capture::CaptureCapabilities& capabilities,
        const HuhuCapture::Capture::Failure& failure);
    void capabilitiesChanged(const HuhuCapture::Capture::CaptureCapabilities& capabilities);
    void finished(quint64 token, const HuhuCapture::Capture::Snapshot& snapshot,
        const HuhuCapture::Capture::Failure& failure);
    void cursorMoved(quint64 sourceId, quint64 generation, QPoint requestedSource, QPoint actualSource,
        const HuhuCapture::Capture::Failure& failure);
    void sourceInvalidated(quint64 sourceId, quint64 generation,
        const HuhuCapture::Capture::Failure& failure);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Capture

Q_DECLARE_METATYPE(HuhuCapture::Capture::CaptureCapabilities)
