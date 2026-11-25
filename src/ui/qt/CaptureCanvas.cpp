#include "ui/qt/CaptureCanvas.h"
#include "capture/PixelSample.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace HuhuCapture::UI::Qt {
namespace {
using Capture::Action;
using Capture::SelectionHit;
using Capture::Stage;
QPoint pixel(QPointF point)
{
    // Qt positions are logical floating-point coordinates. Preserve source
    // pixel cells, including positions outside a view during mouse capture.
    const auto coordinate = [](qreal value) {
        return static_cast<int>(std::clamp(std::floor(value),
            static_cast<qreal>((std::numeric_limits<int>::min)()),
            static_cast<qreal>((std::numeric_limits<int>::max)())));
    };
    return {coordinate(point.x()), coordinate(point.y())};
}
std::unique_ptr<QTextLayout> createTextLayout(const QString& text, QFont font, qreal width)
{
    auto result = std::make_unique<QTextLayout>(text, font);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    result->setTextOption(option); result->setCacheEnabled(true); result->beginLayout();
    qreal y = 0;
    while (true) {
        auto line = result->createLine();
        if (!line.isValid()) break;
        line.setLineWidth((std::max)(qreal(1), width)); line.setPosition({0, y}); y += line.height();
    }
    result->endLayout(); return result;
}
QRectF fitInside(QRectF rectangle, QRectF bounds)
{
    rectangle.moveLeft(std::clamp(rectangle.left(), bounds.left(), (std::max)(bounds.left(), bounds.right() - rectangle.width())));
    rectangle.moveTop(std::clamp(rectangle.top(), bounds.top(), (std::max)(bounds.top(), bounds.bottom() - rectangle.height())));
    return rectangle;
}
}

struct CaptureCanvas::State {
    QPointer<Capture::CaptureSession> session;
    std::optional<Capture::ScreenBinding> screen;
    bool hints{true};
    QString sourcePosition{QStringLiteral("Image position")};
    QVector<QRectF> overlays;
    QString selectionText;
    qreal selectionWidth{};
    std::unique_ptr<QTextLayout> selectionLayout;
    QStringList sampleText;
    qreal sampleWidth{};
    std::array<std::unique_ptr<QTextLayout>, 4> sampleLayouts;
    QRectF imageRect(const CaptureCanvas& view) const
    {
        if (screen) return {QPointF(), QSizeF(screen->sourcePixels.width() / screen->pixelsPerDipX,
            screen->sourcePixels.height() / screen->pixelsPerDipY)};
        if (!session || session->snapshot().image.isNull()) return {};
        const auto size = session->snapshot().image.size();
        const qreal scale = (std::min)({qreal(1), view.width() / qreal(size.width()), view.height() / qreal(size.height())});
        const QSizeF extent(size.width() * scale, size.height() * scale);
        return {{(view.width() - extent.width()) / 2, (view.height() - extent.height()) / 2}, extent};
    }
};

