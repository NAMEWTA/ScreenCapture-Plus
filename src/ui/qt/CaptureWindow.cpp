#include "ui/qt/CaptureWindow.h"

#include "capture/PixelSample.h"
#include "platform/DesktopPointer.h"
#include "ui/qt/DesktopTheme.h"
#include "ui/toolbars/ToolbarMetrics.h"
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QFontDatabase>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResource>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

static void initializeCaptureResources() { Q_INIT_RESOURCE(huhucapture_ui); }

namespace HuhuCapture::UI::Qt {
namespace {
using Capture::Action;
using Capture::Stage;
constexpr int buttonSize = static_cast<int>(UI::toolbarButtonSize);
struct Tool { std::optional<Action> action; const char* key; char16_t glyph; };
constexpr std::array tools{
    Tool{Action::Pin, "cap.mark", u'\ue97f'}, Tool{Action::Scroll, "cap.long", u'\ue73e'},
    Tool{Action::Record, "cap.video", u'\ue660'}, Tool{Action::Qr, "cap.qrcode", u'\ue71e'},
    Tool{Action::History, "clipboard.title", u'\ue77f'}, Tool{{}, "tool.close", u'\ue62d'},
    Tool{Action::SaveAs, "tool.save", u'\ue608'}, Tool{Action::Clipboard, "tool.clipboard", u'\ue6ad'}
};
QString iconFamily()
{
    static const auto family = [] {
        initializeCaptureResources();
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/huhucapture/editor/iconfont.ttf"));
        const auto names = id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
        return names.isEmpty() ? QString() : names.front();
    }();
    return family;
}
QString label(const DesktopTranslator& translator, const char* key)
{
    const auto name = QString::fromLatin1(key);
    const auto result = translator ? translator(name) : QString();
    if (!result.isEmpty() && result != name) return result;
    if (name == QStringLiteral("cap.mark")) return QStringLiteral("Annotate / pin");
    if (name == QStringLiteral("cap.long")) return QStringLiteral("Scrolling capture");
    if (name == QStringLiteral("cap.video")) return QStringLiteral("Record");
    if (name == QStringLiteral("cap.qrcode")) return QStringLiteral("Read QR code");
    if (name == QStringLiteral("clipboard.title")) return QStringLiteral("History");
    if (name == QStringLiteral("tool.close")) return QStringLiteral("Close");
    if (name == QStringLiteral("tool.save")) return QStringLiteral("Save as");
    if (name == QStringLiteral("tool.clipboard")) return QStringLiteral("Copy");
    if (name == QStringLiteral("capture.sourceCoordinates")) return QStringLiteral("Authorized image · coordinates are source pixels");
    if (name == QStringLiteral("capture.preparing")) return QStringLiteral("Preparing the selected image…");
    if (name == QStringLiteral("capture.sourcePosition")) return QStringLiteral("Source");
    return name;
}
QRectF selectionRect(const Capture::CaptureSession& session)
{
    const auto& rect = session.selection().bounds();
    return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
}
::Qt::CursorShape cursorFor(Capture::SelectionHit hit)
{
    using Hit = Capture::SelectionHit;
    switch (hit) {
    case Hit::TopLeft: case Hit::BottomRight: return ::Qt::SizeFDiagCursor;
    case Hit::TopRight: case Hit::BottomLeft: return ::Qt::SizeBDiagCursor;
    case Hit::Top: case Hit::Bottom: return ::Qt::SizeVerCursor;
    case Hit::Left: case Hit::Right: return ::Qt::SizeHorCursor;
    case Hit::Inside: return ::Qt::SizeAllCursor;
    default: return ::Qt::CrossCursor;
    }
}

class Surface final : public QWidget {
public:
    QPointer<Capture::CaptureSession> session;
    std::optional<Capture::ScreenBinding> screenBinding;
    DesktopTranslator translator;
    std::function<QPoint(QPointF)> desktopPoint;
    std::function<void()> resized;
    QFrame* toolbar{};
    QLabel* status{};
    std::array<QToolButton*, tools.size()> buttons{};
    bool shuttingDown{};

