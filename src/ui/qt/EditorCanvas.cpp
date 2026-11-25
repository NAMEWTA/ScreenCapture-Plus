#include "ui/qt/EditorCanvas.h"

#include "core/MosaicPixels.h"
#include <QApplication>
#include <QColorSpace>
#include <QEvent>
#include <QFocusEvent>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <new>

namespace HuhuCapture::UI::Qt {
namespace {
constexpr auto index(EditorTool tool) noexcept { return static_cast<std::size_t>(tool); }
EditorFailure renderFailure(QtAnnotationRenderer::Status status = QtAnnotationRenderer::Status::RenderFailure)
{ return {EditorFailure::Code::RenderFailed, status}; }
bool supportedImage(const QImage& image) noexcept
{
    switch (image.format()) {
    case QImage::Format_RGB32: case QImage::Format_ARGB32: case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888: case QImage::Format_RGBA8888: case QImage::Format_RGBA8888_Premultiplied:
        return true;
    default: return false;
    }
}
}

EditorCanvas::EditorCanvas(QWidget* parent) : QWidget(parent), document_(std::make_unique<Document>())
{
    setObjectName(QStringLiteral("editorCanvas"));
    setFocusPolicy(::Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(::Qt::WA_InputMethodEnabled);
    for (const auto tool : editorTools) styles_[index(tool)] = defaultToolStyle(tool);
    textEditor_ = new QPlainTextEdit(this);
    textEditor_->setObjectName(QStringLiteral("shapeTextEditor"));
    textEditor_->setFrameShape(QFrame::NoFrame);
    textEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    textEditor_->setHorizontalScrollBarPolicy(::Qt::ScrollBarAlwaysOff);
    textEditor_->setVerticalScrollBarPolicy(::Qt::ScrollBarAlwaysOff);
    textEditor_->document()->setDocumentMargin(0);
    textEditor_->setStyleSheet(QStringLiteral("QPlainTextEdit { background: transparent; border: 0; padding: 0; }"));
    textEditor_->viewport()->setAutoFillBackground(false);
    textEditor_->installEventFilter(this);
    textEditor_->hide();
    connect(textEditor_, &QPlainTextEdit::textChanged, this, [this] {
        if (!editing_) return;
        editing_->textModel()->setText(textEditor_->toPlainText().toStdWString());
        if (!editing_->refreshTextLayout()) emit operationFailed(renderFailure(QtAnnotationRenderer::Status::FontUnavailable));
        updateTextEditorGeometry();
        update();
    });
}

EditorCanvas::~EditorCanvas()
{
    // Hide/unfocus the reusable input before destroying the history it borrows.
    disconnect(textEditor_, nullptr, this, nullptr);
    editing_ = hovered_ = pressed_ = previousClickCreated_ = nullptr;
    textEditor_->removeEventFilter(this);
}

EditorFailure EditorCanvas::loadImage(const QImage& image)
{
    if (QThread::currentThread() != thread()) return {EditorFailure::Code::InvalidOperation};
    if (hasImage()) return {EditorFailure::Code::ImageAlreadyLoaded};
    if (image.isNull() || !Core::pixelLayout(image.width(), image.height(), 4))
        return {EditorFailure::Code::InvalidImage};
    try {
        // Unchanged 32-bit source bytes survive export. Other decoder formats
        // normalize once at the boundary, not on every paint or pointer move.
        auto owned = supportedImage(image) ? image.copy() : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (owned.isNull()) return {EditorFailure::Code::OutOfMemory};
        owned.setDevicePixelRatio(1);
        original_ = std::move(owned);
        updateCanvasGeometry();
        update();
        return {};
    } catch (const std::bad_alloc&) { return {EditorFailure::Code::OutOfMemory}; }
}

EditorToolStyle EditorCanvas::toolStyle(EditorTool tool) const noexcept
{ return index(tool) < styles_.size() ? styles_[index(tool)] : EditorToolStyle{}; }

void EditorCanvas::setTool(EditorTool tool)
{
    if (!interactionEnabled_ || index(tool) >= styles_.size() || tool_ == tool) return;
    cancelPointerGesture();
    finishTextEditing();
    tool_ = tool;
    hovered_ = nullptr;
    hoverHandle_ = -1;
    setCursor(tool == EditorTool::None ? ::Qt::ArrowCursor : ::Qt::CrossCursor);
    update();
    emit toolChanged(tool_);
}

void EditorCanvas::setToolStyle(EditorTool tool, EditorToolStyle style)
{
    if (!interactionEnabled_ || index(tool) >= styles_.size()) return;
    style = normalizedToolStyle(tool, style);
    if (styles_[index(tool)] == style) return;
    styles_[index(tool)] = style;
    if (tool == EditorTool::Text && editing_) {
        editing_->setTextStyle(style, static_cast<float>(devicePixelRatioF()));
        updateTextEditorGeometry();
        update();
    }
    emit toolStyleChanged(tool, style);
}

void EditorCanvas::setTranslator(EditorTranslator translator) { translator_ = std::move(translator); }
QString EditorCanvas::translated(QStringView key) const { return translator_ ? translator_(key) : key.toString(); }

void EditorCanvas::setInteractionEnabled(bool enabled)
{
    if (interactionEnabled_ == enabled) return;
    if (!enabled) { cancelPointerGesture(); finishTextEditing(); }
    interactionEnabled_ = enabled;
    textEditor_->setReadOnly(!enabled);
    setCursor(enabled ? (tool_ == EditorTool::None ? ::Qt::ArrowCursor : ::Qt::CrossCursor) : ::Qt::BusyCursor);
}

void EditorCanvas::setPureImageMode(bool pure)
{
    if (!interactionEnabled_ || pureImage_ == pure) return;
    if (pure) { setTool(EditorTool::None); cancelPointerGesture(); finishTextEditing(); }
    pureImage_ = pure;
    update();
    emit pureImageModeChanged(pure);
}

void EditorCanvas::setAnnotationPanelVisible(bool visible) { panelVisible_ = visible; }
qreal EditorCanvas::sourceToViewScale() const noexcept { return zoom_ / devicePixelRatioF(); }
QPointF EditorCanvas::sourceToView(QPointF point) const noexcept { return point * sourceToViewScale(); }
QPointF EditorCanvas::viewToSource(QPointF point) const noexcept { return point / sourceToViewScale(); }

void EditorCanvas::setZoom(qreal requested, QPointF viewAnchor)
{
    if (!hasImage() || pointerGestureActive() || !std::isfinite(requested) || requested <= 0) return;
    const auto maximum = (std::max)(1.0, (std::min)(8.0, 16000.0 / (std::max)(original_.width(), original_.height())));
    const qreal next = std::clamp(requested, panelVisible_ ? .01 : .1, maximum);
    if (qFuzzyCompare(next, zoom_)) return;
    const auto previous = zoom_;
    zoom_ = next;
    updateCanvasGeometry();
    updateTextEditorGeometry();
    update();
    emit viewChanged(previous, zoom_, viewAnchor);
}

QSize EditorCanvas::sizeHint() const
{
    return hasImage() ? QSize(qCeil(original_.width() * sourceToViewScale()),
        qCeil(original_.height() * sourceToViewScale())) : QSize(1, 1);
}

void EditorCanvas::updateCanvasGeometry() { setFixedSize(sizeHint()); updateGeometry(); }
Editor::EditorPoint EditorCanvas::imagePoint(QPointF position) const noexcept
{
    const auto source = viewToSource(position);
    return {static_cast<float>(std::round(source.x())), static_cast<float>(std::round(source.y()))};
}
bool EditorCanvas::insideImage(QPointF position) const noexcept
{
    const auto source = viewToSource(position);
    return hasImage() && source.x() >= 0 && source.y() >= 0 &&
        source.x() < original_.width() && source.y() < original_.height();
}

void EditorCanvas::resolveHover(QPointF position)
{
    hovered_ = nullptr;
    hoverHandle_ = -1;
    if (tool_ != EditorTool::None && insideImage(position)) {
        const auto shapes = document_->activeItems();
        for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
            const auto handle = it->get()->hitTest(imagePoint(position), static_cast<float>(devicePixelRatioF()), static_cast<float>(zoom_));
            if (handle >= 0) { hovered_ = it->get(); hoverHandle_ = handle; break; }
        }
    }
    setCursor(hovered_ ? hovered_->cursorForHandle(hoverHandle_) :
        (tool_ == EditorTool::None ? ::Qt::ArrowCursor : ::Qt::CrossCursor));
}

void EditorCanvas::releaseBorrow(EditorShape* shape)
{
    // Called during HistoryStack mutation: no recursive erase/undo here.
    if (shape == editing_) detachTextEditor();
    if (shape == hovered_) { hovered_ = nullptr; hoverHandle_ = -1; }
    if (shape == pressed_) pressed_ = nullptr;
    if (shape == previousClickCreated_) previousClickCreated_ = nullptr;
}

bool EditorCanvas::removeShape(EditorShape* shape)
{
    if (!shape || !document_->erase(shape, [this](auto* removed) { releaseBorrow(removed); })) return false;
    notifyStructure();
    return true;
}

void EditorCanvas::notifyStructure()
{
    update();
    emit historyChanged();
    emit annotationsChanged();
}

bool EditorCanvas::undo()
{
    if (!interactionEnabled_) return false;
    cancelPointerGesture();
    finishTextEditing();
    if (!document_->undo([this](auto* removed) { releaseBorrow(removed); })) return false;
    notifyStructure();
    return true;
}

bool EditorCanvas::redo()
{
    if (!interactionEnabled_) return false;
    cancelPointerGesture();
    finishTextEditing();
    if (!document_->redo()) return false;
    notifyStructure();
    return true;
}

bool EditorCanvas::deleteSelection()
{
    if (!interactionEnabled_ || editing_ || pointerGestureActive()) return false;
    if (hovered_) return removeShape(hovered_);
    return panelVisible_ && deleteAnnotation(document_->selectedAnnotation());
}
bool EditorCanvas::deleteAnnotation(std::uint64_t number)
{
    if (!interactionEnabled_ || pointerGestureActive()) return false;
    return removeShape(document_->findAnnotation(number));
}
bool EditorCanvas::selectAnnotation(std::uint64_t number, bool focusEditor)
{
    if (!interactionEnabled_ || pointerGestureActive() || !document_->selectAnnotation(number)) return false;
    update();
    emit annotationSelected(number, focusEditor);
    return true;
}
bool EditorCanvas::setAnnotationNote(std::uint64_t number, const QString& value)
{
    if (!interactionEnabled_) return false;
    auto* shape = document_->findAnnotation(number);
    if (!shape) return false;
    auto text = value.toStdWString();
    if (shape->annotationModel()->note() == text) return true;
    shape->annotationModel()->setNote(std::move(text));
    emit annotationNoteChanged(number);
    return true;
}

void EditorCanvas::startTextEditing(EditorShape* shape)
{
    if (!shape || !shape->textModel()) return;
    if (editing_ == shape) { textEditor_->setFocus(); return; }
    finishTextEditing();
    shape->setTextStyle(toolStyle(EditorTool::Text), static_cast<float>(devicePixelRatioF()));
    shape->textModel()->startEditing();
    editing_ = shape;
    {
        const QSignalBlocker blocked(textEditor_);
        textEditor_->setPlainText(QString::fromStdWString(shape->textModel()->text()));
    }
    updateTextEditorGeometry();
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(::Qt::MouseFocusReason);
    auto cursor = textEditor_->textCursor();
    cursor.movePosition(QTextCursor::End);
    textEditor_->setTextCursor(cursor);
    update();
}

void EditorCanvas::detachTextEditor()
{
    if (!editing_) return;
    editing_->textModel()->finishEditing();
    editing_ = nullptr; // Hide may synchronously dispatch FocusOut.
    textEditor_->hide();
}

void EditorCanvas::finishTextEditing()
{
    if (!editing_) return;
    QGuiApplication::inputMethod()->commit();
    auto* shape = editing_;
    shape->textModel()->setText(textEditor_->toPlainText().toStdWString());
    shape->refreshTextLayout();
    const bool empty = shape->textModel()->text().empty();
    const auto ticket = document_->ticket(shape);
    detachTextEditor();
    if (empty) QTimer::singleShot(0, this, [this, ticket] {
        if (auto* current = ticket.get(); current && current != editing_ && current->textModel()->text().empty())
            removeShape(current);
    });
    update();
}

void EditorCanvas::updateTextEditorGeometry()
{
    if (!editing_) return;
    const auto* text = editing_->textModel();
    const auto& b = text->bounds();
    const auto topLeft = sourceToView({b.left + text->padding(), b.top + text->padding()});
    const qreal scale = sourceToViewScale();
    auto font = this->font();
    font.setPointSizeF(text->style().fontSize * scale * 72.0 / logicalDpiY());
    font.setWeight(text->style().bold ? QFont::Bold : QFont::Normal);
    font.setItalic(text->style().italic);
    textEditor_->setFont(font);
    auto palette = textEditor_->palette();
    palette.setColor(QPalette::Text, editorColor(text->style().rgba));
    palette.setColor(QPalette::Base, ::Qt::transparent);
    textEditor_->setPalette(palette);
    const qreal contentWidth = (std::max)(2.0, (b.right - b.left - 2 * text->padding()) * scale);
    const qreal contentHeight = (std::max)(font.pointSizeF() * logicalDpiY() / 72.0 * 1.5,
        (b.bottom - b.top - 2 * text->padding()) * scale);
    // Reserve caret width; the image parent clips text that exceeds its edges.
    textEditor_->setGeometry(qFloor(topLeft.x()), qFloor(topLeft.y()), qCeil(contentWidth) + 3, qCeil(contentHeight) + 2);
}

bool EditorCanvas::buildMosaic(EditorShape& shape)
{
    const int block = (std::max)(6, qRound(shape.pixelStyle().value / 3));
    const auto region = shape.effectBounds().adjusted(-block, -block, block + 1, block + 1)
        .intersected(QRectF(0, 0, original_.width(), original_.height())).toAlignedRect();
    // Moving an existing mask entirely outside the image remains legal. It
    // contributes no pixels and must not make an otherwise valid export fail.
    if (region.isEmpty()) return true;
    auto local = original_.copy(region);
    if (local.isNull()) return false;
    // A local source rectangle, not a full-frame readback for every move.
    {
        QPainter painter(&local);
        if (!painter.isActive()) return false;
        painter.translate(-region.topLeft());
        for (const auto& item : document_->activeItems()) {
            if (item.get() == &shape) break;
            if (!item->paint(painter, original_, true)) return false;
        }
        if (!painter.end()) return false;
    }
    if (!Core::mosaicPixels({local.bits(), static_cast<std::size_t>(local.sizeInBytes())},
        static_cast<std::size_t>(local.bytesPerLine()), local.width(), local.height(), block)) return false;
    shape.setMosaic(std::move(local), region.topLeft());
    return true;
}

bool EditorCanvas::prepareEffects()
{
    for (const auto& item : document_->activeItems())
        if (item->tool() == EditorTool::Mosaic && !item->effectReady() && !buildMosaic(*item.get())) return false;
    return true;
}

EditorFailure EditorCanvas::renderExport(QImage& output, QtAnnotationRenderer::Options options)
{
    if (QThread::currentThread() != thread() || !hasImage() || pointerGestureActive())
        return {EditorFailure::Code::InvalidOperation};
    try {
        finishTextEditing();
        if (!prepareEffects()) return renderFailure();
        auto composed = original_.copy();
        if (composed.isNull()) return {EditorFailure::Code::OutOfMemory};
        {
            QPainter painter(&composed);
            if (!painter.isActive()) return renderFailure();
            for (const auto& shape : document_->activeItems())
                if (!shape->paint(painter, original_, true)) return renderFailure();
            if (!painter.end()) return renderFailure();
        }
        if (options.title.isEmpty()) options.title = translated(u"annotation.title");
        if (options.emptyNoteText.isEmpty()) options.emptyNoteText = translated(u"annotation.empty");
        if (options.font.family().isEmpty()) options.font = font();
        QtAnnotationRenderer::Plan plan;
        const auto notes = document_->annotationNotes();
        auto result = QtAnnotationRenderer::makePlan(composed.size(), notes, options, plan);
        if (result != QtAnnotationRenderer::Status::Ok) return renderFailure(result);
        result = QtAnnotationRenderer::render(composed, plan, output);
        return result == QtAnnotationRenderer::Status::Ok ? EditorFailure{} : renderFailure(result);
    } catch (const std::bad_alloc&) { return {EditorFailure::Code::OutOfMemory}; }
}

void EditorCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().brush(QPalette::Base));
    if (!hasImage()) return;
    painter.scale(sourceToViewScale(), sourceToViewScale());
    painter.setClipRect(QRect(0, 0, original_.width(), original_.height()));
    painter.drawImage(QPoint(0, 0), original_);
    for (const auto& shape : document_->activeItems()) shape->paint(painter, original_);
    if (!pureImage_ && hovered_ && interactionEnabled_)
        hovered_->paintHandles(painter, static_cast<float>(devicePixelRatioF()), static_cast<float>(zoom_));
}

