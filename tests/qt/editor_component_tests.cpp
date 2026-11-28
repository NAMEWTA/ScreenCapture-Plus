#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFontInfo>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QThread>
#include <QToolButton>
#include <QWheelEvent>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace HuhuCapture;
namespace UIQ = HuhuCapture::UI::Qt;

namespace {
void require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}
void settle() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
void waitUntil(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 1500) { settle(); QThread::msleep(5); }
    require(predicate(), message);
}
void show(QWidget& widget)
{
    widget.show(); widget.raise(); widget.activateWindow(); widget.setFocus(); settle();
}
QString text(QStringView key)
{
    if (key == u"annotation.title") return QStringLiteral("Annotations");
    if (key == u"annotation.empty") return QStringLiteral("No note");
    if (key == u"annotation.placeholder") return QStringLiteral("Write the complete note");
    if (key == u"annotation.edit") return QStringLiteral("Note");
    if (key == u"annotation.remove") return QStringLiteral("Delete");
    if (key == u"annotation.collapse") return QStringLiteral("Hide");
    if (key == u"annotation.saveHint") return QStringLiteral("Ctrl+S saves the image and complete notes");
    if (key == u"cap.mark") return QStringLiteral("Annotate Image");
    return key.toString();
}
QImage pixels(QSize size = {640, 400})
{
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            row[x] = qRgba(x % 251, y % 251, (x * 3 + y * 5) % 251, 80 + (x + y) % 176);
    }
    return image;
}
bool sameBytes(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.format() != b.format()) return false;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), static_cast<std::size_t>(a.width()) * 4) != 0) return false;
    return true;
}
void sendMouse(QWidget& target, QEvent::Type type, QPointF local, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF global = QPointF(target.mapToGlobal(local.toPoint())) + local - QPointF(local.toPoint());
    QMouseEvent event(type, local, QPointF(target.mapTo(target.window(), local.toPoint())), global, button, buttons, modifiers);
    QApplication::sendEvent(&target, &event);
}
void press(UIQ::EditorCanvas& canvas, QPointF source, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{ sendMouse(canvas, QEvent::MouseButtonPress, canvas.sourceToView(source), Qt::LeftButton, Qt::LeftButton, modifiers); }
void move(UIQ::EditorCanvas& canvas, QPointF source, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{ sendMouse(canvas, QEvent::MouseMove, canvas.sourceToView(source), Qt::NoButton, Qt::LeftButton, modifiers); }
void release(UIQ::EditorCanvas& canvas, QPointF source, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{ sendMouse(canvas, QEvent::MouseButtonRelease, canvas.sourceToView(source), Qt::LeftButton, Qt::NoButton, modifiers); settle(); }
void drag(UIQ::EditorCanvas& canvas, QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{ press(canvas, from, modifiers); move(canvas, to, modifiers); release(canvas, to, modifiers); }
void click(UIQ::EditorCanvas& canvas, QPointF source) { press(canvas, source); release(canvas, source); }
void key(QWidget& target, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(&target, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers); QApplication::sendEvent(&target, &release);
    settle();
}
void commit(QPlainTextEdit& target, const QString& value)
{
    QInputMethodEvent event;
    event.setCommitString(value);
    QApplication::sendEvent(&target, &event);
    settle();
}
void wheel(UIQ::EditorCanvas& canvas, QPointF source)
{
    const auto local = canvas.sourceToView(source);
    QWheelEvent event(local, canvas.mapToGlobal(local.toPoint()), {}, {0, 120},
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &event);
    settle();
}
void save(const QImage& image, const QString& directory, const QString& name)
{
    if (!directory.isEmpty()) require(image.save(QDir(directory).filePath(name)), "PNG artifact failed: " + name.toStdString());
}
void load(UIQ::EditorCanvas& canvas, const QImage& image)
{
    canvas.setTranslator(text);
    require(!canvas.loadImage(image), "load owned image");
    show(canvas);
}
void load(UIQ::EditorWindow& window, const QImage& image)
{
    window.setTranslator(text);
    require(!window.loadImage(image), "load window image");
    show(window);
}

void sourceOwnershipAndScale()
{
    auto source = pixels({360, 220});
    auto original = source.copy();
    source.setDevicePixelRatio(2);
    UIQ::EditorCanvas canvas;
    load(canvas, source);
    source.fill(Qt::black);
    require(sameBytes(canvas.originalImage(), original), "borrowed source must be deep-owned exactly once");
    require(canvas.originalImage().devicePixelRatio() == 1, "source-pixel DPR normalization");
    require(canvas.loadImage(pixels()).code == UIQ::EditorFailure::Code::ImageAlreadyLoaded,
        "another frame cannot silently discard the document");
    canvas.setZoom(.5);
    const QPointF sourcePoint(103, 77);
    require(QLineF(canvas.viewToSource(canvas.sourceToView(sourcePoint)), sourcePoint).length() < .00001,
        "source/view conversion survives fractional zoom and DPR");
    QImage output;
    require(!canvas.renderExport(output) && sameBytes(output, original), "zero-edit export preserves every source byte and alpha");
    std::cout << "PASS source ownership, physical pixel coordinates and lossless zero-edit export\n";
}

void nineToolRendering(const QString& directory)
{
    const auto source = pixels({280, 180});
    for (const auto tool : UIQ::editorTools) {
        UIQ::EditorCanvas canvas; load(canvas, source); canvas.setTool(tool);
        if (tool == UIQ::EditorTool::Number) click(canvas, {70, 80});
        else if (tool == UIQ::EditorTool::Text) {
            click(canvas, {20, 35});
            auto* editor = canvas.findChild<QPlainTextEdit*>(QStringLiteral("shapeTextEditor"));
            require(editor && canvas.textEditing(), "text starts the one shared editor");
            commit(*editor, QStringLiteral("完整文字 Aa\n中文标注"));
            canvas.finishTextEditing();
        } else drag(canvas, {24, 30}, {170, 125});
        require(canvas.document().activeCount() == 1, "one real shape for every tool");
        QImage output;
        const auto error = canvas.renderExport(output);
        require(!error, "render tool " + UIQ::toolLanguageKey(tool).toStdString() + " status " + std::to_string(static_cast<int>(error.rendering)));
        if (tool != UIQ::EditorTool::Eraser)
            require(!sameBytes(output, source), "real visible pixels for tool " + UIQ::toolLanguageKey(tool).toStdString());
        require(output.pixel(0, 0) == source.pixel(0, 0), "unrelated source pixel remains unchanged");
        save(output, directory, UIQ::toolLanguageKey(tool) + QStringLiteral(".png"));
    }

    UIQ::EditorCanvas canvas; load(canvas, source);
    auto rectangle = canvas.toolStyle(UIQ::EditorTool::Rectangle); rectangle.filled = true;
    canvas.setToolStyle(UIQ::EditorTool::Rectangle, rectangle); canvas.setTool(UIQ::EditorTool::Rectangle);
    drag(canvas, {20, 20}, {220, 150});
    QImage painted; require(!canvas.renderExport(painted), "filled rectangle export");
    require(painted.pixel(70, 70) != source.pixel(70, 70), "filled shape covers test pixel");
    auto eraser = canvas.toolStyle(UIQ::EditorTool::Eraser); eraser.rectangleMode = true;
    canvas.setToolStyle(UIQ::EditorTool::Eraser, eraser); canvas.setTool(UIQ::EditorTool::Eraser);
    drag(canvas, {40, 40}, {120, 110});
    QImage restored; require(!canvas.renderExport(restored), "eraser export");
    require(restored.pixel(70, 70) == source.pixel(70, 70), "eraser restores exact original RGBA inside mask");
    require(restored.pixel(180, 70) == painted.pixel(180, 70), "eraser leaves other edits intact");
    save(restored, directory, QStringLiteral("eraser-restoration.png"));
    std::cout << "PASS all nine real tools, localized full text, mosaic and original-pixel erasing\n";
}

void historyAndCaptureLoss()
{
    UIQ::EditorCanvas canvas; load(canvas, pixels()); canvas.setTool(UIQ::EditorTool::Annotation);
    drag(canvas, {120, 100}, {340, 300});
    drag(canvas, {190, 170}, {270, 240});
    require(canvas.document().activeCount() == 2, "transparent annotation interior permits nesting");
    press(canvas, {440, 30}); move(canvas, {570, 110});
    QEvent ungrab(QEvent::UngrabMouse); QApplication::sendEvent(&canvas, &ungrab);
    require(!canvas.pointerGestureActive() && canvas.document().activeCount() == 2,
        "lost capture removes unfinished new annotation");
    drag(canvas, {440, 30}, {570, 110});
    require(canvas.document().findAnnotation(4) && !canvas.document().findAnnotation(3), "cancelled annotation ID is never recycled");
    const auto before = canvas.document().findAnnotation(1)->annotationModel()->bounds();
    press(canvas, {230, 100}); move(canvas, {230, 130});
    QApplication::sendEvent(&canvas, &ungrab);
    const auto after = canvas.document().findAnnotation(1)->annotationModel()->bounds();
    require(after.top > before.top && after.bottom == before.bottom, "existing resize retains its last valid result on lost capture");
    require(canvas.undo() && !canvas.document().findAnnotation(4), "active-prefix undo");
    require(canvas.redo() && canvas.document().findAnnotation(4), "active-prefix redo restores stable ID");
    require(canvas.undo(), "undo before clearing redo branch");
    drag(canvas, {440, 140}, {570, 250});
    require(canvas.document().findAnnotation(5) && !canvas.canRedo(), "redo branch discarded without reusing annotation ID");
    require(canvas.deleteAnnotation(1) && canvas.document().findAnnotation(2), "delete does not renumber surviving notes");
    canvas.setTool(UIQ::EditorTool::Number); click(canvas, {50, 50}); click(canvas, {50, 150});
    auto items = canvas.document().activeItems();
    require(items.back()->ordinaryNumber() == 2, "ordinary numbers are independent of annotation IDs");
    require(canvas.undo(), "undo ordinary number"); click(canvas, {50, 250});
    items = canvas.document().activeItems();
    require(items.back()->ordinaryNumber() == 2, "ordinary number remains active maximum plus one");
    auto preference = canvas.toolStyle(UIQ::EditorTool::Number); preference.value = 42;
    canvas.setToolStyle(UIQ::EditorTool::Number, preference);
    drag(canvas, {50, 250}, {65, 270});
    require(canvas.toolStyle(UIQ::EditorTool::Number).value == 42,
        "moving an old marker does not overwrite the current radius preference");
    std::cout << "PASS nested hit testing, capture cancellation, resize preservation and both ID policies\n";
}

void windowDeactivation()
{
    UIQ::EditorWindow window; load(window, pixels());
    auto* canvas = window.canvas(); canvas->setTool(UIQ::EditorTool::Rectangle);
    press(*canvas, {70, 70}); move(*canvas, {180, 160});
    require(canvas->pointerGestureActive(), "real editor window owns an unfinished gesture");
    QEvent deactivate(QEvent::WindowDeactivate); QApplication::sendEvent(&window, &deactivate);
    require(!canvas->pointerGestureActive() && canvas->document().activeCount() == 0,
        "top-level WindowDeactivate ends and discards an unfinished gesture without relying on child FocusOut");
    canvas->setTool(UIQ::EditorTool::Annotation);
    drag(*canvas, {70, 70}, {240, 190});
    const auto before = canvas->document().findAnnotation(1)->annotationModel()->bounds();
    press(*canvas, {150, 70}); move(*canvas, {150, 90});
    const auto moved = canvas->document().findAnnotation(1)->annotationModel()->bounds();
    require(moved.top > before.top && canvas->pointerGestureActive(), "existing editor geometry is actively resizing");
    QApplication::sendEvent(&window, &deactivate);
    const auto kept = canvas->document().findAnnotation(1)->annotationModel()->bounds();
    require(!canvas->pointerGestureActive() && kept.top == moved.top && kept.bottom == moved.bottom,
        "top-level deactivation keeps the existing geometry's last valid result");
    std::cout << "PASS top-level deactivation cancels unfinished drawing and retains existing geometry\n";
}

void textFocusAndToolbar(const QString& directory)
{
    UIQ::EditorWindow window; load(window, pixels({520, 250}));
    require(window.iconFontAvailable(), "embedded legacy icon font loads from the retained static qrc");
    auto* canvas = window.canvas(); canvas->setTool(UIQ::EditorTool::Text);
    click(*canvas, {30, 45});
    auto* editor = window.findChild<QPlainTextEdit*>(QStringLiteral("shapeTextEditor"));
    require(editor, "shared shape editor exists");
    waitUntil([&] { return editor->hasFocus(); }, "actual ordinary text input owns keyboard focus");
    commit(*editor, QStringLiteral("中文输入 Aa"));
    const auto count = canvas->document().activeCount();
    key(*editor, Qt::Key_Return);
    commit(*editor, QStringLiteral("Second line"));
    require(editor->toPlainText().contains(QChar(u'\n')) && canvas->document().activeCount() == count,
        "Enter stays in text input without copying or closing");
    auto* bold = window.findChild<QToolButton*>(QStringLiteral("tool.bold"));
    require(bold && bold->focusPolicy() == Qt::NoFocus, "style controls do not take input focus");
    bold->click(); settle();
    require(canvas->textEditing() && editor->hasFocus() && canvas->toolStyle(UIQ::EditorTool::Text).bold,
        "style changes retain live text editing and native input focus");
    require(editor->font().weight() == QFont::Bold, "ordinary text preserves native Bold rather than annotation SemiBold");
    auto* slider = window.findChild<QSlider*>(QStringLiteral("toolValue"));
    require(slider, "shared style value slider"); slider->setValue(24);
    const auto* shape = canvas->document().activeItems().front().get();
    require(shape->textModel()->style().fontSize == 24 * canvas->devicePixelRatioF(), "font size applies once at source DPI");
    key(*editor, Qt::Key_A, Qt::ControlModifier); key(*editor, Qt::Key_C, Qt::ControlModifier);
    require(QGuiApplication::clipboard()->text() == editor->toPlainText(), "Ctrl+C copies text while text editor owns focus");
    key(*editor, Qt::Key_Escape);
    require(!canvas->textEditing() && window.isVisible(), "Escape finishes text editing without closing the editor window");
    click(*canvas, {45, 57});
    require(canvas->textEditing(), "existing text re-enters the shared input");
    canvas->setFocus(Qt::MouseFocusReason);
    // Exercise real QWidget focus delivery before the subsequent outside press.
    press(*canvas, {440, 200}); release(*canvas, {440, 200});
    require(!canvas->textEditing() && canvas->document().activeCount() == count,
        "one outside click only finishes the prior input after an early FocusOut");
    canvas->setTool(UIQ::EditorTool::Rectangle);
    auto rectangle = canvas->toolStyle(UIQ::EditorTool::Rectangle); rectangle.value = 2;
    canvas->setToolStyle(UIQ::EditorTool::Rectangle, rectangle);
    click(*canvas, {45, 57});
    require(canvas->textEditing() && shape->textModel()->style().fontSize == 24 * canvas->devicePixelRatioF(),
        "re-entering text from another tool cannot replace its font size with a 2px outline");
    auto explicitText = canvas->toolStyle(UIQ::EditorTool::Text); explicitText.value = 30;
    canvas->setToolStyle(UIQ::EditorTool::Text, explicitText);
    require(shape->textModel()->style().fontSize == 30 * canvas->devicePixelRatioF(),
        "explicit Text preferences still apply to the active shared input");
    canvas->finishTextEditing();
    QImage output; require(!canvas->renderExport(output), "complete ordinary text export");
    save(output, directory, QStringLiteral("text-style-input.png"));
    for (const auto tool : UIQ::editorTools) {
        auto* button = window.findChild<QToolButton*>(UIQ::toolLanguageKey(tool));
        require(button && button->width() == 44 && button->height() == 44 && button->iconSize() == QSize(24, 24),
            "shared toolbar preserves 44 DIP targets and 24 DIP icons");
    }
    require(window.findChildren<QPlainTextEdit*>().size() == 2, "one ordinary text input and one full-note input per document");
    std::cout << "PASS native text input, commit events, shortcut ownership and nonactivating 44/24 style controls\n";
}

void notesAndOutput(const QString& directory)
{
    UIQ::EditorWindow window; load(window, pixels({420, 260}));
    auto* canvas = window.canvas(); canvas->setTool(UIQ::EditorTool::Annotation);
    drag(*canvas, {20, 20}, {220, 170});
    auto* note = window.findChild<QPlainTextEdit*>(QStringLiteral("annotationNoteEditor"));
    require(note, "shared full note editor");
    waitUntil([&] { return note->hasFocus(); }, "completed new region focuses the shared note editor after release");
    const QString full = QStringLiteral("完整中文备注，保留全部内容。\nhttps://example.invalid/a/long/path?value=1234567890\n").repeated(60);
    commit(*note, full);
    require(QString::fromStdWString(canvas->document().findAnnotation(1)->annotationModel()->note()) == full,
        "shape owns the complete string, independently of row excerpt");
    const auto count = canvas->document().activeCount();
    key(*note, Qt::Key_A, Qt::ControlModifier); key(*note, Qt::Key_C, Qt::ControlModifier);
    require(QGuiApplication::clipboard()->text() == full && canvas->document().activeCount() == count,
        "note copy shortcut cannot copy/export the whole image");
    key(*note, Qt::Key_Delete);
    require(note->toPlainText().isEmpty() && canvas->document().activeCount() == count,
        "note Delete never deletes the shape");
    key(*note, Qt::Key_Z, Qt::ControlModifier);
    require(note->toPlainText() == full && canvas->document().activeCount() == count,
        "note undo belongs to native text document");
    auto* rectangle = window.findChild<QToolButton*>(QStringLiteral("tool.rect"));
    require(rectangle, "rectangle tool button exists");
    sendMouse(*rectangle, QEvent::MouseButtonPress, rectangle->rect().center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(*rectangle, QEvent::MouseButtonRelease, rectangle->rect().center(), Qt::LeftButton, Qt::NoButton);
    settle();
    require(canvas->tool() == UIQ::EditorTool::Rectangle && canvas->hasFocus() && !note->hasFocus(),
        "a real drawing-tool click finishes note focus and restores canvas shortcuts");
    require(note->toPlainText() == full &&
        QString::fromStdWString(canvas->document().findAnnotation(1)->annotationModel()->note()) == full,
        "tool changes preserve the complete committed note");
    note->setFocus(); settle();
    auto* fill = window.findChild<QToolButton*>(QStringLiteral("tool.rectFill"));
    require(fill, "rectangle style control exists");
    sendMouse(*fill, QEvent::MouseButtonPress, fill->rect().center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(*fill, QEvent::MouseButtonRelease, fill->rect().center(), Qt::LeftButton, Qt::NoButton);
    settle();
    require(note->hasFocus() && note->toPlainText() == full,
        "style-only toolbar clicks remain nonactivating while a note is focused");
    QImage complete; require(!canvas->renderExport(complete), "long CJK and URL notes export completely");
    require(complete.width() > canvas->originalImage().width() && complete.height() > canvas->originalImage().height(),
        "full notes expand the output rather than truncate or shrink the source");
    save(complete, directory, QStringLiteral("complete-notes.png"));
    QImage retained(3, 2, QImage::Format_ARGB32); retained.fill(qRgba(9, 33, 77, 111));
    const auto sentinel = retained.copy();
    QtAnnotationRenderer::Options tooSmall; tooSmall.maximumBytes = 4;
    require(static_cast<bool>(canvas->renderExport(retained, tooSmall)) && sameBytes(retained, sentinel),
        "an impossible export budget leaves the earlier output byte-exact");
    require(canvas->document().activeCount() == count && QString::fromStdWString(canvas->document().findAnnotation(1)->annotationModel()->note()) == full,
        "failed full export preserves all edits and complete notes");
    struct Request { quint64 token; UIQ::OutputIntent intent; QImage image; };
    std::vector<Request> requests;
    QObject::connect(&window, &UIQ::EditorWindow::outputRequested, &window,
        [&](quint64 token, UIQ::OutputIntent intent, const QImage& image) { requests.push_back({token, intent, image}); });
    require(!window.requestOutput(UIQ::OutputIntent::Clipboard) && requests.empty(), "unwired output never pretends success");
    window.setOutputEnabled(UIQ::OutputIntent::Clipboard, true);
    window.setOutputEnabled(UIQ::OutputIntent::SaveAs, true);
    window.setOutputEnabled(UIQ::OutputIntent::Archive, true);
    window.setOutputEnabled(UIQ::OutputIntent::Pin, true);
    require(window.requestOutput(UIQ::OutputIntent::Clipboard) && window.outputPending(), "owned async output request");
    require(!canvas->interactionEnabled() && requests.back().image.size() == complete.size(), "pending output uses final full snapshot and locks mutations");
    const auto first = requests.back().token;
    window.completeOutput(first + 100, UIQ::OutputResult::Succeeded);
    require(window.outputPending(), "unrelated completion token is ignored");
    window.completeOutput(first, UIQ::OutputResult::Failed, QStringLiteral("Disk full"));
    require(!window.outputPending() && window.isVisible() && canvas->interactionEnabled(), "failure returns the intact editor to use");
    require(window.findChild<QLabel*>(QStringLiteral("editorStatus"))->text() == QStringLiteral("Disk full"), "host failure is visible");
    window.completeOutput(first, UIQ::OutputResult::Succeeded);
    require(window.isVisible(), "late success for a failed operation cannot close the document");
    note->setFocus(); key(*note, Qt::Key_S, Qt::ControlModifier);
    require(window.outputPending() && requests.back().intent == UIQ::OutputIntent::SaveAs,
        "explicit Ctrl+S remains available while writing a note");
    window.completeOutput(requests.back().token, UIQ::OutputResult::Cancelled);
    require(window.isVisible() && !window.outputPending(), "cancelled save retains the window and all text");
    window.setCloseOnSuccessfulOutput(false);
    require(window.requestOutput(UIQ::OutputIntent::Pin), "real pin intent reaches the host");
    window.completeOutput(requests.back().token, UIQ::OutputResult::Succeeded);
    require(window.isVisible(), "host can keep a pinned editor open after successful output");
    if (!directory.isEmpty()) require(window.grab().save(QDir(directory).filePath(QStringLiteral("editor-window.png"))), "real QWidget PNG capture");
    window.setCloseOnSuccessfulOutput(true);
    require(window.requestOutput(UIQ::OutputIntent::Archive), "completion/archive intent reaches host");
    window.completeOutput(requests.back().token, UIQ::OutputResult::Succeeded);
    require(!window.isVisible(), "capture completion closes only after host-confirmed success");
    std::cout << "PASS full notes, keyboard isolation, all-or-nothing rendering and asynchronous success/failure/cancel tokens\n";
}

void zoomAndPureImage()
{
    UIQ::EditorWindow window; load(window, pixels({1200, 900}));
    auto* canvas = window.canvas(); canvas->setTool(UIQ::EditorTool::Rectangle);
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("editorScrollArea"));
    require(scroll, "source canvas scrolling view");
    canvas->setZoom(1); settle();
    scroll->horizontalScrollBar()->setValue(100); scroll->verticalScrollBar()->setValue(80);
    const QPointF anchor(250, 200);
    const auto oldValue = QPointF(scroll->horizontalScrollBar()->value(), scroll->verticalScrollBar()->value());
    const auto physicalPoint = canvas->viewToSource(anchor);
    canvas->setZoom(1.1, anchor); settle();
    const QPointF viewportAfter = canvas->sourceToView(physicalPoint) - QPointF(scroll->horizontalScrollBar()->value(), scroll->verticalScrollBar()->value());
    require(QLineF(viewportAfter, anchor - oldValue).length() <= 2, "zoom preserves the pointer's source-pixel anchor with existing scroll offsets");
    sendMouse(*canvas, QEvent::MouseButtonPress, {25, 25}, Qt::RightButton, Qt::RightButton); settle();
    require(canvas->pureImageMode() && canvas->tool() == UIQ::EditorTool::None,
        "right click enters pure-image mode and clears the drawing tool");
    sendMouse(*canvas, QEvent::MouseButtonPress, {25, 25}, Qt::RightButton, Qt::RightButton); settle();
    require(!canvas->pureImageMode(), "right click restores editing chrome");
    std::cout << "PASS pointer-anchored zoom and shared pure-image interaction\n";
}

void selectionChromeAndWheel()
{
    QImage source(300, 220, QImage::Format_RGB32); source.fill(Qt::white);
    UIQ::EditorCanvas canvas; load(canvas, source); canvas.setTool(UIQ::EditorTool::Annotation);
    drag(canvas, {70, 50}, {210, 160});
    QEvent leave(QEvent::Leave); QApplication::sendEvent(&canvas, &leave);
    canvas.setAnnotationPanelVisible(false);
    const auto unselected = canvas.grab().toImage();
    canvas.setAnnotationPanelVisible(true);
    const auto selected = canvas.grab().toImage();
    require(!sameBytes(selected, unselected), "selected annotation keeps real handles after the pointer enters the note panel");
    press(canvas, {140, 50});
    require(canvas.pointerGestureActive(), "existing annotation begins a captured resize");
    require(sameBytes(canvas.grab().toImage(), unselected), "hover handles stay out of the active gesture paint");
    QEvent ungrab(QEvent::UngrabMouse); QApplication::sendEvent(&canvas, &ungrab);

    canvas.setTool(UIQ::EditorTool::Number); click(canvas, {40, 100});
    const auto* marker = canvas.document().activeItems().back().get();
    const auto& model = std::get<Editor::NumberModel>(marker->geometry());
    const auto middle = model.outline().middle;
    const QPointF handle(middle.x, middle.y);
    const auto radius = model.radius();
    sendMouse(canvas, QEvent::MouseMove, canvas.sourceToView(handle), Qt::NoButton, Qt::NoButton);
    for (int i = 0; i < 6; ++i) wheel(canvas, handle);
    require(model.radius() == radius + 6, "continuous wheel resizing retains its moving handle target until pointer motion");
    const auto wheelPreview = canvas.grab().toImage();
    sendMouse(canvas, QEvent::MouseMove, canvas.sourceToView({40, 100}), Qt::NoButton, Qt::NoButton);
    require(!sameBytes(canvas.grab().toImage(), wheelPreview), "number handles return on pointer motion after wheel adjustment");

    UIQ::EditorWindow window; load(window, source); window.canvas()->setTool(UIQ::EditorTool::Annotation);
    drag(*window.canvas(), {70, 50}, {210, 160});
    auto* collapse = window.findChild<QToolButton*>(QStringLiteral("annotationCollapse"));
    auto* panel = window.findChild<QWidget*>(QStringLiteral("annotationPanel"));
    require(collapse && panel && !panel->isHidden(), "shared annotation rail is open");
    collapse->click(); settle();
    require(panel->isHidden() && window.canvas()->tool() == UIQ::EditorTool::None &&
        window.canvas()->document().activeCount() == 1, "collapse returns to pin movement without removing any annotation");
    std::cout << "PASS selected-region chrome, capture paint, continuous number wheel and annotation collapse\n";
}

void noteCaptionLayout(const QString& directory)
{
    UIQ::EditorWindow window; window.setTranslator(text);
    require(!window.loadImage(pixels({420, 340})), "caption layout source image");
    show(window); window.canvas()->setTool(UIQ::EditorTool::Annotation);
    drag(*window.canvas(), {30, 30}, {180, 150}); settle();
    auto* panel = window.findChild<QWidget*>(QStringLiteral("annotationPanel"));
    auto* footer = window.findChild<QLabel*>(QStringLiteral("annotationFooter"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("annotationFooterScroll"));
    auto* input = window.findChild<QPlainTextEdit*>(QStringLiteral("annotationNoteEditor"));
    auto* heading = window.findChild<QLabel*>(QStringLiteral("annotationHeading"));
    require(panel && footer && scroll && input && heading, "caption layout controls");
    require(window.font().pixelSize() == 14 && window.font().weight() == QFont::Normal &&
        heading->font().pixelSize() == 13 && footer->font().pixelSize() == 12 && input->font().pixelSize() == 15,
        "editor chrome uses explicit logical-pixel font sizes on both platforms");
    for (const int width : {240, 340}) {
        panel->setFixedWidth(width); window.resize(800, 520); settle();
        require(footer->wordWrap() && footer->height() >= footer->heightForWidth(footer->width()),
            "normal full footer is laid out at its actual font and width");
        require(scroll->verticalScrollBar()->maximum() == 0 &&
            input->geometry().bottom() < scroll->geometry().top() &&
            panel->rect().contains(scroll->geometry()), "wrapped footer remains visible without overlapping note input");
        if (!directory.isEmpty()) require(window.grab().save(QDir(directory).filePath(
            QStringLiteral("annotation-caption-%1.png").arg(width))), "wrapped footer PNG evidence");
    }
    const auto full = QStringLiteral("完整快捷键提示，Ctrl+S saves the image and every complete note. ").repeated(80);
    window.setTranslator([&](QStringView key) { return key == u"annotation.saveHint" ? full : text(key); });
    panel->setFixedWidth(240); window.resize(800, 520); settle();
    require(footer->text() == full && footer->height() >= footer->heightForWidth(footer->width()),
        "long custom translation remains complete in the real QLabel");
    require(scroll->height() <= 72 && scroll->verticalScrollBar()->maximum() > 0 &&
        input->geometry().bottom() < scroll->geometry().top() && input->height() >= 72,
        "exceptionally long footer scrolls within budget while preserving input height");
    const QFontInfo actual(footer->font());
    std::cout << "FONT editor caption: " << actual.family().toStdString() << ", pixel="
              << actual.pixelSize() << ", weight=" << actual.weight() << '\n';
    std::cout << "PASS actual-width caption wrapping, explicit chrome fonts and bounded long translations\n";
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    try {
        if (!directory.isEmpty()) require(QDir().mkpath(directory), "create image evidence directory");
        const QFontInfo info(app.font());
        std::cout << "FONT " << info.family().toStdString() << " / " << info.pixelSize() << " px; QPA "
                  << QGuiApplication::platformName().toStdString() << '\n';
        sourceOwnershipAndScale(); nineToolRendering(directory); historyAndCaptureLoss(); windowDeactivation();
        textFocusAndToolbar(directory); notesAndOutput(directory); zoomAndPureImage(); selectionChromeAndWheel(); noteCaptionLayout(directory);
        std::cout << "PASS real Qt editor component contracts (physical multi-monitor/IME sessions remain separate acceptance)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
