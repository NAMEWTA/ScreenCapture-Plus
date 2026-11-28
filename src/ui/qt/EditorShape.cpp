#include "ui/qt/EditorShape.h"

#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QTextOption>
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace HuhuCapture::UI::Qt {
namespace {

QPointF point(Editor::EditorPoint value) { return {value.x, value.y}; }
QRectF rectangle(const Editor::EditorBounds& value)
{
    return {value.left, value.top, value.right - value.left, value.bottom - value.top};
}

EditorShape::Geometry makeGeometry(EditorTool tool, const EditorToolStyle& style,
    float dpi, float zoom, QSize image, std::uint64_t number)
{
    using namespace Editor;
    using G = EditorShape::Geometry;
    switch (tool) {
    case EditorTool::Ellipse: return G(std::in_place_type<BoxModel>, BoxKind::Ellipse);
    case EditorTool::Arrow: return G(std::in_place_type<ArrowModel>, style.value * dpi * 4.f);
    case EditorTool::Number: return G(std::in_place_type<NumberModel>, number, style.value * dpi);
    case EditorTool::Text: return G(std::in_place_type<TextModel>, 6.f * dpi);
    case EditorTool::Annotation:
        return G(std::in_place_type<AnnotationModel>, number, static_cast<std::uint32_t>(image.width()),
            static_cast<std::uint32_t>(image.height()), dpi / (std::max)(.01f, zoom));
    case EditorTool::Line: return G(std::in_place_type<StrokeModel>);
    case EditorTool::Mosaic: case EditorTool::Eraser:
        if (!style.rectangleMode) return G(std::in_place_type<StrokeModel>);
        [[fallthrough]];
    default: return G(std::in_place_type<BoxModel>);
    }
}

struct PainterState {
    QPainter& painter;
    explicit PainterState(QPainter& value) : painter(value) { painter.save(); }
    ~PainterState() { painter.restore(); }
};

} // namespace

EditorShape::EditorShape(EditorTool tool, EditorToolStyle style, float dpi, float zoom,
    QSize imageSize, std::uint64_t number, QFont font)
    : tool_(tool), style_(normalizedToolStyle(tool, style)), creationDpi_(dpi), imageSize_(imageSize),
      geometry_(makeGeometry(tool, style_, dpi, zoom, imageSize, number)), font_(std::move(font)),
      metrics_(1, 1, QImage::Format_ARGB32_Premultiplied)
{
    style_.value *= dpi;
    metrics_.setDotsPerMeterX(3780);
    metrics_.setDotsPerMeterY(3780);
    if (auto* text = textModel()) text->setStyle({style_.value, style_.rgba, style_.bold, style_.italic});
    refreshTextLayout();
}

EditorShape::~EditorShape() = default;

const Editor::AnnotationModel* EditorShape::annotationModel() const noexcept
{ return std::get_if<Editor::AnnotationModel>(&geometry_); }
Editor::AnnotationModel* EditorShape::annotationModel() noexcept
{ return std::get_if<Editor::AnnotationModel>(&geometry_); }
std::optional<std::uint64_t> EditorShape::ordinaryNumber() const noexcept
{
    if (const auto* model = std::get_if<Editor::NumberModel>(&geometry_)) return model->number();
    return std::nullopt;
}
Editor::TextModel* EditorShape::textModel() noexcept { return std::get_if<Editor::TextModel>(&geometry_); }
const Editor::TextModel* EditorShape::textModel() const noexcept { return std::get_if<Editor::TextModel>(&geometry_); }
bool EditorShape::validWithoutDrag() const noexcept
{ return tool_ == EditorTool::Text || tool_ == EditorTool::Number; }
bool EditorShape::hasValidRegion() const noexcept
{ const auto* model = annotationModel(); return !model || model->hasValidRegion(); }

int EditorShape::hitTest(Editor::EditorPoint position, float dpi, float zoom) const
{
    const float handles = 6.f * creationDpi_;
    return std::visit([&](const auto& model) -> int {
        using T = std::decay_t<decltype(model)>;
        if constexpr (std::is_same_v<T, Editor::BoxModel>) {
            const bool filled = style_.filled || tool_ == EditorTool::Mosaic || tool_ == EditorTool::Eraser;
            return static_cast<int>(model.hitTest(position, handles, filled ? 2.f * dpi : style_.value / 2.f + dpi));
        } else if constexpr (std::is_same_v<T, Editor::ArrowModel>) {
            const auto handle = model.hitHandles(position, handles);
            if (handle != Editor::PathHandle::None) return static_cast<int>(handle);
            return path_.contains(point(position)) ? static_cast<int>(Editor::PathHandle::Move) : -1;
        } else if constexpr (std::is_same_v<T, Editor::NumberModel>)
            return static_cast<int>(model.hitTest(position, handles));
        else if constexpr (std::is_same_v<T, Editor::StrokeModel>)
            return static_cast<int>(model.hitTest(position, handles, style_.value, dpi));
        else if constexpr (std::is_same_v<T, Editor::TextModel>)
            return static_cast<int>(model.hitTest(position, dpi));
        else
            return static_cast<int>(model.hitTest(position, 4.f * dpi / (std::max)(.01f, zoom), static_cast<float>(badgeRadius_)));
    }, geometry_);
}

void EditorShape::beginGesture(Editor::EditorPoint position, int handle, bool shift)
{
    std::visit([&](auto& model) {
        using T = std::decay_t<decltype(model)>;
        if constexpr (std::is_same_v<T, Editor::BoxModel> || std::is_same_v<T, Editor::AnnotationModel>)
            model.beginGesture(position, static_cast<Editor::EditorHandle>(handle));
        else if constexpr (std::is_same_v<T, Editor::ArrowModel>)
            model.beginGesture(position, static_cast<Editor::PathHandle>(handle));
        else if constexpr (std::is_same_v<T, Editor::NumberModel>)
            model.beginGesture(position, static_cast<Editor::NumberHandle>(handle));
        else if constexpr (std::is_same_v<T, Editor::StrokeModel>)
            model.beginGesture(position, shift, static_cast<Editor::PathHandle>(handle));
        else {
            if (!model.placed()) model.place(position);
            if (handle == static_cast<int>(Editor::TextHandle::Move)) model.beginMove(position);
        }
    }, geometry_);
    rebuildPath();
}

void EditorShape::drag(Editor::EditorPoint position, bool shift, float dpi)
{
    std::visit([&](auto& model) {
        using T = std::decay_t<decltype(model)>;
        if constexpr (std::is_same_v<T, Editor::BoxModel> || std::is_same_v<T, Editor::ArrowModel> ||
                      std::is_same_v<T, Editor::StrokeModel>) model.drag(position, shift);
        else if constexpr (std::is_same_v<T, Editor::NumberModel>) {
            if (const auto requested = model.drag(position, 8.f * dpi)) {
                const auto* range = Editor::findToolSlider(L"number");
                style_.value = std::clamp(*requested, range->min * dpi, range->max * dpi);
                model.setRadius(style_.value);
                refreshTextLayout();
            }
        } else model.drag(position);
    }, geometry_);
    if (tool_ == EditorTool::Mosaic || tool_ == EditorTool::Eraser) {
        effectReady_ = false;
        mosaic_ = {};
    }
    rebuildPath();
}

void EditorShape::finishGesture()
{
    std::visit([](auto& model) { model.finishGesture(); }, geometry_);
    if (tool_ == EditorTool::Eraser) effectReady_ = !effectPath().isEmpty();
}

void EditorShape::setPixelValue(float value)
{
    if (!std::isfinite(value) || value <= 0) return;
    style_.value = value;
    if (auto* arrow = std::get_if<Editor::ArrowModel>(&geometry_)) arrow->setSize(value * 4.f);
    if (auto* number = std::get_if<Editor::NumberModel>(&geometry_)) number->setRadius(value);
    rebuildPath();
    refreshTextLayout();
}

void EditorShape::setTextStyle(EditorToolStyle logicalStyle, float dpi)
{
    if (auto* text = textModel()) {
        style_ = normalizedToolStyle(EditorTool::Text, logicalStyle);
        style_.value *= dpi;
        text->setStyle({style_.value, style_.rgba, style_.bold, style_.italic});
        refreshTextLayout();
    }
}

void EditorShape::setFont(QFont font) { font_ = std::move(font); refreshTextLayout(); }

bool EditorShape::makeLabel(const QString& text, qreal pixels, bool bold, qreal lineWidth)
{
    if (metrics_.isNull() || !std::isfinite(pixels) || pixels <= 0) return labelValid_ = false;
    auto font = font_;
    font.setPointSizeF(pixels * 72.0 / 96.0);
    font.setWeight(bold ? (tool_ == EditorTool::Text ? QFont::Bold : QFont::DemiBold) : QFont::Normal);
    font.setItalic(tool_ == EditorTool::Text && style_.italic);
    const QFontInfo info(font);
    if (info.family().trimmed().isEmpty() || info.pixelSize() <= 0) return labelValid_ = false;
    const QFontMetricsF metrics(font, &metrics_);
    QString normalized(text);
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar(u'\r'), QChar(u'\n'));
    const auto paragraphs = normalized.split(QChar(u'\n'), ::Qt::KeepEmptyParts);
    std::vector<TextLine> next;
    qreal top = 0, width = 0;
    for (const auto& paragraph : paragraphs) {
        auto layout = std::make_unique<QTextLayout>(paragraph, font, &metrics_);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        option.setFlags(QTextOption::IncludeTrailingSpaces);
        option.setTextDirection(::Qt::LayoutDirectionAuto);
        layout->setTextOption(option);
        layout->setCacheEnabled(true);
        layout->beginLayout();
        qreal height = 0;
        for (;;) {
            auto line = layout->createLine();
            if (!line.isValid()) break;
            line.setLeadingIncluded(true);
            line.setLineWidth(lineWidth);
            line.setPosition({0, height});
            width = (std::max)(width, line.naturalTextWidth());
            height += line.height();
        }
        layout->endLayout();
        for (const auto& run : layout->glyphRuns()) {
            const auto raw = run.rawFont();
            if (!raw.isValid() || raw.familyName().trimmed().isEmpty() || raw.pixelSize() <= 0)
                return labelValid_ = false;
            for (const auto glyph : run.glyphIndexes()) if (glyph == 0) return labelValid_ = false;
        }
        next.push_back({std::move(layout), {0, top}});
        top += (std::max)(height, metrics.height());
    }
    if (!std::isfinite(width) || !std::isfinite(top)) return labelValid_ = false;
    label_ = std::move(next);
    labelSize_ = {width, top};
    return labelValid_ = true;
}

