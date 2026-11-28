#pragma once

#include "ui/toolbars/ToolbarMetrics.h"

#include <QColor>
#include <QFontDatabase>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QResource>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include <utility>
#include <vector>

// Keep the generated qrc symbol reachable in static UI libraries. This global
// helper must remain outside the application namespace for Q_INIT_RESOURCE.
inline void initializeHuhuUiResources() { Q_INIT_RESOURCE(huhucapture_ui); }

namespace HuhuCapture::UI::Qt::ToolbarControls {
inline constexpr int buttonSize = static_cast<int>(UI::toolbarButtonSize);
inline constexpr int iconSize = static_cast<int>(UI::toolbarIconSize);

inline QString iconFontFamily()
{
    static const QString family = [] {
        initializeHuhuUiResources();
        const auto id = QFontDatabase::addApplicationFont(QStringLiteral(":/huhucapture/editor/iconfont.ttf"));
        const auto families = id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QString() : families.front();
    }();
    return family;
}

// Existing iconfont glyphs and the existing annotation path, rendered at the
// requested Qt icon/device scale. No per-platform asset or hard-coded DPR.
class EditorIcon final : public QIconEngine {
public:
    EditorIcon(QString family, char16_t glyph, bool annotation = false)
        : family_(std::move(family)), glyph_(glyph), annotation_(annotation) {}
    explicit EditorIcon(QColor color) : color_(color) {}
    QIconEngine* clone() const override { return new EditorIcon(*this); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->save();
        const auto side = (std::min)(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - side) / 2.0, rect.y() + (rect.height() - side) / 2.0);
        painter->scale(side / 24.0, side / 24.0);
        painter->setRenderHint(QPainter::Antialiasing);
        const QColor ink = mode == QIcon::Disabled ? QColor(150, 150, 150) : QColor(51, 51, 51);
        if (color_.isValid()) {
            painter->setPen(color_ == QColor(::Qt::white) ? QPen(QColor(168, 168, 168), 1) : QPen(::Qt::NoPen));
            painter->setBrush(color_);
            painter->drawRoundedRect(QRectF(.5, .5, 23, 23), 2, 2);
            if (state == QIcon::On) {
                painter->setPen(QPen(color_ == QColor(::Qt::white) ? ::Qt::black : ::Qt::white, 1.8, ::Qt::SolidLine, ::Qt::RoundCap, ::Qt::RoundJoin));
                QPainterPath check; check.moveTo(6, 12); check.lineTo(10, 16); check.lineTo(18, 8);
                painter->drawPath(check);
            }
        } else if (annotation_) {
            painter->setBrush(::Qt::NoBrush);
            painter->setPen(QPen(ink, 1.6, ::Qt::SolidLine, ::Qt::SquareCap, ::Qt::RoundJoin));
            QPainterPath p;
            p.moveTo(5, 3); p.lineTo(19, 3); p.cubicTo(22, 3, 22, 5, 22, 6);
            p.lineTo(22, 14); p.cubicTo(22, 17, 20, 17, 19, 17);
            p.lineTo(11, 17); p.lineTo(7, 21); p.lineTo(7, 17); p.lineTo(5, 17);
            p.cubicTo(2, 17, 2, 15, 2, 14); p.lineTo(2, 6); p.cubicTo(2, 3, 4, 3, 5, 3); p.closeSubpath();
            painter->drawPath(p);
            painter->drawLine(QPointF(7, 8), QPointF(9, 6)); painter->drawLine(QPointF(9, 6), QPointF(9, 13));
            painter->drawLine(QPointF(7, 13), QPointF(11, 13));
            painter->drawLine(QPointF(14, 7), QPointF(18, 7)); painter->drawLine(QPointF(14, 11), QPointF(18, 11));
        } else {
            QFont font(family_); font.setPixelSize(24);
            painter->setFont(font); painter->setPen(ink);
            painter->drawText(QRectF(0, 0, 24, 24), ::Qt::AlignCenter, QString(QChar(glyph_)));
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size); result.fill(::Qt::transparent);
        QPainter painter(&result); paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    QString family_;
    char16_t glyph_{};
    bool annotation_{};
    QColor color_;
};

class ToolbarStrip final : public QWidget {
public:
    explicit ToolbarStrip(QWidget* parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setStyleSheet(QStringLiteral("ToolbarStrip { background: white; } QToolButton { border: 0; border-radius: 0; padding: 0; background: transparent; color: #333333; } QToolButton:hover { background: #f2f2f2; } QToolButton:checked { background: #e6f4ff; } QToolButton:disabled { color: #969696; }"));
    }
    void add(QWidget* widget, int width = buttonSize)
    {
        widget->setParent(this);
        controls_.push_back(widget);
        widths_.push_back(static_cast<float>(width));
        widget->show();
        arrange();
    }
    void clear()
    {
        for (auto* control : controls_) delete control;
        controls_.clear(); widths_.clear(); arrange();
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    { return qCeil(UI::measureToolbar(visibleWidths(), static_cast<float>(width)).height); }
    QSize sizeHint() const override
    {
        const auto extent = UI::measureToolbar(visibleWidths());
        return {qCeil(extent.width), qCeil(extent.height)};
    }
    QSize minimumSizeHint() const override { return {buttonSize + 2, heightForWidth(width())}; }
    void refreshTextWidths()
    {
        for (std::size_t i = 0; i < controls_.size(); ++i) {
            if (auto* button = qobject_cast<QToolButton*>(controls_[i]); button && button->icon().isNull())
                widths_[i] = static_cast<float>((std::max)(buttonSize, button->sizeHint().width() + 16));
        }
        arrange();
    }
protected:
    void resizeEvent(QResizeEvent*) override { arrange(); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), ::Qt::white);
        painter.setPen(QColor(168, 168, 168)); painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
private:
    std::vector<float> visibleWidths() const
    {
        std::vector<float> result; result.reserve(widths_.size());
        for (std::size_t i = 0; i < controls_.size(); ++i)
            if (!controls_[i]->isHidden()) result.push_back(widths_[i]);
        return result;
    }
    void arrange()
    {
        if (controls_.empty()) { setFixedHeight(0); return; }
        const int limit = (std::max)(buttonSize, width() - 2);
        int x = 1, y = 1;
        for (std::size_t i = 0; i < controls_.size(); ++i) {
            if (controls_[i]->isHidden()) continue;
            const int w = qCeil(widths_[i]);
            if (x > 1 && x + w > limit + 1) { x = 1; y += buttonSize; }
            controls_[i]->setGeometry(x, y, w, buttonSize);
            x += w;
        }
        const int nextHeight = heightForWidth(width());
        if (height() != nextHeight) setFixedHeight(nextHeight);
        updateGeometry(); update();
    }
    std::vector<QWidget*> controls_;
    std::vector<float> widths_;
};

class ToolbarDivider final : public QWidget {
public:
    explicit ToolbarDivider(QWidget* parent) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(QRectF(0, (height() - UI::toolbarDividerHeight) / 2,
            UI::toolbarDivider, UI::toolbarDividerHeight), QColor(220, 220, 220));
    }
};

// Use the active Qt style's toolbar grip instead of another icon asset. The
// owning window handles the press with QWindow::startSystemMove(), so Wayland
// retains its actual input serial and the compositor owns cross-screen moves.
class ToolbarDragHandle final : public QWidget {
public:
    explicit ToolbarDragHandle(QWidget* parent) : QWidget(parent)
    { setCursor(::Qt::SizeAllCursor); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOption option; option.initFrom(this); option.state |= QStyle::State_Horizontal;
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_IndicatorToolBarHandle, &option, &painter, this);
    }
};

inline QToolButton* makeButton(QWidget* parent, const QString& key, char16_t glyph,
    const QString& family, bool annotation = false)
{
    auto* button = new QToolButton(parent);
    button->setProperty("languageKey", key);
    button->setObjectName(key);
    button->setFocusPolicy(::Qt::NoFocus);
    button->setIconSize({iconSize, iconSize});
    if (annotation || !family.isEmpty()) button->setIcon(QIcon(new EditorIcon(family, glyph, annotation)));
    else button->setToolButtonStyle(::Qt::ToolButtonTextOnly); // Readable localized fallback, not empty boxes.
    return button;
}

} // namespace HuhuCapture::UI::Qt::ToolbarControls