void EditorCanvas::mousePressEvent(QMouseEvent* event)
{
    if (!interactionEnabled_ || !hasImage()) { event->ignore(); return; }
    if (event->button() == ::Qt::RightButton) { setPureImageMode(!pureImage_); event->accept(); return; }
    if (event->button() != ::Qt::LeftButton || !insideImage(event->position())) { event->ignore(); return; }
    if (pureImage_) setPureImageMode(false);
    previousClickCreated_ = nullptr;
    pressPosition_ = event->position();
    pressGlobal_ = event->globalPosition().toPoint();
    dragged_ = false;
    if (tool_ == EditorTool::None) {
        finishTextEditing();
        movingWindow_ = true;
        windowPosition_ = window()->pos();
        auto* native = window()->windowHandle();
        nativeWindowMove_ = native && native->startSystemMove();
        if (!nativeWindowMove_) grabMouse();
        emit pointerGestureChanged(true);
        return;
    }
    // An outside press finishes ordinary text; it must not create an empty new
    // shape under the same event or steal the text widget's selection gestures.
    if (editing_) { finishTextEditing(); setFocus(); event->accept(); return; }
    setFocus(::Qt::MouseFocusReason);
    resolveHover(event->position());
    if (hovered_ && hovered_->textModel() && hoverHandle_ == static_cast<int>(Editor::TextHandle::Edit)) {
        startTextEditing(hovered_);
        return;
    }
    newlyCreated_ = !hovered_;
    try {
        if (!hovered_) {
            const auto number = tool_ == EditorTool::Annotation ? document_->allocateAnnotationId() :
                tool_ == EditorTool::Number ? document_->nextOrdinaryNumber() : std::optional<std::uint64_t>{0};
            if (!number) { emit operationFailed({EditorFailure::Code::InvalidOperation}); return; }
            auto shape = std::make_unique<EditorShape>(tool_, toolStyle(tool_), static_cast<float>(devicePixelRatioF()),
                static_cast<float>(zoom_), original_.size(), *number, font());
            hovered_ = document_->append(std::move(shape), [this](auto* removed) { releaseBorrow(removed); });
            hoverHandle_ = -1;
        }
        pressed_ = hovered_;
        pressed_->beginGesture(imagePoint(event->position()), hoverHandle_, event->modifiers().testFlag(::Qt::ShiftModifier));
        if (pressed_->textModel() && newlyCreated_) {
            pressed_->finishGesture();
            pressed_ = nullptr;
            startTextEditing(hovered_);
            notifyStructure();
            return;
        }
        grabMouse();
        emit pointerGestureChanged(true);
        if (newlyCreated_) notifyStructure();
        update();
    } catch (const std::bad_alloc&) {
        cancelPointerGesture();
        emit operationFailed({EditorFailure::Code::OutOfMemory});
    }
}

void EditorCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!interactionEnabled_) return;
    if (movingWindow_) {
        if (!event->buttons().testFlag(::Qt::LeftButton)) { finishPointer(true); return; }
        if (!nativeWindowMove_) window()->move(windowPosition_ + event->globalPosition().toPoint() - pressGlobal_);
        return;
    }
    if (pressed_) {
        if (!event->buttons().testFlag(::Qt::LeftButton)) { cancelPointerGesture(); return; }
        dragged_ = dragged_ || event->position() != pressPosition_;
        const auto previousValue = pressed_->pixelStyle().value;
        pressed_->drag(imagePoint(event->position()), event->modifiers().testFlag(::Qt::ShiftModifier),
            static_cast<float>(devicePixelRatioF()));
        if (pressed_->tool() == EditorTool::Number && pressed_->pixelStyle().value != previousValue) {
            auto style = toolStyle(EditorTool::Number);
            style.value = pressed_->pixelStyle().value / static_cast<float>(devicePixelRatioF());
            setToolStyle(EditorTool::Number, style);
        }
        update();
        return;
    }
    resolveHover(event->position());
    update();
}

void EditorCanvas::finishPointer(bool cancelled)
{
    if (!pointerGestureActive()) return;
    auto* shape = pressed_;
    const bool created = newlyCreated_;
    const bool moved = dragged_;
    pressed_ = nullptr;
    newlyCreated_ = dragged_ = movingWindow_ = nativeWindowMove_ = false;
    if (QWidget::mouseGrabber() == this) releaseMouse();
    previousClickCreated_ = nullptr;
    if (shape) {
        const bool discard = created && (cancelled || (!moved && !shape->validWithoutDrag()) || !shape->hasValidRegion());
        if (discard) removeShape(shape);
        else {
            shape->finishGesture();
            if (shape->tool() == EditorTool::Mosaic && !buildMosaic(*shape)) emit operationFailed(renderFailure());
            if (created) previousClickCreated_ = shape;
            if (const auto* annotation = shape->annotationModel())
                selectAnnotation(annotation->number(), !cancelled && (created || !moved));
        }
    }
    update();
    emit pointerGestureChanged(false);
}

