#pragma once

#include "editor/EditorDocument.h"
#include "ui/qt/EditorShape.h"

#include <QWidget>
#include <array>
#include <memory>

class QPlainTextEdit;

namespace HuhuCapture::UI::Qt {

// A single source-pixel document, shared by capture and pinned-image hosts on
// both platforms. OS capture, clipboard and persistence stay with the host.
class EditorCanvas final : public QWidget {
    Q_OBJECT
public:
    using Document = Editor::EditorDocument<EditorShape>;

    explicit EditorCanvas(QWidget* parent = nullptr);
    ~EditorCanvas() override;

    // Copies a borrowed frame once. Loading another image requires another
    // canvas/document so an accidental callback cannot discard existing edits.
    EditorFailure loadImage(const QImage& image);
    const QImage& originalImage() const noexcept { return original_; }
    const Document& document() const noexcept { return *document_; }
    bool hasImage() const noexcept { return !original_.isNull(); }
    bool canUndo() const noexcept { return document_->activeCount() != 0; }
    bool canRedo() const noexcept { return document_->activeCount() < document_->size(); }

    EditorTool tool() const noexcept { return tool_; }
    EditorToolStyle toolStyle(EditorTool tool) const noexcept;
    void setTool(EditorTool tool);
    void setToolStyle(EditorTool tool, EditorToolStyle style);
    void setTranslator(EditorTranslator translator);
    void setInteractionEnabled(bool enabled);
    bool interactionEnabled() const noexcept { return interactionEnabled_; }
    bool pointerGestureActive() const noexcept { return pressed_ != nullptr || movingWindow_; }
    bool textEditing() const noexcept { return editing_ != nullptr; }
    bool pureImageMode() const noexcept { return pureImage_; }
    void setPureImageMode(bool pure);
    void setAnnotationPanelVisible(bool visible);

    qreal zoom() const noexcept { return zoom_; }
    qreal sourceToViewScale() const noexcept;
    QPointF sourceToView(QPointF point) const noexcept;
    QPointF viewToSource(QPointF point) const noexcept;
    void setZoom(qreal zoom, QPointF viewAnchor = {});
    QSize sizeHint() const override;

    bool undo();
    bool redo();
    bool deleteSelection();
    bool deleteAnnotation(std::uint64_t number);
    bool selectAnnotation(std::uint64_t number, bool focusEditor = true);
    bool setAnnotationNote(std::uint64_t number, const QString& text);
    void finishTextEditing();
    void cancelPointerGesture();

    // Full notes and all active shapes, at original pixels. A failure leaves
    // output unchanged and never removes edits. Call on the GUI thread.
    EditorFailure renderExport(QImage& output, QtAnnotationRenderer::Options options = {});

signals:
    void toolChanged(EditorTool tool);
    void toolStyleChanged(EditorTool tool, EditorToolStyle style);
    void historyChanged();
    void annotationsChanged();
    void annotationNoteChanged(quint64 number);
    void annotationSelected(quint64 number, bool focusEditor);
    void pointerGestureChanged(bool active);
    void pureImageModeChanged(bool pure);
    void viewChanged(qreal previousZoom, qreal zoom, QPointF previousViewAnchor);
    void outputRequested(OutputIntent intent);
    void closeRequested();
    void operationFailed(EditorFailure failure);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Editor::EditorPoint imagePoint(QPointF point) const noexcept;
    bool insideImage(QPointF point) const noexcept;
    void resolveHover(QPointF point);
    void releaseBorrow(EditorShape* shape);
    bool removeShape(EditorShape* shape);
    void notifyStructure();
    void startTextEditing(EditorShape* shape);
    void detachTextEditor();
    void updateTextEditorGeometry();
    void updateCanvasGeometry();
    bool buildMosaic(EditorShape& shape);
    bool prepareEffects();
    void finishPointer(bool cancelled);
    QString translated(QStringView key) const;

    std::unique_ptr<Document> document_;
    QImage original_;
    std::array<EditorToolStyle, 10> styles_{};
    EditorTool tool_{EditorTool::None};
    EditorTranslator translator_;
    QPlainTextEdit* textEditor_{};
    EditorShape *hovered_{}, *pressed_{}, *editing_{}, *previousClickCreated_{};
    int hoverHandle_{-1};
    bool newlyCreated_{}, dragged_{}, interactionEnabled_{true}, pureImage_{}, panelVisible_{};
    bool movingWindow_{}, nativeWindowMove_{};
    QPointF pressPosition_;
    QPoint pressGlobal_, windowPosition_;
    qreal zoom_{1.0};
};

} // namespace HuhuCapture::UI::Qt
