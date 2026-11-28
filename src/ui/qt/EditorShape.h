#pragma once

#include "editor/AnnotationModel.h"
#include "editor/ArrowModel.h"
#include "editor/BoxModel.h"
#include "editor/NumberModel.h"
#include "editor/StrokeModel.h"
#include "editor/TextModel.h"
#include "ui/qt/EditorTypes.h"

#include <QFont>
#include <QImage>
#include <QPainterPath>
#include <QPoint>
#include <QRectF>
#include <QTextLayout>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

class QPainter;

namespace HuhuCapture::UI::Qt {

// Qt presentation resources around the one shared geometry implementation.
// This record is owned exclusively by EditorDocument; widgets borrow it only
// until the document's beforeRemove/beforeDeactivate callback releases them.
class EditorShape final {
public:
    using Geometry = std::variant<Editor::BoxModel, Editor::ArrowModel, Editor::NumberModel,
        Editor::StrokeModel, Editor::TextModel, Editor::AnnotationModel>;

    EditorShape(EditorTool tool, EditorToolStyle logicalStyle, float dpi, float zoom,
        QSize imageSize, std::uint64_t number, QFont font);
    ~EditorShape();
    EditorShape(const EditorShape&) = delete;
    EditorShape& operator=(const EditorShape&) = delete;

    EditorTool tool() const noexcept { return tool_; }
    const Geometry& geometry() const noexcept { return geometry_; }
    const EditorToolStyle& pixelStyle() const noexcept { return style_; }
    const Editor::AnnotationModel* annotationModel() const noexcept;
    Editor::AnnotationModel* annotationModel() noexcept;
    std::optional<std::uint64_t> ordinaryNumber() const noexcept;
    Editor::TextModel* textModel() noexcept;
    const Editor::TextModel* textModel() const noexcept;
    bool validWithoutDrag() const noexcept;
    bool hasValidRegion() const noexcept;
    bool effectReady() const noexcept { return effectReady_; }
    int hitTest(Editor::EditorPoint point, float dpi, float zoom) const;
    void beginGesture(Editor::EditorPoint point, int handle, bool shift);
    void drag(Editor::EditorPoint point, bool shift, float dpi);
    void finishGesture();
    void setPixelValue(float value);
    void setTextStyle(EditorToolStyle logicalStyle, float dpi);
    void setFont(QFont font);
    bool refreshTextLayout();
    QRectF effectBounds() const;
    void setMosaic(QImage image, QPoint origin);
    bool paint(QPainter& painter, const QImage& original, bool exporting = false) const;
    void paintHandles(QPainter& painter, float dpi, float zoom) const;
    ::Qt::CursorShape cursorForHandle(int handle) const noexcept;

private:
    struct TextLine {
        std::unique_ptr<QTextLayout> layout;
        QPointF position;
    };
    void rebuildPath();
    bool makeLabel(const QString& text, qreal pixels, bool bold, qreal lineWidth = 1048576.0);
    QPainterPath effectPath() const;
    void drawLabel(QPainter& painter, QPointF position) const;

    EditorTool tool_;
    EditorToolStyle style_;
    float creationDpi_;
    QSize imageSize_;
    Geometry geometry_;
    QFont font_;
    QImage metrics_;
    QPainterPath path_;
    std::vector<TextLine> label_;
    QSizeF labelSize_;
    bool labelValid_{true};
    qreal badgeRadius_{};
    QImage mosaic_;
    QPoint mosaicOrigin_;
    bool effectReady_{};
};

} // namespace HuhuCapture::UI::Qt