bool EditorShape::refreshTextLayout()
{
    if (auto* text = textModel()) {
        if (!makeLabel(QString::fromStdWString(text->text()), text->style().fontSize, text->style().bold)) return false;
        if (text->placed()) {
            const auto bounds = text->bounds();
            text->setLayoutBounds({bounds.left, bounds.top,
                bounds.left + static_cast<float>(labelSize_.width()) + text->padding() * 2,
                bounds.top + static_cast<float>(labelSize_.height()) + text->padding() * 2});
        }
    } else if (const auto* number = std::get_if<Editor::NumberModel>(&geometry_)) {
        return makeLabel(QString::number(static_cast<qulonglong>(number->number())), number->radius(), false);
    } else if (const auto* annotation = annotationModel()) {
        const qreal unit = annotation->creationScale();
        const auto label = QString::number(static_cast<qulonglong>(annotation->number()));
        if (!makeLabel(label, 13 * unit, true)) return false;
        badgeRadius_ = (std::max)(12 * unit, std::ceil((std::max)(labelSize_.width() + 8 * unit,
            labelSize_.height() + 2 * unit) / 2));
        const qreal imageRadius = (std::min)(imageSize_.width(), imageSize_.height()) / 2.0;
        if (imageRadius > 0 && imageRadius < badgeRadius_) {
            if (!makeLabel(label, 13 * unit * imageRadius / badgeRadius_, true)) return false;
            badgeRadius_ = imageRadius;
        }
    }
    return true;
}

