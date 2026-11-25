#include "services/rendering/QtAnnotationRenderer.h"

#include "core/PixelBuffer.h"

#include <QColorSpace>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace HuhuCapture::QtAnnotationRenderer {

struct Plan::Data {
    // Keep this device at a stable address and alive longer than the layouts.
    QImage metricsDevice;
    QPointer<QThread> owner;
    std::size_t maximumBytes{};
    std::uint32_t maximumDimension{};
    Core::AnnotationLayout layout;
    std::vector<PreparedNote> notes;
    PreparedText title;
};

namespace {

constexpr int dotsPerMeter96Dpi = 3780;

bool finiteNonnegative(qreal value) noexcept
{
    return std::isfinite(value) && value >= 0;
}

bool validBounds(const QRectF& bounds) noexcept
{
    return std::isfinite(bounds.left()) && std::isfinite(bounds.top()) &&
        std::isfinite(bounds.right()) && std::isfinite(bounds.bottom()) &&
        finiteNonnegative(bounds.width()) && finiteNonnegative(bounds.height());
}

QFont textFont(const QFont& base, qreal pixels, const QImage& device)
{
    QFont result = base;
    result.setPointSizeF(pixels * 72.0 / device.logicalDpiY());
    result.setWeight(QFont::Normal);
    result.setItalic(false);
    result.setUnderline(false);
    result.setOverline(false);
    result.setStrikeOut(false);
    // An exported image has no fixed subpixel order. Keep font fallback enabled
    // for mixed scripts and avoid screen-specific colored antialiasing fringes.
    result.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    return result;
}

struct TextMetrics {
    QRectF ink;
    qreal logicalHeight{};
    qreal logicalWidth{};
    bool hasInk{};
};

Status shapeText(const QString& original, const QFont& font, const QImage& device,
    qreal width, bool wrap, PreparedText& result, TextMetrics& metrics)
{
    if (original.size() > (std::numeric_limits<int>::max)() ||
        !std::isfinite(width) || width <= 0)
        return Status::InvalidInput;

    QString paragraphs = original;
    paragraphs.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    paragraphs.replace(QChar('\r'), QChar('\n'));
    paragraphs.replace(QChar(0x2029), QChar('\n'));
    const auto parts = paragraphs.split(QChar('\n'), Qt::KeepEmptyParts);
    result.paragraphs.clear();
    result.paragraphs.reserve(static_cast<std::size_t>(parts.size()));
    result.lineCount = 0;
    metrics = {};

    QTextOption option;
    option.setAlignment(Qt::AlignLeading);
    option.setTextDirection(Qt::LayoutDirectionAuto);
    option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);

    for (const auto& part : parts) {
        auto paragraph = std::make_unique<QTextLayout>(part, font, &device);
        paragraph->setTextOption(option);
        paragraph->setCacheEnabled(true);
        paragraph->beginLayout();
        qreal height = 0;
        for (;;) {
            auto line = paragraph->createLine();
            if (!line.isValid()) break;
            line.setLeadingIncluded(true);
            line.setLineWidth(width);
            line.setPosition(QPointF(0, height));
            const qreal lineHeight = line.height();
            if (!std::isfinite(lineHeight) || lineHeight <= 0) {
                paragraph->endLayout();
                return Status::RenderFailure;
            }
            height += lineHeight;
            metrics.logicalWidth = (std::max)(metrics.logicalWidth, line.naturalTextWidth());
            ++result.lineCount;
        }
        paragraph->endLayout();
        // Empty paragraphs are observable newlines, including a final newline.
        // Some font engines do not create a QTextLine for an empty string.
        if (paragraph->lineCount() == 0) {
            height = QFontMetricsF(font, &device).height();
            ++result.lineCount;
        }
        if (!std::isfinite(height) || height <= 0 ||
            !finiteNonnegative(metrics.logicalWidth))
            return Status::RenderFailure;

        const QPointF position(0, metrics.logicalHeight);
        // QTextLayout's glyph-run rectangle can be a selection rectangle.
        // Qt 6.12.1 qglyphrun.cpp documents that a null explicit rectangle asks
        // boundingRect() to recompute actual glyph ink using the raw font. This
        // includes combining marks and fallback-font overhang, not just advances.
        for (auto run : paragraph->glyphRuns()) {
            run.setBoundingRect(QRectF());
            const auto ink = run.boundingRect().translated(position);
            if (!validBounds(ink)) return Status::RenderFailure;
            if (ink.isEmpty()) continue;
            metrics.ink = metrics.hasInk ? metrics.ink.united(ink) : ink;
            metrics.hasInk = true;
        }
        metrics.logicalHeight += height;
        if (!std::isfinite(metrics.logicalHeight)) return Status::LayoutOverflow;
        result.paragraphs.push_back({std::move(paragraph), position});
    }
    result.inkBounds = metrics.ink;
    return Status::Ok;
}

