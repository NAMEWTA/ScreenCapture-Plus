#pragma once

#include "capture/CaptureTypes.h"
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <memory>
#include <stop_token>

namespace HuhuCapture::Platform {

struct CaptureDisplay {
    QString name;
    // Windows: HMONITOR, matched to Qt's public native screen interface.
    // X11: monitor/output identity; the name is the exact RandR/QScreen name.
    quintptr nativeId{};
    QRect pixels;
    Core::FrameTransform rotation{Core::FrameTransform::None};
    bool operator==(const CaptureDisplay&) const = default;
};
struct CaptureDesktop {
    QRect bounds;
    QVector<CaptureDisplay> displays;
    QVector<QRect> windows; // front to back, already in native desktop pixels
    QPoint cursor;
    bool sameLayout(const CaptureDesktop& other) const
    { return bounds == other.bounds && displays == other.displays; }
};
struct NativeScreenshot {
    CaptureDesktop desktop;
    QImage image; // owned, opaque, devicePixelRatio == 1
    Capture::Failure failure;
};

// Internal worker-only OS seam. The same worker owns initialization, repeated
// native capture, cursor movement and destruction. No QObject/QScreen/widget or
// capture/selection/output policy belongs here. The Windows implementation uses
// the existing GdiCapture; the X11 implementation uses its reusable XCB surface.
class ScreenCaptureBackend final {
public:
    ScreenCaptureBackend();
    ~ScreenCaptureBackend();
    ScreenCaptureBackend(const ScreenCaptureBackend&) = delete;
    ScreenCaptureBackend& operator=(const ScreenCaptureBackend&) = delete;
    Capture::Failure initialize();
    bool requiresPortal() const noexcept;
    NativeScreenshot capture(std::stop_token stop);
    Capture::Failure moveCursor(const CaptureDesktop& expected, QPoint target,
        QPoint& actual, std::stop_token stop);
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Platform