    Surface(Capture::CaptureSession& model, std::optional<Capture::ScreenBinding> binding)
        : QWidget(nullptr, binding ? ::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint : ::Qt::Window),
          session(&model), screenBinding(std::move(binding))
    {
        setObjectName(QStringLiteral("captureSurface")); setWindowTitle(QStringLiteral("huhucapture"));
        setAttribute(::Qt::WA_OpaquePaintEvent); setMouseTracking(true); setFocusPolicy(::Qt::StrongFocus);
        setFont(DesktopTheme::font(12)); setCursor(::Qt::CrossCursor);
        toolbar = new QFrame(this); toolbar->setObjectName(QStringLiteral("captureToolbar"));
        toolbar->setStyleSheet(QStringLiteral("QFrame#captureToolbar { background: white; border: 1px solid #a8a8a8; }"
            "QToolButton { background: transparent; color: #333333; border: 0; padding: 0; }"
            "QToolButton:hover { background: #f2f2f2; } QToolButton:focus { background: #e6f4ff; }"
            "QToolButton:disabled { color: #969696; }"));
        const auto family = iconFamily();
        for (std::size_t i = 0; i < tools.size(); ++i) {
            auto* button = buttons[i] = new QToolButton(toolbar);
            button->setObjectName(QStringLiteral("capture.") + QString::fromLatin1(tools[i].key));
            button->setFixedSize(buttonSize, buttonSize); button->setFocusPolicy(::Qt::StrongFocus);
            if (tools[i].action == Action::History) {
                // History's old glyph belongs to Windows-only Segoe MDL2,
                // not the application's icon font used by both platforms.
                button->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
                button->setIconSize(QSize(24, 24));
            } else if (!family.isEmpty()) {
                QFont icon(family); icon.setPixelSize(static_cast<int>(UI::toolbarIconSize));
                button->setFont(icon); button->setText(QString(QChar(tools[i].glyph)));
            } else {
                button->setIcon(style()->standardIcon(i == 5 ? QStyle::SP_DialogCancelButton :
                    i == 6 ? QStyle::SP_DialogSaveButton : QStyle::SP_FileDialogDetailedView));
            }
            connect(button, &QToolButton::clicked, &model, [&model, action = tools[i].action] {
                if (action) model.request(*action); else model.cancel();
            });
        }
        toolbar->hide();
        status = new QLabel(this); status->setTextFormat(::Qt::PlainText); status->setWordWrap(true);
        status->setFont(DesktopTheme::font(12)); status->setStyleSheet(QStringLiteral("color: white; background: rgba(0,0,0,190); padding: 8px;"));
        status->hide();
        addShortcut(QKeySequence(::Qt::Key_Escape), [&model] { model.cancel(); });
        addShortcut(QKeySequence(::Qt::Key_Return), [&model] { model.request(Action::Clipboard); });
        addShortcut(QKeySequence(::Qt::Key_Enter), [&model] { model.request(Action::Clipboard); });
        // Baseline Ctrl+C/S belong to scrolling/recording. In selection mode,
        // Enter or the actual toolbar copies an adjusted selection.
        for (const auto& [key, action] : std::array<std::pair<int, Action>, 4>{
            {{::Qt::Key_H, Action::Hex},
             {::Qt::Key_R, Action::Rgb}, {::Qt::Key_K, Action::Cmyk}, {::Qt::Key_P, Action::Position}}})
            addShortcut(QKeySequence(::Qt::CTRL | key), [&model, action] { model.request(action); });
    }
    void addShortcut(const QKeySequence& key, std::function<void()> action)
    {
        auto* shortcut = new QShortcut(key, this);
        shortcut->setContext(::Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, session, std::move(action));
    }
    QRectF imageViewport() const
    {
        if (screenBinding) return rect();
        if (!session || session->snapshot().image.isNull()) return {};
        const QSizeF available = QSizeF(size()) - QSizeF(24, 24);
        const auto original = QSizeF(session->snapshot().image.size());
        const auto scale = std::min({1.0, available.width() / original.width(), available.height() / original.height()});
        const QSizeF shown(original.width() * scale, original.height() * scale);
        return {(width() - shown.width()) / 2, (height() - shown.height()) / 2, shown.width(), shown.height()};
    }
    QRectF sourceViewport() const
    { return screenBinding ? QRectF(screenBinding->sourcePixels) : QRectF(session->snapshot().image.rect()); }
    QPointF sourcePoint(QPointF local) const
    {
        const auto view = imageViewport(), source = sourceViewport();
        return {source.x() + (local.x() - view.x()) * source.width() / std::max(1.0, view.width()),
            source.y() + (local.y() - view.y()) * source.height() / std::max(1.0, view.height())};
    }
    QPointF localPoint(QPointF source) const
    {
        const auto view = imageViewport(), pixels = sourceViewport();
        return {view.x() + (source.x() - pixels.x()) * view.width() / pixels.width(),
            view.y() + (source.y() - pixels.y()) * view.height() / pixels.height()};
    }
    QPoint eventPoint(QMouseEvent* event) const
    {
        const auto point = screenBinding && desktopPoint ? QPointF(desktopPoint(event->globalPosition())) : sourcePoint(event->position());
        return {static_cast<int>(std::floor(point.x())), static_cast<int>(std::floor(point.y()))};
    }
    void refresh(bool showToolbar)
    {
        if (!session) return;
        const bool selecting = session->stage() == Stage::Selecting;
        const bool busy = session->stage() == Stage::Submitting;
        setCursor(busy ? ::Qt::WaitCursor : selecting ? ::Qt::CrossCursor :
            cursorFor(session->selection().hitTest({static_cast<float>(session->cursor().x()), static_cast<float>(session->cursor().y())})));
        for (std::size_t i = 0; i < tools.size(); ++i) {
            auto title = label(translator, tools[i].key);
            const auto capability = tools[i].action ? session->capability(*tools[i].action) : Capture::Capability{Capture::CapabilityState::Available, {}};
            buttons[i]->setEnabled(!busy && (!tools[i].action || capability.canRequest()));
            if (!capability.reason.isEmpty()) title += QStringLiteral("\n") + capability.reason;
            buttons[i]->setToolTip(title); buttons[i]->setAccessibleName(label(translator, tools[i].key));
        }
        QString message = session->failureText();
        if (!screenBinding && message.isEmpty()) message = label(translator, "capture.sourceCoordinates");
        if (busy && message.isEmpty()) message = label(translator, "capture.preparing");
        status->setText(message); status->setFixedWidth(std::max(1, std::min(500, width() - 24)));
        status->setFixedHeight(std::max(1, status->heightForWidth(status->width())));
        status->move(12, std::max(12, height() - status->height() - 12)); status->setVisible(!message.isEmpty());
        auto area = screenBinding ? screenBinding->logicalWorkArea.translated(-screenBinding->logicalGeometry.topLeft()).intersected(rect()) : QRectF(rect());
        area.adjust(8, 8, -8, -8);
        if (!message.isEmpty()) area.setBottom(std::max(area.top() + buttonSize + 2, std::min(area.bottom(), qreal(status->y() - 8))));
        const int columns = std::max(1, std::min(static_cast<int>(tools.size()), (qFloor(area.width()) - 2) / buttonSize));
        const int rows = (static_cast<int>(tools.size()) + columns - 1) / columns;
        toolbar->resize(columns * buttonSize + 2, rows * buttonSize + 2);
        for (std::size_t i = 0; i < tools.size(); ++i)
            buttons[i]->move(1 + static_cast<int>(i) % columns * buttonSize, 1 + static_cast<int>(i) / columns * buttonSize);
        const auto selected = selectionRect(*session);
        const auto corner = localPoint(selected.bottomRight());
        int x = static_cast<int>(corner.x()) - toolbar->width();
        int y = static_cast<int>(corner.y()) + 8;
        if (y + toolbar->height() > area.bottom()) y = static_cast<int>(localPoint(selected.topLeft()).y()) - toolbar->height() - 8;
        toolbar->move(qRound(std::clamp(qreal(x), area.left(), std::max(area.left(), area.right() - toolbar->width()))),
            qRound(std::clamp(qreal(y), area.top(), std::max(area.top(), area.bottom() - toolbar->height()))));
        toolbar->setVisible(showToolbar && !session->pointerPressed() && session->stage() == Stage::Adjusting);
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), QColor(36, 38, 42));
        if (!session) return;
        const auto view = imageViewport(), source = sourceViewport();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(view, session->snapshot().image, source);
        const auto selection = selectionRect(*session);
        const auto first = localPoint(selection.topLeft()), last = localPoint(selection.bottomRight());
        const QRectF selected(first, last);
        const auto clipped = selected.intersected(view);
        const QColor mask(0, 0, 0, 117);
        if (clipped.isEmpty()) painter.fillRect(view, mask);
        else {
            painter.fillRect(QRectF(view.left(), view.top(), view.width(), clipped.top() - view.top()), mask);
            painter.fillRect(QRectF(view.left(), clipped.bottom(), view.width(), view.bottom() - clipped.bottom()), mask);
            painter.fillRect(QRectF(view.left(), clipped.top(), clipped.left() - view.left(), clipped.height()), mask);
            painter.fillRect(QRectF(clipped.right(), clipped.top(), view.right() - clipped.right(), clipped.height()), mask);
        }
        if (session->selection().hasSelection()) {
            painter.setPen(QPen(QColor(22, 119, 255), 2)); painter.setBrush(::Qt::NoBrush); painter.drawRect(selected);
            if (session->stage() == Stage::Adjusting) {
                painter.setBrush(::Qt::white);
                const std::array<QPointF, 8> handles{selected.topLeft(), QPointF(selected.center().x(), selected.top()), selected.topRight(),
                    QPointF(selected.right(), selected.center().y()), selected.bottomRight(), QPointF(selected.center().x(), selected.bottom()),
                    selected.bottomLeft(), QPointF(selected.left(), selected.center().y())};
                for (const auto handle : handles) painter.drawRect(QRectF(handle.x() - 3, handle.y() - 3, 6, 6));
            }
            const auto origin = session->snapshot().desktopOrigin.value_or(QPoint());
            const auto text = QStringLiteral("X:%1 Y:%2  %3 × %4").arg(static_cast<int>(selection.x()) + origin.x())
                .arg(static_cast<int>(selection.y()) + origin.y()).arg(static_cast<int>(selection.width())).arg(static_cast<int>(selection.height()));
            auto captionFont = font(); captionFont.setPixelSize(10); painter.setFont(captionFont);
            const QFontMetrics metrics(captionFont); const QSizeF size(metrics.horizontalAdvance(text) + 16, metrics.height() + 10);
            const QPointF labelAt(std::clamp(selected.left(), 4.0, std::max(4.0, width() - size.width() - 4)),
                selected.top() > size.height() + 8 ? selected.top() - size.height() - 4 : std::max(4.0, selected.top() + 4));
            const QRectF box(labelAt, size); painter.fillRect(box, QColor(0, 0, 0, 180)); painter.setPen(::Qt::white);
            painter.drawText(box.adjusted(8, 0, -8, 0), ::Qt::AlignVCenter | ::Qt::AlignLeft, text);
        }
        if (session->stage() == Stage::Selecting && !session->pointerPressed() && source.contains(session->cursor()))
            paintMagnifier(painter);
    }
    void paintMagnifier(QPainter& painter)
    {
        const auto point = session->cursor(); const auto position = localPoint(point);
        constexpr int sampleWidth = 50, sampleHeight = 30;
        const qreal zoom = 5 / devicePixelRatioF(); // five physical display pixels per source pixel
        const qreal imageWidth = sampleWidth * zoom, imageHeight = sampleHeight * zoom;
        const int boxWidth = std::min(width(), std::max(250, qCeil(imageWidth)));
        auto sampleFont = font(); sampleFont.setPixelSize(10); painter.setFont(sampleFont);
        const QFontMetrics metrics(sampleFont);
        const int boxHeight = qCeil(imageHeight) + metrics.height() * 4 + 22;
        int x = static_cast<int>(position.x()) + 12, y = static_cast<int>(position.y()) + 12;
        if (x + boxWidth > width()) x = static_cast<int>(position.x()) - boxWidth - 12;
        if (y + boxHeight > height()) y = static_cast<int>(position.y()) - boxHeight - 12;
        x = std::clamp(x, 0, std::max(0, width() - boxWidth)); y = std::clamp(y, 0, std::max(0, height() - boxHeight));
        if (status->isVisible() && QRect(x, y, boxWidth, boxHeight).intersects(status->geometry()))
            y = std::max(0, status->y() - boxHeight - 4);
        const QRectF box(x, y, boxWidth, boxHeight); painter.fillRect(box, QColor(0, 0, 0, 200));
        const qreal imageX = x + (boxWidth - imageWidth) / 2;
        const QRect sample(point.x() - sampleWidth / 2, point.y() - sampleHeight / 2, sampleWidth, sampleHeight);
        const auto clipped = sample.intersected(session->snapshot().image.rect());
        const QRectF target(imageX + (clipped.x() - sample.x()) * zoom, y + (clipped.y() - sample.y()) * zoom,
            clipped.width() * zoom, clipped.height() * zoom);
        painter.drawImage(target, session->snapshot().image, clipped);
        painter.setPen(QPen(QColor(26, 128, 255, 180), 1));
        painter.drawLine(QPointF(imageX, y + imageHeight / 2 + zoom / 2), QPointF(imageX + imageWidth, y + imageHeight / 2 + zoom / 2));
        painter.drawLine(QPointF(imageX + imageWidth / 2 + zoom / 2, y), QPointF(imageX + imageWidth / 2 + zoom / 2, y + imageHeight));
        painter.setPen(QPen(::Qt::white, 1)); painter.setBrush(::Qt::NoBrush);
        painter.drawRect(QRectF(imageX + imageWidth / 2, y + imageHeight / 2, zoom, zoom));
        const auto rgb = session->snapshot().image.pixel(point);
        const auto [cyan, magenta, yellow, black] = Capture::cmyk(static_cast<std::uint8_t>(qRed(rgb)),
            static_cast<std::uint8_t>(qGreen(rgb)), static_cast<std::uint8_t>(qBlue(rgb)));
        const auto actual = session->snapshot().desktopOrigin ? point + *session->snapshot().desktopOrigin : point;
        const std::array text{
            QStringLiteral("HEX (Ctrl+H): #%1%2%3").arg(qRed(rgb), 2, 16, QChar(u'0')).arg(qGreen(rgb), 2, 16, QChar(u'0')).arg(qBlue(rgb), 2, 16, QChar(u'0')).toUpper(),
            QStringLiteral("RGB (Ctrl+R): %1,%2,%3").arg(qRed(rgb)).arg(qGreen(rgb)).arg(qBlue(rgb)),
            QStringLiteral("CMYK (Ctrl+K): %1,%2,%3,%4").arg(cyan).arg(magenta).arg(yellow).arg(black),
            QStringLiteral("%1 (Ctrl+P): %2,%3").arg(session->snapshot().desktopOrigin ? QStringLiteral("POS") : label(translator, "capture.sourcePosition"))
                .arg(actual.x()).arg(actual.y())};
        painter.setPen(::Qt::white);
        for (std::size_t i = 0; i < text.size(); ++i)
            painter.drawText(QRectF(x + 8, y + imageHeight + 6 + static_cast<int>(i) * metrics.height(),
                boxWidth - 16, metrics.height()), ::Qt::AlignLeft | ::Qt::AlignVCenter, text[i]);
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (!session) return;
        if (event->button() == ::Qt::RightButton) { session->cancel(); return; }
        if (event->button() != ::Qt::LeftButton) return;
        setFocus(::Qt::MouseFocusReason); session->pointerPress(eventPoint(event)); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    { if (session) session->pointerMove(eventPoint(event)); event->accept(); }
    void mouseReleaseEvent(QMouseEvent* event) override
    { if (session && event->button() == ::Qt::LeftButton) session->pointerRelease(eventPoint(event), event->modifiers().testFlag(::Qt::ControlModifier)); event->accept(); }
    void mouseDoubleClickEvent(QMouseEvent* event) override
    { if (session && event->button() == ::Qt::LeftButton) session->request(Action::Clipboard); event->accept(); }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (!session) return;
        QPoint delta;
        if (event->key() == ::Qt::Key_Left) delta = {-1, 0};
        else if (event->key() == ::Qt::Key_Right) delta = {1, 0};
        else if (event->key() == ::Qt::Key_Up) delta = {0, -1};
        else if (event->key() == ::Qt::Key_Down) delta = {0, 1};
        else { QWidget::keyPressEvent(event); return; }
        session->nudgeCursor(delta); event->accept();
    }
    bool event(QEvent* event) override
    {
        if (session && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)) {
            const QPointer<Surface> lifetime(this); session->pointerCaptureLost();
            if (!lifetime) return true;
        }
        return QWidget::event(event);
    }
    void resizeEvent(QResizeEvent* event) override
    { QWidget::resizeEvent(event); if (resized) resized(); }
    void closeEvent(QCloseEvent* event) override
    { if (!shuttingDown && session) { event->ignore(); session->cancel(); } else event->accept(); }
};
}

