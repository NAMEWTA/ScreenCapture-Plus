#pragma once

#include "core/Annotation.h"

#include <QFont>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTextLayout>

#include <memory>
#include <span>
#include <vector>

class QPainter;

// Qt implementation of the existing complete-note export contract. Geometry,
// font compaction and column packing remain in Core::layoutAnnotations; Qt
// supplies Unicode shaping, font fallback, wrapping and rasterization only.
namespace HuhuCapture::QtAnnotationRenderer {

enum class Status { Ok, InvalidInput, LayoutOverflow, OutOfMemory, RenderFailure, FontUnavailable };

struct Options : Core::AnnotationLayoutOptions {
    QString title;
    QString emptyNoteText;
    QFont font;
};

struct Paragraph {
    std::unique_ptr<const QTextLayout> layout;
    QPointF position;
};

struct PreparedText {
    // Original full UTF-16 text, before normalizing paragraph separators for
    // layout. No elision, rich-text parsing or text-count limit is applied.
    QString text;
    std::vector<Paragraph> paragraphs;
    QRectF inkBounds;
    QPointF inset;
    qreal height{};
    qreal width{};
    qreal fontSize{};
    std::size_t lineCount{};
};

struct PreparedNote {
    std::uint64_t number{};
    Core::AnnotationColor color{};
    PreparedText body;
    PreparedText badge;
};

class Plan {
public:
    Plan() noexcept;
    ~Plan();
    Plan(Plan&&) noexcept;
    Plan& operator=(Plan&&) noexcept;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;

    bool isValid() const noexcept;
    const Core::AnnotationLayout& layout() const noexcept;
    std::span<const PreparedNote> notes() const noexcept;
    const PreparedText& title() const noexcept;

private:
    struct Data;
    std::unique_ptr<Data> data_;

    friend Status makePlan(QSize, std::span<const Core::AnnotationNote>, const Options&, Plan&) noexcept;
    friend Status paintNotes(QPainter&, const Plan&) noexcept;
    friend Status render(const QImage&, const Plan&, QImage&) noexcept;
};

// Requires a QGuiApplication when notes are present. Font sizes and measurements
// use a private 96-DPI image, independent of screen DPI and application scale.
// Plans own all layouts and complete text. Create, paint and destroy each plan
// on the same thread; Qt font engines must not migrate between worker threads.
// Failure leaves an earlier valid plan unchanged.
Status makePlan(QSize imageSize, std::span<const Core::AnnotationNote> notes,
    const Options& options, Plan& output) noexcept;

// Paint the complete rail using the caller's transform (for an editor preview).
// Does not clear the image area or change the caller's painter state. A preview
// is independent of render(), which always composes at original-image pixels.
Status paintNotes(QPainter& painter, const Plan& plan) noexcept;

// Supports the 32-bit RGB32/ARGB32/RGBX8888/RGBA8888 formats, including both
// premultiplied variants. The output retains the source format/color-space tag,
// has DPR 1 and 96-DPI pixel coordinates, and copies every original pixel byte
// without scaling or alpha blending. Extra canvas below the image is white.
// Borrowed source buffers must remain alive throughout this call. Invalid
// formats, allocation failures or render failures leave output unchanged.
Status render(const QImage& source, const Plan& plan, QImage& output) noexcept;

} // namespace HuhuCapture::QtAnnotationRenderer
