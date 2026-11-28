#pragma once

#include "ui/SettingsMetrics.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <algorithm>

namespace HuhuCapture::UI::Qt::DesktopTheme {
namespace Metrics = HuhuCapture::UI::SettingsMetrics;

inline QFont font(int pixels = static_cast<int>(Metrics::bodySize), QFont::Weight weight = QFont::Normal)
{
    auto result = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    result.setPixelSize(pixels); result.setWeight(weight); result.setItalic(false);
    return result;
}

inline QColor color(std::uint32_t rgba) { return QColor::fromRgba((rgba >> 8) | (rgba << 24)); }

inline void repolish(QWidget* widget)
{ widget->style()->unpolish(widget); widget->style()->polish(widget); widget->update(); }

inline void apply(QWidget* widget)
{
    widget->setFont(font());
    auto palette = widget->palette();
    palette.setColor(QPalette::Window, color(Metrics::canvas));
    palette.setColor(QPalette::WindowText, color(Metrics::ink));
    palette.setColor(QPalette::Text, color(Metrics::ink));
    palette.setColor(QPalette::Base, color(Metrics::surface));
    widget->setPalette(palette); widget->setAutoFillBackground(true);
    widget->setStyleSheet(QStringLiteral(
        "QLabel { color: #1d1d1f; background: transparent; }"
        "QLabel[secondary=\"true\"] { color: #636366; }"
        "QLabel[error=\"true\"] { color: #b42318; }"
        "QFrame[card=\"true\"] { background: white; border: 1px solid #e0e0e0; border-radius: 12px; }"
        "QFrame[card=\"true\"][selected=\"true\"] { border-color: #80b6ed; }"
        "QPushButton, QComboBox, QKeySequenceEdit { color: #1d1d1f; background: white; border: 1px solid #e0e0e0; border-radius: 8px; padding: 0px 12px; min-height: 30px; }"
        "QPushButton:hover, QComboBox:hover { border-color: #0071e3; background: #f5f5f7; color: #0066cc; }"
        "QPushButton:focus, QComboBox:focus, QKeySequenceEdit:focus { border: 2px solid #0071e3; padding: 0px 11px; }"
        "QPushButton:disabled, QComboBox:disabled, QKeySequenceEdit:disabled { color: #8b8b91; background: #f5f5f7; }"
        "QPushButton[primary=\"true\"] { background: #0066cc; color: white; border-color: #0066cc; }"
        "QPushButton[primary=\"true\"]:hover { background: #0071e3; }"
        "QPushButton[primary=\"true\"]:disabled { background: #e0e0e0; color: #636366; border-color: #e0e0e0; }"
        "QPushButton[destructive=\"true\"]:hover { color: #b42318; background: #fff0ed; border-color: #e4aaa3; }"
        "QPushButton[link=\"true\"] { border: none; color: #0066cc; background: transparent; }"
        "QPushButton[navigation=\"true\"] { border: none; text-align: left; background: white; padding: 0px 10px; }"
        "QPushButton[navigation=\"true\"]:checked { background: #eaf3ff; color: #0066cc; }"
        "QComboBox { padding-right: 30px; }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background: white; color: #1d1d1f; selection-background-color: #eaf3ff; selection-color: #1d1d1f; border: 1px solid #e0e0e0; }"
        "QKeySequenceEdit QLineEdit { border: none; background: transparent; padding: 0; }"
        "QScrollArea { border: none; background: transparent; }"));
}

inline QLabel* label(QWidget* parent, int pixels = static_cast<int>(Metrics::bodySize), bool secondary = false)
{
    auto* result = new QLabel(parent); result->setTextFormat(::Qt::PlainText);
    result->setFont(font(pixels)); result->setProperty("secondary", secondary);
    return result;
}

inline QPushButton* button(QWidget* parent, const char* name, bool primary = false)
{
    auto* result = new QPushButton(parent); result->setObjectName(QString::fromLatin1(name));
    result->setFont(font()); result->setFixedHeight(static_cast<int>(Metrics::controlHeight));
    result->setProperty("primary", primary); result->setAutoDefault(false);
    result->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    return result;
}

inline QFrame* card(QWidget* parent)
{
    auto* result = new QFrame(parent); result->setProperty("card", true);
    return result;
}

inline QFrame* divider(QWidget* parent)
{
    auto* result = new QFrame(parent); result->setFixedHeight(1);
    result->setStyleSheet(QStringLiteral("background: #e0e0e0; border: none;")); return result;
}

inline void initialSize(QWidget* widget, QSize preferred)
{
    // Native/QPA owns placement after the host's first show. Never recenter on
    // refresh, language changes, thumbnails, or monitor/DPI notifications.
    const auto available = widget->screen()->availableGeometry().size() - QSize(32, 32);
    widget->resize(preferred.boundedTo(available.expandedTo(QSize(240, 240))));
    widget->setMinimumSize(QSize(640, 400).boundedTo(available.expandedTo(QSize(240, 240))));
}

// One real text value, elided only while painting. No filesystem access or
// altered path is fed back to the host; tooltip/accessibility retains it all.
class ElidingLabel final : public QLabel {
public:
    explicit ElidingLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setTextFormat(::Qt::PlainText); setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    void setFullText(const QString& value)
    { setText(value); setToolTip(value); setAccessibleName(value); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.setFont(font());
        painter.setPen(property("secondary").toBool() ? color(Metrics::secondary) : palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(), alignment() | ::Qt::TextSingleLine,
            fontMetrics().elidedText(text(), ::Qt::ElideRight, contentsRect().width()));
    }
};

} // namespace HuhuCapture::UI::Qt::DesktopTheme
