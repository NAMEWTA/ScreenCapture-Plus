#pragma once

#include "core/MediaFrame.h"

#include <QImage>
#include <QMetaType>
#include <QPointer>
#include <QRectF>
#include <QScreen>
#include <QString>
#include <QVector>
#include <optional>

namespace HuhuCapture::Capture {

enum class CoordinateSpace { DesktopPixels, AuthorizedSourcePixels };
enum class CapabilityState { Available, RequiresPermission, Unavailable, Busy };
struct Capability {
    CapabilityState state{CapabilityState::Unavailable};
    QString reason;
    bool canRequest() const noexcept
    { return state == CapabilityState::Available || state == CapabilityState::RequiresPermission; }
};

struct Failure {
    enum class Code { None, InvalidSnapshot, Busy, Unavailable, Cancelled, Denied, SourceChanged, Allocation, Platform };
    Code code{Code::None};
    qint64 nativeCode{};
    QString detail;
    explicit operator bool() const noexcept { return code != Code::None; }
};

// GUI-thread binding, verified by the platform service before publication.
// QScreen is borrowed through QPointer; no worker dereferences it. Native pixels
// already have the display rotation applied. nativeRotation describes the
// display configuration, and must not rotate those pixels a second time.
struct ScreenBinding {
    quint64 screenId{};
    QPointer<QScreen> screen;
    QString nativeId;
    QRect sourcePixels;
    QRectF logicalGeometry;
    QRectF logicalWorkArea;
    qreal pixelsPerDipX{1}, pixelsPerDipY{1};
    Core::FrameTransform nativeRotation{Core::FrameTransform::None};
};

// The producer transfers owned, immutable, top-down CPU pixels. Never wrap a
// reusable capture DIB, PipeWire buffer or borrowed data in this image. Native
// screen pixels are opaque SDR BGRA; authorized image alpha is preserved.
struct Snapshot {
    QImage image;
    quint64 sourceId{}, generation{};
    CoordinateSpace coordinates{CoordinateSpace::AuthorizedSourcePixels};
    std::optional<QPoint> desktopOrigin;
    QVector<ScreenBinding> screens;
    QVector<QRect> windows; // front-to-back source-pixel candidate rectangles
    QPoint cursor;
    QString sourceName;
};

enum class Action { Pin, Scroll, Record, Qr, History, SaveAs, Clipboard, Hex, Rgb, Cmyk, Position, Edit, Count };
enum class ActionResult { Completed, Cancelled, Failed };
enum class Stage { Empty, Selecting, Adjusting, Submitting, Closed };
struct StartOptions {
    // An explicit CLI entry takes precedence over Ctrl-release pinning.
    std::optional<Action> enter;
};
struct ActionRequest {
    quint64 token{}, sourceId{}, generation{};
    Action action{Action::Clipboard};
    CoordinateSpace coordinates{CoordinateSpace::AuthorizedSourcePixels};
    QRect sourceRect;
    std::optional<QRect> desktopRect;
    QImage image; // owned selection, copied on a worker, never a borrowed view
    QString text;
};

// Local widget DIP from one explicit screen binding, including positions
// outside that widget during mouse capture. Never scale a virtual desktop by
// one global DPR or reinterpret a Portal source position as a desktop point.
QPointF sourceFromScreenLocal(const ScreenBinding& binding, QPointF local);
QPointF screenLocalFromSource(const ScreenBinding& binding, QPointF source);

} // namespace HuhuCapture::Capture

Q_DECLARE_METATYPE(HuhuCapture::Capture::Failure)
Q_DECLARE_METATYPE(HuhuCapture::Capture::Snapshot)
Q_DECLARE_METATYPE(HuhuCapture::Capture::ActionRequest)
Q_DECLARE_METATYPE(HuhuCapture::Capture::Action)
Q_DECLARE_METATYPE(HuhuCapture::Capture::ActionResult)
Q_DECLARE_METATYPE(HuhuCapture::Capture::Stage)
