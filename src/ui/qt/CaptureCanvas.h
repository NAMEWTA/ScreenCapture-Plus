#pragma once

#include "capture/CaptureSession.h"

#include <QPointer>
#include <QWidget>
#include <memory>
#include <optional>

namespace HuhuCapture::UI::Qt {

// The same selection view renders a native desktop screen or a Portal-provided
// image. All selection/crop/action rules remain in the shared CaptureSession.
class CaptureCanvas final : public QWidget {
    Q_OBJECT
public:
    CaptureCanvas(Capture::CaptureSession& session,
        std::optional<Capture::ScreenBinding> screen = {}, QWidget* parent = nullptr);
    ~CaptureCanvas() override;
    QPointF sourceAt(QPointF local) const;
    QPointF localAt(QPointF source) const;
    QRectF localSelection() const;
    QRectF sourceExtent() const;
    QRectF localWorkArea() const;
    bool desktopView() const noexcept;
    void setHintsVisible(bool visible);
    void setOverlayAreas(QVector<QRectF> areas);
    void setSourcePositionLabel(QString text);
    QSize sizeHint() const override;

Q_SIGNALS:
    void interacted(HuhuCapture::UI::Qt::CaptureCanvas* view);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void sync();
    void drawMagnifier(QPainter& painter);
};

} // namespace HuhuCapture::UI::Qt