void EditorCanvas::cancelPointerGesture() { finishPointer(true); }
void EditorCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == ::Qt::LeftButton) { finishPointer(false); event->accept(); }
    else event->ignore();
}

void EditorCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != ::Qt::LeftButton || !interactionEnabled_ || editing_ ||
        tool_ == EditorTool::Annotation || (hovered_ && hovered_->annotationModel())) {
        mousePressEvent(event);
        return;
    }
    cancelPointerGesture();
    if (previousClickCreated_) undo();
    previousClickCreated_ = nullptr;
    emit outputRequested(OutputIntent::Clipboard);
}

void EditorCanvas::wheelEvent(QWheelEvent* event)
{
    if (!interactionEnabled_ || pointerGestureActive()) { event->ignore(); return; }
    const auto delta = event->angleDelta().y();
    if (!delta) { event->ignore(); return; }
    if (event->modifiers().testFlag(::Qt::ControlModifier)) {
        setZoom(zoom_ * (delta > 0 ? 1.1 : 1 / 1.1), event->position());
        event->accept();
        return;
    }
    resolveHover(event->position());
    if (!hovered_) { event->ignore(); return; }
    const auto tool = hovered_->tool();
    if ((tool != EditorTool::Rectangle && tool != EditorTool::Ellipse && tool != EditorTool::Arrow && tool != EditorTool::Number) ||
        ((tool == EditorTool::Rectangle || tool == EditorTool::Ellipse) && hovered_->pixelStyle().filled)) {
        event->ignore(); return;
    }
    const auto dpi = static_cast<float>(devicePixelRatioF());
    const float step = tool == EditorTool::Number ? 1.f : dpi;
    auto style = toolStyle(tool);
    style.value = (hovered_->pixelStyle().value + (delta > 0 ? step : -step)) / dpi;
    style = normalizedToolStyle(tool, style);
    hovered_->setPixelValue(style.value * dpi);
    setToolStyle(tool, style);
    update();
    event->accept();
}