Status measureWrapped(const QString& text, const QFont& base, const QImage& device,
    qreal pixels, qreal width, PreparedText& output)
{
    if (!std::isfinite(pixels) || pixels <= 0 || !std::isfinite(width) || width <= 8)
        return Status::InvalidInput;
    PreparedText candidate;
    candidate.text = text;
    candidate.fontSize = pixels;
    candidate.width = width;
    const auto font = textFont(base, pixels, device);
    qreal left = 4, right = 4;
    for (;;) {
        const qreal contentWidth = width - left - right;
        if (contentWidth <= 0) return Status::LayoutOverflow;
        TextMetrics metrics;
        const auto status = shapeText(text, font, device, contentWidth, true, candidate, metrics);
        if (status != Status::Ok) return status;
        const qreal nextLeft = metrics.hasInk ? (std::max)(left, std::ceil(2 - metrics.ink.left())) : left;
        const qreal nextRight = metrics.hasInk
            ? (std::max)(right, std::ceil(metrics.ink.right() - contentWidth + 2)) : right;
        if (nextLeft > left || nextRight > right) {
            // Insets only grow by whole pixels; the available width strictly
            // decreases. An unfit glyph fails instead of being clipped.
            left = nextLeft;
            right = nextRight;
            continue;
        }
        const qreal top = metrics.hasInk ? (std::max)(qreal(0), -metrics.ink.top()) : 0;
        const qreal bottom = metrics.hasInk
            ? (std::max)(qreal(0), metrics.ink.bottom() - metrics.logicalHeight) : 0;
        candidate.inset = QPointF(left, 4 + top);
        candidate.height = metrics.logicalHeight + candidate.inset.y() + 4 + bottom;
        if (!std::isfinite(candidate.height) || candidate.height > (std::numeric_limits<float>::max)())
            return Status::LayoutOverflow;
        output = std::move(candidate);
        return Status::Ok;
    }
}

Status measureBadge(std::uint64_t number, const QFont& base, const QImage& device,
    qreal pixels, PreparedText& output)
{
    PreparedText candidate;
    candidate.text = QString::number(static_cast<qulonglong>(number));
    candidate.fontSize = pixels;
    TextMetrics metrics;
    // NoWrap keeps the complete identifier; a tiny logical line width does not
    // clip or elide it and avoids passing an unbounded coordinate to Qt.
    const auto status = shapeText(candidate.text, textFont(base, pixels, device), device,
        1, false, candidate, metrics);
    if (status != Status::Ok) return status;
    QRectF bounds(0, 0, metrics.logicalWidth, metrics.logicalHeight);
    if (metrics.hasInk) bounds = bounds.united(metrics.ink);
    candidate.width = bounds.width();
    candidate.height = bounds.height();
    candidate.inset = -bounds.topLeft();
    output = std::move(candidate);
    return Status::Ok;
}

bool supportedFormat(QImage::Format format) noexcept
{
    switch (format) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return true;
    default:
        return false;
    }
}

void drawText(QPainter& painter, const PreparedText& text, QPointF position)
{
    for (const auto& paragraph : text.paragraphs)
        paragraph.layout->draw(&painter, position + text.inset + paragraph.position);
}

class PainterState {
public:
    explicit PainterState(QPainter& painter) : painter_(painter) { painter_.save(); }
    ~PainterState() { painter_.restore(); }
    PainterState(const PainterState&) = delete;
    PainterState& operator=(const PainterState&) = delete;
private:
    QPainter& painter_;
};

} // namespace

Plan::Plan() noexcept = default;
Plan::~Plan() = default;
Plan::Plan(Plan&&) noexcept = default;
Plan& Plan::operator=(Plan&&) noexcept = default;

bool Plan::isValid() const noexcept { return data_ != nullptr; }

const Core::AnnotationLayout& Plan::layout() const noexcept
{
    static const Core::AnnotationLayout empty;
    return data_ ? data_->layout : empty;
}

std::span<const PreparedNote> Plan::notes() const noexcept
{
    return data_ ? std::span<const PreparedNote>(data_->notes) : std::span<const PreparedNote>();
}

const PreparedText& Plan::title() const noexcept
{
    static const PreparedText empty;
    return data_ ? data_->title : empty;
}

