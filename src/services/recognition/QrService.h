#pragma once

#include "core/QrRecognition.h"
#include "platform/QrText.h"

#include <QImage>
#include <QString>

namespace HuhuCapture::Recognition {

struct QrResult {
    Core::QrDetection detection;
    Platform::QrTextDocument decoded;
    QString text;
};

// Synchronous worker entry point, shared by every Qt capture/pin surface. The
// caller supplies an immutable owned image for the duration of the job and
// applies its own request/lifetime token when dispatching the result to a view.
// No QApplication, clipboard publication or dialog lives in this service.
//
// Native BGRA/RGB32 images are borrowed without another full-image copy. Other
// formats normalize once to straight BGRA; that allocation is included in the
// working budget. Every failure preserves all fields of the prior output.
Core::QrError recognizeImage(const QImage& image, QrResult& output,
    const Core::QrLimits& limits = {}, std::stop_token stop = {});

} // namespace HuhuCapture::Recognition