void EditorShape::rebuildPath()
{
    path_ = {};
    if (const auto* arrow = std::get_if<Editor::ArrowModel>(&geometry_)) {
        const auto polygon = arrow->polygon();
        if (polygon.empty()) return;
        path_.moveTo(point(polygon.front()));
        for (const auto p : polygon.subspan(1)) path_.lineTo(point(p));
        path_.closeSubpath();
    } else if (const auto* stroke = std::get_if<Editor::StrokeModel>(&geometry_)) {
        const auto points = stroke->points();
        if (points.empty()) return;
        path_.moveTo(point(points.front()));
        for (const auto p : points.subspan(1)) path_.lineTo(point(p));
    } else if (const auto* number = std::get_if<Editor::NumberModel>(&geometry_)) {
        if (!number->placed()) return;
        const auto& outline = number->outline();
        const auto center = point(number->center());
        const qreal radius = number->radius();
        const QRectF circle(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
        path_.moveTo(point(outline.start));
        path_.arcTo(circle, number->angle() + 10, 170);
        path_.arcTo(circle, number->angle() + 180, 170);
        path_.lineTo(point(outline.tip));
        path_.closeSubpath();
    }
}

QPainterPath EditorShape::effectPath() const
{
    if (const auto* box = std::get_if<Editor::BoxModel>(&geometry_)) {
        QPainterPath result;
        if (box->placed()) result.addRect(rectangle(box->bounds()));
        return result;
    }
    QPainterPathStroker stroke;
    stroke.setWidth(style_.value);
    stroke.setCapStyle(::Qt::RoundCap);
    stroke.setJoinStyle(::Qt::RoundJoin);
    return stroke.createStroke(path_);
}

QRectF EditorShape::effectBounds() const { return effectPath().boundingRect(); }
void EditorShape::setMosaic(QImage image, QPoint origin)
{
    mosaic_ = std::move(image);
    mosaicOrigin_ = origin;
    effectReady_ = !mosaic_.isNull();
}

void EditorShape::drawLabel(QPainter& painter, QPointF position) const
{
    for (const auto& paragraph : label_) paragraph.layout->draw(&painter, position + paragraph.position);
}

bool EditorShape::paint(QPainter& painter, const QImage& original, bool exporting) const
{
    if (!painter.isActive()) return false;
    if ((tool_ == EditorTool::Text || tool_ == EditorTool::Number || tool_ == EditorTool::Annotation) && !labelValid_) return false;
    const PainterState saved(painter);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const auto color = editorColor(style_.rgba);
    if (tool_ == EditorTool::Mosaic || tool_ == EditorTool::Eraser) {
        const auto region = effectPath();
        if (!region.boundingRect().intersects(QRectF(0, 0, original.width(), original.height()))) return true;
        if (!effectReady_) {
            if (exporting) return false;
            // Existing live gesture feedback; it is never admitted to export.
            painter.fillPath(region, QColor::fromRgbF(240.0 / 255, 0, 240.0 / 255, .38));
        } else {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.setClipPath(region, ::Qt::IntersectClip);
            // An image blit preserves source bytes in fully covered pixels;
            // sampling an ARGB texture brush would round-trip straight alpha
            // through premultiplication even for an unchanged eraser interior.
            if (tool_ == EditorTool::Eraser) painter.drawImage(QPoint(), original);
            else painter.drawImage(mosaicOrigin_, mosaic_);
        }
    } else if (const auto* box = std::get_if<Editor::BoxModel>(&geometry_)) {
        painter.setPen(style_.filled ? QPen(::Qt::NoPen) : QPen(color, style_.value));
        painter.setBrush(style_.filled ? QBrush(color) : QBrush(::Qt::NoBrush));
        if (tool_ == EditorTool::Ellipse) painter.drawEllipse(rectangle(box->bounds()));
        else painter.drawRect(rectangle(box->bounds()));
    } else if (tool_ == EditorTool::Arrow) {
        painter.setPen(style_.filled ? QPen(::Qt::NoPen) : QPen(color, creationDpi_));
        painter.setBrush(style_.filled ? QBrush(color) : QBrush(::Qt::NoBrush));
        painter.drawPath(path_);
    } else if (const auto* number = std::get_if<Editor::NumberModel>(&geometry_)) {
        painter.setPen(style_.filled ? QPen(::Qt::NoPen) : QPen(color, creationDpi_));
        painter.setBrush(style_.filled ? QBrush(color) : QBrush(::Qt::NoBrush));
        painter.drawPath(path_);
        painter.setPen(style_.filled ? QColor(::Qt::white) : color);
        const auto center = point(number->center());
        drawLabel(painter, center - QPointF(labelSize_.width() / 2, labelSize_.height() / 2));
    } else if (tool_ == EditorTool::Line) {
        auto penColor = color;
        if (style_.semiTransparent) penColor.setAlphaF(penColor.alphaF() * .5);
        painter.setPen(QPen(penColor, style_.value, ::Qt::SolidLine, ::Qt::RoundCap, ::Qt::RoundJoin));
        painter.setBrush(::Qt::NoBrush);
        painter.drawPath(path_);
    } else if (const auto* text = textModel()) {
        if (!text->isEditing() || exporting) {
            painter.setPen(editorColor(text->style().rgba));
            drawLabel(painter, {text->bounds().left + text->padding(), text->bounds().top + text->padding()});
        }
    } else if (const auto* annotation = annotationModel()) {
        const auto bounds = rectangle(annotation->bounds());
        if (bounds.isEmpty()) return true;
        const auto palette = annotation->color();
        const auto fill = QColor::fromRgb(palette.backgroundRgb);
        const qreal unit = annotation->creationScale();
        const qreal stroke = (std::min)(2 * unit, (std::min)(bounds.width(), bounds.height()));
        painter.setPen(QPen(fill, stroke));
        painter.setBrush(::Qt::NoBrush);
        painter.drawRect(bounds.adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2));
        if (!annotation->hasValidRegion()) return true;
        const auto center = point(annotation->badgeCenter(static_cast<float>(badgeRadius_)));
        painter.setPen(::Qt::NoPen);
        painter.setBrush(fill);
        painter.drawEllipse(center, badgeRadius_, badgeRadius_);
        const qreal outline = (std::min)(unit, badgeRadius_ / 3);
        painter.setBrush(::Qt::NoBrush);
        painter.setPen(QPen(::Qt::white, outline));
        painter.drawEllipse(center, badgeRadius_ - outline / 2, badgeRadius_ - outline / 2);
        painter.setPen(QColor::fromRgb(palette.textRgb));
        drawLabel(painter, center - QPointF(labelSize_.width() / 2, labelSize_.height() / 2));
    }
    return true;
}