Status makePlan(QSize imageSize, std::span<const Core::AnnotationNote> notes,
    const Options& options, Plan& output) noexcept
{
    if (imageSize.width() <= 0 || imageSize.height() <= 0) return Status::InvalidInput;
    try {
        auto candidate = std::make_unique<Plan::Data>();
        auto geometry = static_cast<Core::AnnotationLayoutOptions>(options);
        candidate->maximumBytes = options.maximumBytes;
        candidate->maximumDimension = options.maximumDimension;
        const auto imageWidth = static_cast<std::uint32_t>(imageSize.width());
        const auto imageHeight = static_cast<std::uint32_t>(imageSize.height());
        // Validate the source allocation before asking any font engine to work.
        const auto imageOnly = Core::layoutAnnotations(imageWidth, imageHeight, 0, geometry,
            [](std::size_t, float, float) -> std::optional<float> { return 0; });
        if (!imageOnly) return Status::LayoutOverflow;
        if (notes.empty()) {
            candidate->layout = *imageOnly;
            output.data_ = std::move(candidate);
            return Status::Ok;
        }
        if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ||
            geometry.panelWidth == 0 || geometry.panelWidth > geometry.maximumDimension - imageWidth ||
            !finiteNonnegative(geometry.preferredFontSize) || geometry.preferredFontSize <= 0 ||
            !finiteNonnegative(geometry.minimumFontSize) || geometry.minimumFontSize <= 0 ||
            geometry.minimumFontSize > geometry.preferredFontSize ||
            !finiteNonnegative(geometry.padding) ||
            !finiteNonnegative(geometry.badgeDiameter) || geometry.badgeDiameter <= 0 ||
            !finiteNonnegative(geometry.badgeGap) || !finiteNonnegative(geometry.rowGap))
            return Status::InvalidInput;

        candidate->metricsDevice = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        if (candidate->metricsDevice.isNull()) return Status::OutOfMemory;
        candidate->metricsDevice.setDotsPerMeterX(dotsPerMeter96Dpi);
        candidate->metricsDevice.setDotsPerMeterY(dotsPerMeter96Dpi);
        candidate->owner = QThread::currentThread();
        candidate->notes.reserve(notes.size());
        for (const auto& note : notes) {
            if (note.number == 0) return Status::InvalidInput;
            PreparedNote prepared;
            prepared.number = note.number;
            prepared.color = Core::annotationColor(note.number);
            prepared.body.text = note.note.empty() ? options.emptyNoteText : QString::fromStdWString(note.note);
            if (prepared.body.text.isEmpty()) prepared.body.text = QString(QChar(0x2014));
            const auto status = measureBadge(note.number, options.font, candidate->metricsDevice, 16, prepared.badge);
            if (status != Status::Ok) return status;
            geometry.badgeDiameter = (std::max)(geometry.badgeDiameter,
                static_cast<float>(std::ceil((std::max)(prepared.badge.width, prepared.badge.height) + 12)));
            candidate->notes.push_back(std::move(prepared));
        }

        geometry.headerHeight = geometry.padding;
        if (!options.title.isEmpty()) {
            const auto status = measureWrapped(options.title, options.font, candidate->metricsDevice,
                (std::max)(18.0f, geometry.preferredFontSize),
                static_cast<qreal>(geometry.panelWidth) - geometry.padding * 2,
                candidate->title);
            if (status != Status::Ok) return status;
            geometry.headerHeight = static_cast<float>(geometry.padding * 2 + candidate->title.height);
        }

        Status textStatus = Status::Ok;
        const auto layout = Core::layoutAnnotations(imageWidth, imageHeight, candidate->notes.size(), geometry,
            [&](std::size_t index, float size, float width) -> std::optional<float> {
                auto& note = candidate->notes[index];
                textStatus = measureWrapped(note.body.text, options.font, candidate->metricsDevice,
                    size, width, note.body);
                if (textStatus != Status::Ok) return std::nullopt;
                return static_cast<float>(note.body.height);
            });
        if (!layout) return textStatus == Status::Ok ? Status::LayoutOverflow : textStatus;
        candidate->layout = *layout;
        for (auto& note : candidate->notes) {
            const auto status = measureBadge(note.number, options.font, candidate->metricsDevice,
                layout->badgeFontSize, note.badge);
            if (status != Status::Ok) return status;
            if (note.badge.width > layout->badgeDiameter || note.badge.height > layout->badgeDiameter)
                return Status::LayoutOverflow;
            note.badge.inset += QPointF((layout->badgeDiameter - note.badge.width) / 2,
                (layout->badgeDiameter - note.badge.height) / 2);
        }
        output.data_ = std::move(candidate);
        return Status::Ok;
    }
    catch (const std::bad_alloc&) { return Status::OutOfMemory; }
    catch (const std::length_error&) { return Status::OutOfMemory; }
    catch (...) { return Status::RenderFailure; }
}