struct CaptureWindow::State {
    CaptureWindow* owner{};
    QPointer<Capture::CaptureSession> session;
    DesktopTranslator translator;
    std::vector<std::unique_ptr<Surface>> surfaces;
    std::vector<QMetaObject::Connection> screenConnections;
    bool hiddenForAction{}, destroying{}, closingQueued{};
    explicit State(CaptureWindow* value) : owner(value) {}
    QPoint fromDesktop(QPointF global) const
    {
        if (!session) return {};
        const auto& screens = session->snapshot().screens;
        const Capture::ScreenBinding* closest{}; qreal distance = (std::numeric_limits<qreal>::max)();
        for (const auto& screen : screens) {
            if (screen.logicalGeometry.contains(global))
                return Capture::sourceFromScreenLocal(screen, global - screen.logicalGeometry.topLeft()).toPoint();
            const auto clamped = QPointF(std::clamp(global.x(), screen.logicalGeometry.left(), screen.logicalGeometry.right()),
                std::clamp(global.y(), screen.logicalGeometry.top(), screen.logicalGeometry.bottom()));
            const auto diff = global - clamped; const auto candidate = diff.x() * diff.x() + diff.y() * diff.y();
            if (candidate < distance) { distance = candidate; closest = &screen; }
        }
        return closest ? Capture::sourceFromScreenLocal(*closest, global - closest->logicalGeometry.topLeft()).toPoint() : QPoint();
    }
    void refresh()
    {
        if (!session || destroying) return;
        if (session->stage() == Stage::Closed) { for (const auto& surface : surfaces) surface->hide(); return; }
        if (session->stage() == Stage::Adjusting || session->stage() == Stage::Selecting) hiddenForAction = false;
        std::size_t toolbarIndex{};
        const auto selected = selectionRect(*session);
        qreal largest = -1;
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            const auto visible = selected.intersected(surfaces[i]->sourceViewport());
            const auto area = visible.width() * visible.height();
            if (area > largest) { toolbarIndex = i; largest = area; }
        }
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            surfaces[i]->translator = translator; surfaces[i]->refresh(i == toolbarIndex);
            if (!hiddenForAction && !surfaces[i]->isVisible()) surfaces[i]->show();
            if (hiddenForAction) surfaces[i]->hide();
        }
    }
    void clear()
    {
        for (const auto& connection : screenConnections) QObject::disconnect(connection);
        screenConnections.clear();
        for (const auto& surface : surfaces) { surface->shuttingDown = true; surface->hide(); }
        surfaces.clear();
    }
    void invalidate()
    {
        if (!session || session->stage() == Stage::Closed || destroying) return;
        const QPointer<CaptureWindow> lifetime(owner);
        const Capture::Failure failure{Capture::Failure::Code::SourceChanged, 0,
            QStringLiteral("The display layout changed. Start a fresh screenshot to use the new coordinates.")};
        session->cancel();
        if (lifetime) Q_EMIT owner->sourceInvalidated(failure);
    }
};