void EditorShape::paintHandles(QPainter& painter, float dpi, float zoom) const
{
    const PainterState saved(painter);
    painter.setBrush(::Qt::NoBrush);
    painter.setPen(QPen(::Qt::black, dpi));
    const auto draw = [&](const auto& bounds) { painter.drawRect(rectangle(bounds)); };
    std::visit([&](const auto& model) {
        using T = std::decay_t<decltype(model)>;
        if constexpr (std::is_same_v<T, Editor::AnnotationModel>) {
            if (!model.hasValidRegion() || model.creating()) return;
            const float unit = dpi / (std::max)(.01f, zoom);
            painter.setPen(QPen(QColor::fromRgb(model.color().backgroundRgb), unit));
            painter.setBrush(::Qt::white);
            for (const auto center : Core::annotationRegionHandles(model.bounds()))
                draw(Editor::editorHandleBounds(center, 6 * unit));
        } else if constexpr (std::is_same_v<T, Editor::TextModel>) {
            auto pen = QPen(editorColor(model.style().rgba), dpi);
            pen.setDashPattern({2, 2});
            painter.setPen(pen);
            draw(model.bounds());
        } else {
            for (const auto& bounds : model.handles(6.f * creationDpi_)) draw(bounds);
        }
    }, geometry_);
}