CaptureCanvas::CaptureCanvas(Capture::CaptureSession& session,
    std::optional<Capture::ScreenBinding> screen, QWidget* parent)
    : QWidget(parent), state_(std::make_unique<State>())
{
    state_->session = &session; state_->screen = std::move(screen);
    setObjectName(QStringLiteral("capture.canvas")); setMouseTracking(true);
    setFocusPolicy(::Qt::StrongFocus); setAttribute(::Qt::WA_OpaquePaintEvent);
    auto chrome = font(); chrome.setPixelSize(14); chrome.setWeight(QFont::Normal); chrome.setItalic(false); setFont(chrome);
    connect(&session, &Capture::CaptureSession::changed, this, &CaptureCanvas::sync);
    connect(&session, &QObject::destroyed, this, [this] { setEnabled(false); update(); });
    sync();
}
CaptureCanvas::~CaptureCanvas() = default;
bool CaptureCanvas::desktopView() const noexcept { return state_->screen.has_value(); }
QRectF CaptureCanvas::sourceExtent() const
{
    if (state_->screen) return state_->screen->sourcePixels;
    return state_->session ? QRectF(state_->session->snapshot().image.rect()) : QRectF{};
}
QPointF CaptureCanvas::sourceAt(QPointF local) const
{
    if (state_->screen) return Capture::sourceFromScreenLocal(*state_->screen, local);
    const auto rectangle = state_->imageRect(*this);
    const auto source = sourceExtent();
    if (rectangle.isEmpty()) return {};
    return {(local.x() - rectangle.x()) * source.width() / rectangle.width(),
        (local.y() - rectangle.y()) * source.height() / rectangle.height()};
}
QPointF CaptureCanvas::localAt(QPointF source) const
{
    if (state_->screen) return Capture::screenLocalFromSource(*state_->screen, source);
    const auto rectangle = state_->imageRect(*this);
    const auto extent = sourceExtent();
    if (extent.isEmpty()) return {};
    return {rectangle.x() + source.x() * rectangle.width() / extent.width(),
        rectangle.y() + source.y() * rectangle.height() / extent.height()};
}
QRectF CaptureCanvas::localSelection() const
{
    if (!state_->session) return {};
    const auto& selected = state_->session->selection().bounds();
    return {localAt({selected.left, selected.top}), localAt({selected.right, selected.bottom})};
}
QRectF CaptureCanvas::localWorkArea() const
{
    if (state_->screen) return state_->screen->logicalWorkArea.translated(-state_->screen->logicalGeometry.topLeft()).intersected(rect());
    return rect();
}
void CaptureCanvas::setHintsVisible(bool visible)
{ if (state_->hints != visible) { state_->hints = visible; update(); } }
void CaptureCanvas::setOverlayAreas(QVector<QRectF> areas)
{ if (state_->overlays != areas) { state_->overlays = std::move(areas); update(); } }
void CaptureCanvas::setSourcePositionLabel(QString text)
{ state_->sourcePosition = std::move(text); state_->sampleText.clear(); update(); }
QSize CaptureCanvas::sizeHint() const
{
    if (state_->screen) return state_->screen->logicalGeometry.size().toSize();
    return state_->session ? state_->session->snapshot().image.size().boundedTo({1200, 800}).expandedTo({400, 300}) : QSize(640, 480);
}
void CaptureCanvas::sync()
{
    if (!state_->session) return;
    const auto stage = state_->session->stage();
    auto shape = ::Qt::ArrowCursor;
    if (stage == Stage::Selecting) shape = ::Qt::CrossCursor;
    else if (stage == Stage::Submitting) shape = ::Qt::WaitCursor;
    else if (stage == Stage::Adjusting) {
        const auto position = state_->session->cursor();
        switch (state_->session->selection().hitTest({static_cast<float>(position.x()), static_cast<float>(position.y())})) {
        case SelectionHit::TopLeft: case SelectionHit::BottomRight: shape = ::Qt::SizeFDiagCursor; break;
        case SelectionHit::TopRight: case SelectionHit::BottomLeft: shape = ::Qt::SizeBDiagCursor; break;
        case SelectionHit::Top: case SelectionHit::Bottom: shape = ::Qt::SizeVerCursor; break;
        case SelectionHit::Left: case SelectionHit::Right: shape = ::Qt::SizeHorCursor; break;
        case SelectionHit::Inside: shape = ::Qt::SizeAllCursor; break;
        default: break;
        }
    }
    setCursor(shape); update();
}
void CaptureCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this); painter.fillRect(rect(), QColor(32, 32, 32));
    if (!state_->session) return;
    const auto& snapshot = state_->session->snapshot();
    const auto image = state_->imageRect(*this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(image, snapshot.image, sourceExtent());
    const auto selected = localSelection();
    QPainterPath shade; shade.setFillRule(::Qt::OddEvenFill); shade.addRect(rect());
    if (state_->session->selection().hasSelection()) shade.addRect(selected);
    painter.fillPath(shade, QColor(0, 0, 0, 117));
    if (state_->session->selection().hasSelection()) {
        painter.setPen(QPen(QColor(22, 119, 255), 2)); painter.setBrush(::Qt::NoBrush);
        painter.drawRect(selected.adjusted(-1, -1, 1, 1));
        if (state_->hints) {
            const auto& bounds = state_->session->selection().bounds();
            const QString text = QStringLiteral("X:%1 Y:%2 R:%3 B:%4 W:%5 H:%6")
                .arg(bounds.left).arg(bounds.top).arg(bounds.right).arg(bounds.bottom)
                .arg(bounds.right - bounds.left).arg(bounds.bottom - bounds.top);
            constexpr qreal padding = 4;
            const qreal lineWidth = (std::max)(qreal(1), width() - padding * 4);
            if (state_->selectionText != text || state_->selectionWidth != lineWidth || !state_->selectionLayout) {
                auto textFont = font(); textFont.setPixelSize(10);
                state_->selectionLayout = createTextLayout(text, textFont, lineWidth);
                state_->selectionText = text; state_->selectionWidth = lineWidth;
            }
            qreal naturalWidth = 0;
            for (int index = 0; index < state_->selectionLayout->lineCount(); ++index)
                naturalWidth = (std::max)(naturalWidth, state_->selectionLayout->lineAt(index).naturalTextWidth());
            const QSizeF extent(naturalWidth, state_->selectionLayout->boundingRect().height());
            QRectF label({selected.left(), selected.top() - extent.height() - padding * 3}, extent + QSizeF(padding * 2, padding * 2));
            if (label.top() < 0) label.moveTop(selected.top() + padding / 2);
            label = fitInside(label, rect());
            for (const auto& overlay : state_->overlays) if (label.intersects(overlay)) {
                label.moveTop(selected.top() + padding); label = fitInside(label, rect());
            }
            painter.fillRect(label, QColor(0, 0, 0, 145)); painter.setPen(::Qt::white);
            state_->selectionLayout->draw(&painter, label.topLeft() + QPointF(padding, padding));
        }
    }
    if (state_->hints && state_->session->stage() == Stage::Selecting && !state_->session->pointerPressed()) drawMagnifier(painter);
}
void CaptureCanvas::drawMagnifier(QPainter& painter)
{
    const auto& snapshot = state_->session->snapshot();
    const auto position = state_->session->cursor();
    const auto color = snapshot.image.pixel(position);
    const auto [c, m, y, k] = Capture::cmyk(static_cast<std::uint8_t>(qRed(color)),
        static_cast<std::uint8_t>(qGreen(color)), static_cast<std::uint8_t>(qBlue(color)));
    const auto coordinate = snapshot.desktopOrigin ? position + *snapshot.desktopOrigin : position;
    const auto hex = QStringLiteral("%1%2%3").arg(qRed(color), 2, 16, QChar(u'0')).arg(qGreen(color), 2, 16, QChar(u'0')).arg(qBlue(color), 2, 16, QChar(u'0')).toUpper();
    QStringList texts{
        QStringLiteral("HEX (Ctrl+H) : #%1").arg(hex),
        QStringLiteral("RGB (Ctrl+R) : %1,%2,%3").arg(qRed(color)).arg(qGreen(color)).arg(qBlue(color)),
        QStringLiteral("CMYK (Ctrl+K) : %1,%2,%3,%4").arg(c).arg(m).arg(y).arg(k),
        QStringLiteral("%1 (Ctrl+P) : X:%2 Y:%3").arg(snapshot.desktopOrigin ? QStringLiteral("POS") : state_->sourcePosition).arg(coordinate.x()).arg(coordinate.y())};
    constexpr qreal padding = 7;
    // The existing magnifier samples 50x30 source pixels at five physical
    // display pixels per source pixel. Text stays in logical pixels at any DPR.
    const qreal scale = 5 / devicePixelRatioF();
    const QSizeF sampled(50 * scale, 30 * scale);
    const qreal boxWidth = (std::min)(qreal(width()), (std::max)(sampled.width(), qreal(250)));
    const qreal lineWidth = (std::max)(qreal(1), boxWidth - padding * 2);
    if (state_->sampleText != texts || state_->sampleWidth != lineWidth) {
        auto sampleFont = font(); sampleFont.setPixelSize(10);
        for (std::size_t index = 0; index < state_->sampleLayouts.size(); ++index)
            state_->sampleLayouts[index] = createTextLayout(texts[static_cast<qsizetype>(index)], sampleFont, lineWidth);
        state_->sampleText = texts; state_->sampleWidth = lineWidth;
    }
    qreal boxHeight = sampled.height() + padding;
    for (const auto& row : state_->sampleLayouts) boxHeight += row->boundingRect().height() + padding;
    const auto local = localAt(position);
    QPointF topLeft = local + QPointF(11, 11);
    if (topLeft.x() + boxWidth > width()) topLeft.setX(local.x() - 10 - boxWidth);
    if (topLeft.y() + boxHeight > height()) topLeft.setY(local.y() - 10 - boxHeight);
    auto frame = fitInside(QRectF(topLeft, QSizeF(boxWidth, boxHeight)), rect());
    for (const auto& overlay : state_->overlays) if (frame.intersects(overlay)) {
        frame.moveTop(overlay.top() - frame.height() - 4); frame = fitInside(frame, rect());
    }
    painter.fillRect(frame, QColor(0, 0, 0, 200));
    const QPointF origin = frame.topLeft() + QPointF((boxWidth - sampled.width()) / 2, 0);
    const QRect source(position.x() - 25, position.y() - 15, 50, 30);
    const auto valid = source.intersected(snapshot.image.rect());
    if (!valid.isEmpty()) {
        const auto offset = valid.topLeft() - source.topLeft();
        painter.drawImage(QRectF(origin + QPointF(offset.x() * scale, offset.y() * scale),
            QSizeF(valid.width() * scale, valid.height() * scale)), snapshot.image, valid);
    }
    const auto middle = origin + QPointF(sampled.width() / 2, sampled.height() / 2);
    const QColor cross(26, 128, 255, 128); constexpr qreal gap = 4;
    painter.fillRect(QRectF(origin.x(), middle.y() - gap, sampled.width() / 2 - gap, gap * 2), cross);
    painter.fillRect(QRectF(middle.x() + gap, middle.y() - gap, sampled.width() / 2 - gap, gap * 2), cross);
    painter.fillRect(QRectF(middle.x() - gap, origin.y(), gap * 2, sampled.height() / 2 - gap), cross);
    painter.fillRect(QRectF(middle.x() - gap, middle.y() + gap, gap * 2, sampled.height() / 2 - gap), cross);
    painter.setPen(::Qt::white); qreal rowY = frame.top() + sampled.height() + padding;
    for (const auto& row : state_->sampleLayouts) {
        row->draw(&painter, QPointF(frame.left() + padding, rowY)); rowY += row->boundingRect().height() + padding;
    }
}
void CaptureCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!state_->session) return;
    const QPointer<CaptureCanvas> lifetime(this);
    Q_EMIT interacted(this);
    if (lifetime && state_->session) state_->session->pointerMove(pixel(sourceAt(event->position())));
    event->accept();
}
void CaptureCanvas::mousePressEvent(QMouseEvent* event)
{
    if (!state_->session) return;
    if (event->button() == ::Qt::RightButton) { event->accept(); state_->session->cancel(); return; }
    if (event->button() != ::Qt::LeftButton) return;
    const QPointer<CaptureCanvas> lifetime(this);
    Q_EMIT interacted(this);
    if (!lifetime || !state_->session) return;
    setFocus(::Qt::MouseFocusReason);
    if (!lifetime || !state_->session) return;
    state_->session->pointerPress(pixel(sourceAt(event->position())));
    if (lifetime && state_->session && state_->session->pointerPressed()) grabMouse();
    event->accept();
}
void CaptureCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (!state_->session || event->button() != ::Qt::LeftButton) return;
    const QPointer<CaptureCanvas> lifetime(this);
    state_->session->pointerRelease(pixel(sourceAt(event->position())), event->modifiers().testFlag(::Qt::ControlModifier));
    if (lifetime && mouseGrabber() == this) releaseMouse();
    event->accept();
}
void CaptureCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (state_->session && event->button() == ::Qt::LeftButton && state_->session->stage() == Stage::Adjusting) {
        event->accept(); state_->session->request(Action::Clipboard); return;
    }
    mousePressEvent(event);
}
void CaptureCanvas::keyPressEvent(QKeyEvent* event)
{
    if (!state_->session) return;
    const auto key = event->key();
    event->accept();
    if (key == ::Qt::Key_Escape) { state_->session->cancel(); return; }
    if (event->modifiers().testFlag(::Qt::ControlModifier)) {
        std::optional<Action> action;
        if (key == ::Qt::Key_H) action = Action::Hex;
        else if (key == ::Qt::Key_R) action = Action::Rgb;
        else if (key == ::Qt::Key_K) action = Action::Cmyk;
        else if (key == ::Qt::Key_P) action = Action::Position;
        if (action) { state_->session->request(*action); return; }
    }
    if (key == ::Qt::Key_Up) state_->session->nudgeCursor({0, -1});
    else if (key == ::Qt::Key_Down) state_->session->nudgeCursor({0, 1});
    else if (key == ::Qt::Key_Left) state_->session->nudgeCursor({-1, 0});
    else if (key == ::Qt::Key_Right) state_->session->nudgeCursor({1, 0});
    else if ((key == ::Qt::Key_Return || key == ::Qt::Key_Enter) && state_->session->stage() == Stage::Adjusting)
        state_->session->request(Action::Clipboard);
    else event->ignore(); // Ctrl+C/S belong to the actual long/recording service, as in WinCap.
}
bool CaptureCanvas::event(QEvent* event)
{
    if (state_ && state_->session && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)) {
        const QPointer<CaptureCanvas> lifetime(this);
        state_->session->pointerCaptureLost();
        if (!lifetime) return true;
    }
    return QWidget::event(event);
}

} // namespace HuhuCapture::UI::Qt