void EditorCanvas::keyPressEvent(QKeyEvent* event)
{
    if (!interactionEnabled_ || editing_) { event->ignore(); return; }
    const bool control = event->modifiers().testFlag(::Qt::ControlModifier);
    if (control && event->key() == ::Qt::Key_Z) undo();
    else if (control && event->key() == ::Qt::Key_Y) redo();
    else if (control && event->key() == ::Qt::Key_C) emit outputRequested(OutputIntent::Clipboard);
    else if (control && event->key() == ::Qt::Key_S) emit outputRequested(OutputIntent::SaveAs);
    else if (event->key() == ::Qt::Key_Return || event->key() == ::Qt::Key_Enter) emit outputRequested(OutputIntent::Clipboard);
    else if (event->key() == ::Qt::Key_Delete) deleteSelection();
    else if (event->key() == ::Qt::Key_Escape) emit closeRequested();
    else { event->ignore(); return; }
    event->accept();
}

void EditorCanvas::leaveEvent(QEvent* event)
{
    if (!pointerGestureActive() && !editing_) { hovered_ = nullptr; hoverHandle_ = -1; update(); }
    QWidget::leaveEvent(event);
}
void EditorCanvas::focusOutEvent(QFocusEvent* event)
{
    if (pressed_ && pressed_->annotationModel()) cancelPointerGesture();
    QWidget::focusOutEvent(event);
}
bool EditorCanvas::event(QEvent* event)
{
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate) cancelPointerGesture();
    if (event->type() == QEvent::DevicePixelRatioChange) {
        updateCanvasGeometry();
        updateTextEditorGeometry();
        update();
    }
    return QWidget::event(event);
}
bool EditorCanvas::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == textEditor_) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == ::Qt::Key_Escape) {
            finishTextEditing();
            setFocus();
            return true;
        }
        if (event->type() == QEvent::FocusOut && editing_) {
            // Qt can transfer focus to the canvas before delivering its press.
            // Keep the binding until that press can finish the existing input;
            // otherwise one outside click would also create a new empty text.
            // A ticket prevents a late FocusOut from ending a newer session.
            const auto ticket = document_->ticket(editing_);
            QTimer::singleShot(0, this, [this, ticket] {
                if (editing_ && editing_ == ticket.get() && !textEditor_->hasFocus()) finishTextEditing();
            });
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace HuhuCapture::UI::Qt