::Qt::CursorShape EditorShape::cursorForHandle(int handle) const noexcept
{
    if (tool_ == EditorTool::Text) {
        if (handle == static_cast<int>(Editor::TextHandle::Edit)) return ::Qt::IBeamCursor;
        if (handle == static_cast<int>(Editor::TextHandle::Move)) return ::Qt::SizeAllCursor;
        return ::Qt::ArrowCursor;
    }
    if (std::holds_alternative<Editor::BoxModel>(geometry_) || annotationModel()) {
        switch (static_cast<Editor::EditorHandle>(handle)) {
        case Editor::EditorHandle::TopLeft: case Editor::EditorHandle::BottomRight: return ::Qt::SizeFDiagCursor;
        case Editor::EditorHandle::TopRight: case Editor::EditorHandle::BottomLeft: return ::Qt::SizeBDiagCursor;
        case Editor::EditorHandle::Top: case Editor::EditorHandle::Bottom: return ::Qt::SizeVerCursor;
        case Editor::EditorHandle::Left: case Editor::EditorHandle::Right: return ::Qt::SizeHorCursor;
        default: break;
        }
    }
    return handle >= 0 ? ::Qt::SizeAllCursor : ::Qt::CrossCursor;
}

} // namespace HuhuCapture::UI::Qt
