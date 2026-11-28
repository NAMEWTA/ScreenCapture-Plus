#pragma once

#include "capture/CaptureTypes.h"
#include <stop_token>

namespace HuhuCapture::Platform {

struct NativeScreen {
    QString id;
    QRect pixels;
    Core::FrameTransform rotation{Core::FrameTransform::None};
    quintptr nativeHandle{};
    bool operator==(const NativeScreen&) const = default;
};
struct NativeSnapshot {
    Capture::Failure failure;
    QImage image;
    QRect desktop;
    QVector<NativeScreen> screens;
    QVector<QRect> windows;
    QPoint cursor;
};

// Worker-only snapshot. The GUI service subsequently verifies each native
// display identity against the current QScreen before exposing any coordinates.
// Implementations never dereference a QWidget or QScreen from this worker.
NativeSnapshot captureNativeDesktop(std::stop_token stop = {});

} // namespace HuhuCapture::Platform