CaptureWindow::CaptureWindow(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this))
{ state_->session = new Capture::CaptureSession(this); connectSession(); }
CaptureWindow::CaptureWindow(Capture::CaptureSession& session, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this))
{ state_->session = &session; connectSession(); }
CaptureWindow::~CaptureWindow() { state_->destroying = true; state_->clear(); }
void CaptureWindow::connectSession()
{
    connect(state_->session, &Capture::CaptureSession::changed, this, [this] { state_->refresh(); });
    connect(state_->session, &Capture::CaptureSession::closed, this, [this] {
        for (const auto& surface : state_->surfaces) surface->hide();
        // A toolbar/mouse callback can be the caller. Remove widgets only once
        // that native event stack has returned, using this QObject as context.
        if (!state_->closingQueued) {
            state_->closingQueued = true;
            QTimer::singleShot(0, this, [this] {
                state_->closingQueued = false;
                if (state_->session && state_->session->stage() != Stage::Closed) return;
                state_->clear(); Q_EMIT closed();
            });
        }
    });
    connect(state_->session, &Capture::CaptureSession::cursorNudged, this, [this](QPoint source) {
        if (!state_->session || state_->session->snapshot().coordinates != Capture::CoordinateSpace::DesktopPixels) return;
        if (const auto origin = state_->session->snapshot().desktopOrigin)
            Platform::requestDesktopPointerMove(source + *origin);
    });
    connect(state_->session, &QObject::destroyed, this, [this] { state_->clear(); });
}
Capture::CaptureSession* CaptureWindow::session() const noexcept { return state_->session; }
Capture::Failure CaptureWindow::showSnapshot(Capture::Snapshot snapshot, Capture::StartOptions options)
{
    if (!state_->session) return {Capture::Failure::Code::Unavailable, 0, QStringLiteral("The capture session no longer exists")};
    if (state_->session->stage() == Stage::Empty || state_->session->stage() == Stage::Closed) state_->clear();
    const QPointer<CaptureWindow> lifetime(this);
    const auto loaded = state_->session->load(std::move(snapshot), options);
    if (!lifetime) return {Capture::Failure::Code::Cancelled, 0, {}};
    if (loaded) return loaded;
    state_->clear(); state_->hiddenForAction = false;
    const auto& source = state_->session->snapshot();
    if (source.coordinates == Capture::CoordinateSpace::DesktopPixels) {
        for (const auto& screen : source.screens) {
            auto surface = std::make_unique<Surface>(*state_->session, screen);
            surface->setAttribute(::Qt::WA_NativeWindow);
            surface->winId(); surface->windowHandle()->setScreen(screen.screen);
            surface->setGeometry(screen.logicalGeometry.toRect());
            surface->desktopPoint = [this](QPointF global) { return state_->fromDesktop(global); };
            surface->resized = [this] { state_->refresh(); };
            state_->surfaces.push_back(std::move(surface));
            auto invalidate = [this] { state_->invalidate(); };
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::geometryChanged, this, invalidate));
            state_->screenConnections.push_back(connect(screen.screen, &QScreen::physicalDotsPerInchChanged, this, invalidate));
            state_->screenConnections.push_back(connect(screen.screen, &QObject::destroyed, this, invalidate));
        }
        state_->screenConnections.push_back(connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { state_->invalidate(); }));
        state_->screenConnections.push_back(connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { state_->invalidate(); }));
        for (const auto& surface : state_->surfaces) surface->showFullScreen();
    } else {
        auto surface = std::make_unique<Surface>(*state_->session, std::nullopt);
        surface->setMinimumSize(320, 260); surface->resize(QSize(1100, 760).boundedTo(surface->screen()->availableGeometry().size() - QSize(40, 40)));
        surface->resized = [this] { state_->refresh(); };
        state_->surfaces.push_back(std::move(surface));
        state_->surfaces.front()->show();
    }
    state_->refresh();
    auto* active = state_->surfaces.front().get();
    for (const auto& surface : state_->surfaces)
        if (surface->sourceViewport().contains(source.cursor)) active = surface.get();
    active->activateWindow(); active->setFocus(::Qt::ActiveWindowFocusReason);
    return {};
}
void CaptureWindow::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); state_->refresh(); }
QWidget* CaptureWindow::dialogParent() const noexcept
{
    for (const auto& surface : state_->surfaces) if (surface->isActiveWindow()) return surface.get();
    return state_->surfaces.empty() ? nullptr : state_->surfaces.front().get();
}
void CaptureWindow::hide()
{ state_->hiddenForAction = true; for (const auto& surface : state_->surfaces) surface->hide(); }
void CaptureWindow::hideForAction() { hide(); }
void CaptureWindow::showAfterCancelledAction() { state_->hiddenForAction = false; state_->refresh(); }
void CaptureWindow::cancel() { if (state_->session) state_->session->cancel(); }
void CaptureWindow::shutdown() { cancel(); state_->clear(); }
bool CaptureWindow::visible() const noexcept
{ return std::any_of(state_->surfaces.begin(), state_->surfaces.end(), [](const auto& surface) { return surface->isVisible(); }); }

} // namespace HuhuCapture::UI::Qt