Status paintNotes(QPainter& painter, const Plan& plan) noexcept
{
    if (!painter.isActive() || !plan.data_) return Status::InvalidInput;
    if (plan.data_->notes.empty()) return Status::Ok;
    if (plan.data_->owner != QThread::currentThread()) return Status::InvalidInput;
    try {
        PainterState state(painter);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const auto& layout = plan.data_->layout;
        painter.fillRect(QRectF(layout.imageWidth, 0, layout.width - layout.imageWidth, layout.height),
            QColor(0xF8, 0xFA, 0xFC));
        for (std::uint32_t column = 0; column < layout.columns; ++column) {
            const qreal left = layout.imageWidth + column * layout.panelWidth;
            painter.setPen(QPen(QColor(0xCB, 0xD5, 0xE1), 1));
            painter.drawLine(QPointF(left + 0.5, 0), QPointF(left + 0.5, layout.height));
            if (!plan.data_->title.paragraphs.empty()) {
                painter.setPen(QColor(0x17, 0x20, 0x33));
                drawText(painter, plan.data_->title, QPointF(left + layout.padding, layout.padding));
            }
        }
        for (const auto& row : layout.rows) {
            const auto& note = plan.data_->notes[row.index];
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor::fromRgb(note.color.backgroundRgb));
            painter.drawEllipse(QRectF(row.x, row.y, layout.badgeDiameter, layout.badgeDiameter));
            painter.setPen(QColor::fromRgb(note.color.textRgb));
            drawText(painter, note.badge, QPointF(row.x, row.y));
            painter.setPen(QColor(0x27, 0x36, 0x4B));
            drawText(painter, note.body, QPointF(row.x + layout.badgeDiameter + layout.badgeGap,
                row.y + (row.height - note.body.height) / 2));
        }
        return Status::Ok;
    }
    catch (const std::bad_alloc&) { return Status::OutOfMemory; }
    catch (...) { return Status::RenderFailure; }
}

Status render(const QImage& source, const Plan& plan, QImage& output) noexcept
{
    if (!plan.data_ || source.isNull() || !supportedFormat(source.format())) return Status::InvalidInput;
    const auto& layout = plan.data_->layout;
    if (source.width() != static_cast<int>(layout.imageWidth) ||
        source.height() != static_cast<int>(layout.imageHeight) ||
        (!plan.data_->notes.empty() && plan.data_->owner != QThread::currentThread()))
        return Status::InvalidInput;
    const auto pixels = Core::pixelLayout(static_cast<int>(layout.width), static_cast<int>(layout.height), 4);
    const auto sourcePixels = Core::pixelLayout(source.width(), source.height(), 4);
    if (!pixels || !sourcePixels || layout.width > plan.data_->maximumDimension ||
        layout.height > plan.data_->maximumDimension || pixels->bytes > plan.data_->maximumBytes ||
        source.bytesPerLine() < 0 || static_cast<std::size_t>(source.bytesPerLine()) < sourcePixels->stride)
        return Status::InvalidInput;
    try {
        QImage candidate(static_cast<int>(layout.width), static_cast<int>(layout.height), source.format());
        if (candidate.isNull()) return Status::OutOfMemory;
        if (candidate.sizeInBytes() < 0 || static_cast<std::size_t>(candidate.sizeInBytes()) > plan.data_->maximumBytes)
            return Status::LayoutOverflow;
        candidate.setDevicePixelRatio(1);
        candidate.setDotsPerMeterX(dotsPerMeter96Dpi);
        candidate.setDotsPerMeterY(dotsPerMeter96Dpi);
        candidate.setColorSpace(source.colorSpace());
        candidate.fill(Qt::white);
        QPainter painter;
        if (!painter.begin(&candidate)) return Status::RenderFailure;
        const auto status = paintNotes(painter, plan);
        const bool ended = painter.end();
        if (status != Status::Ok) return status;
        if (!ended) return Status::RenderFailure;
        // QPainter::drawImage would blend or reinterpret alpha and DPR. Copy
        // only the logical row bytes, also supporting a padded borrowed source.
        for (int y = 0; y < source.height(); ++y)
            std::memcpy(candidate.scanLine(y), source.constScanLine(y), sourcePixels->stride);
        output = std::move(candidate);
        return Status::Ok;
    }
    catch (const std::bad_alloc&) { return Status::OutOfMemory; }
    catch (const std::length_error&) { return Status::OutOfMemory; }
    catch (...) { return Status::RenderFailure; }
}

} // namespace HuhuCapture::QtAnnotationRenderer
