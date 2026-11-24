#include "pch.h"
#include "ui/AnnotationPanel.h"
#include "ui/AnnotationPanelLayout.h"
#include "editor/WinPin.h"
#include "services/Lang.h"

#include <algorithm>
#include <cmath>
#include <climits>

using Microsoft::WRL::ComPtr;

namespace {
    bool inside(D2D1_RECT_F rect, float x, float y) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }
}

AnnotationPanel::AnnotationPanel(Ling::WinBase* window)
    : Ling::Node(window), owner(static_cast<WinPin*>(window))
{
    setPositionType(Ling::Position::Absolute);
    setPosition(Ling::Edge::Top, 0.f);
    setBg(0xF8FAFCFF);
    canvas = makeChild<Ling::Canvas>();
    canvas->setPositionType(Ling::Position::Absolute);
    canvas->setSizePercent(100.f, 100.f);
    canvas->enableSwapChain();

    editor = makeChild<Ling::TextBox>();
    editor->setPositionType(Ling::Position::Absolute);
    editor->setFontSize(15.f);
    editor->setFontFamily(L"Segoe UI");
    editor->setPadding(10.f);
    editor->setBg(0xFFFFFFFF);
    editor->setColor(0x172033FF);
    editor->setCaretColor(0x2563EBFF);
    editor->setSelectionBgColor(0xBFDBFEAA);
    editor->setBorder(1.f, 0xCBD5E1FF);
    editor->setBorderRadius(7.f);
    editor->setPlaceholder(Lang::get(L"annotation.placeholder"));
    textToken = editor->onTextChanged.add([this](Ling::TextBox*, const std::wstring& value) {
        if (changingText || !owner || !selected) return;
        owner->updateAnnotationNote(selected, value);
    });
    focusToken = editor->onFocusChanged.add([this](Ling::TextBox*, bool focused) {
        editor->setBorderColor(focused ? 0x2563EBFF : 0xCBD5E1FF);
    });
    const auto lifetime = getWeakThis();
    blurToken = win->onBlur.add([this, lifetime]() {
        if (lifetime.lock()) cancelPointerGesture();
    });
    setPanelVisible(false);
}

AnnotationPanel::~AnnotationPanel()
{
    detachOwner();
    win->onBlur.remove(blurToken);
    editor->onTextChanged.remove(textToken);
    editor->onFocusChanged.remove(focusToken);
}

void AnnotationPanel::detachOwner()
{
    cancelPointerGesture();
    owner = nullptr;
    if (editor) editor->blur();
}

void AnnotationPanel::setPanelVisible(bool visible)
{
    panelVisible = visible;
    if (visible) {
        show();
        updateEditor();
    } else {
        cancelPointerGesture();
        finishEditing();
        editor->hide(); // Ling inputs check their own visibility, not ancestors.
        hide();
    }
}

void AnnotationPanel::setBounds(float left, float width, float height)
{
    const float d = win->dpi;
    const bool resized = width != requestedWidth || height != requestedHeight || cachedDpi != d;
    if (left == requestedLeft && !resized) return;
    const bool remeasure = width != requestedWidth || cachedDpi != d;
    requestedLeft = left;
    requestedWidth = width;
    requestedHeight = height;
    setPosition(Ling::Edge::Left, left / d);
    setSize(width / d, height / d);
    // The owner calls this before its Yoga pass. Update input coordinates as
    // well: another input callback may run in the same mouse event before paint.
    x = left;
    y = 0.f;
    w = width;
    h = height;
    const auto geometry = HuhuCapture::UI::annotationPanelGeometry(height, d);
    listTop = geometry.listTop;
    listBottom = geometry.listBottom;
    editorHeadingTop = geometry.editorHeadingTop;
    editorTop = geometry.editorTop;
    editorHeight = geometry.editorHeight;
    footerTop = geometry.footerTop;
    synchronizeEditorGeometry();
    cachedDpi = d;
    if (remeasure) {
        for (auto& row : rows) { row.text.Reset(); row.number.Reset(); }
        chromeDirty = true;
    }
    if (resized) layoutsDirty = true;
    // Hidden input nodes have zero Yoga geometry. Show only after valid bounds
    // and a matching selected row are ready, never at the TextBox default size.
    if (panelVisible && selected) updateEditor();
}

void AnnotationPanel::synchronizeEditorGeometry()
{
    if (requestedWidth <= 0.f || requestedHeight <= 0.f) return;
    const float d = win->dpi;
    const float width = std::max(1.f, requestedWidth - 28.f * d);
    editor->setPosition(Ling::Edge::Left, 14.f);
    editor->setPosition(Ling::Edge::Top, editorTop / d);
    editor->setSize(width / d, editorHeight / d);
    editor->x = requestedLeft + 14.f * d;
    editor->y = editorTop;
    editor->w = width;
    editor->h = editorHeight;
}

void AnnotationPanel::sync(std::vector<HuhuCapture::Core::AnnotationNote> notes, std::uint64_t active)
{
    cancelPointerGesture();
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.number < b.number; });
    const auto previousCount = rows.size();
    const auto previousSelection = selected;
    std::vector<Row> nextRows;
    nextRows.reserve(notes.size());
    std::size_t oldIndex = 0;
    for (auto& note : notes) {
        while (oldIndex < rows.size() && rows[oldIndex].note.number < note.number) ++oldIndex;
        Row row;
        if (oldIndex < rows.size() && rows[oldIndex].note.number == note.number) {
            row = std::move(rows[oldIndex++]);
            if (row.note.note != note.note) row.text.Reset();
        }
        row.note = std::move(note);
        nextRows.push_back(std::move(row));
    }
    rows = std::move(nextRows);
    selected = active;
    if (std::none_of(rows.begin(), rows.end(), [this](const Row& row) { return row.note.number == selected; }))
        selected = rows.empty() ? 0 : rows.back().note.number;
    hoveredRow = 0;
    layoutsDirty = true;
    chromeDirty = chromeDirty || previousCount != rows.size() || previousSelection != selected;
    revealSelected = true;
    updateEditor();
    win->refresh();
}

void AnnotationPanel::select(std::uint64_t number, bool focus)
{
    if (selected != number) {
        selected = number;
        layoutsDirty = true;
        chromeDirty = true;
        updateEditor();
    }
    revealSelected = true;
    keepSelectedVisible();
    if (focus) focusEditor();
    win->refresh();
}

void AnnotationPanel::updateNote(std::uint64_t number, const std::wstring& text)
{
    for (auto& row : rows) {
        if (row.note.number != number) continue;
        if (row.note.note == text) return;
        row.note.note = text;
        row.text.Reset();
        layoutsDirty = true;
        revealSelected = number == selected;
        break;
    }
    win->refresh();
}

void AnnotationPanel::updateEditor()
{
    const auto row = std::find_if(rows.begin(), rows.end(), [this](const Row& value) { return value.note.number == selected; });
    if (row == rows.end()) {
        editor->blur();
        editor->hide();
        return;
    }
    changingText = true;
    if (editor->getText() != row->note.note) editor->setText(row->note.note);
    changingText = false;
    synchronizeEditorGeometry();
    if (panelVisible && requestedWidth > 0.f && requestedHeight > 0.f) editor->show();
    else editor->hide();
}

void AnnotationPanel::focusEditor()
{
    if (!panelVisible || !selected || !owner || requestedWidth <= 0.f || requestedHeight <= 0.f) return;
    synchronizeEditorGeometry();
    editor->show();
    editor->focus();
}

void AnnotationPanel::finishEditing() { if (editor) editor->blur(); }
bool AnnotationPanel::isEditing() const { return panelVisible && editor && editor->isFocused(); }

bool AnnotationPanel::contains(POINT point) const
{
    return panelVisible && point.x >= x && point.x < x + w && point.y >= y && point.y < y + h;
}

ComPtr<IDWriteTextLayout> AnnotationPanel::makeText(const std::wstring& text,
    float fontSize, float width, float height, bool center) const
{
    ComPtr<IDWriteTextLayout> result;
    auto d2d = Ling::D2D::get();
    if (FAILED(d2d->dwriteFactory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
        d2d->baseTextFormat.Get(), std::max(1.f, width), std::max(1.f, height), &result))) return result;
    result->SetFontSize(fontSize, {0, static_cast<UINT32>(text.size())});
    result->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (center) {
        result->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        result->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return result;
}

void AnnotationPanel::layout()
{
    // Child Yoga styles have already been prepared by setBounds. Changing them
    // here would be one frame too late: WinBase calculates Yoga before this call.
    Ling::Node::layout();
    if (!panelVisible) return;
    if (layoutsDirty || chromeDirty) rebuildLayouts();
    paint();
}

void AnnotationPanel::prepareHitLayout()
{
    // Input can arrive before WM_PAINT after undo/removal or a DPI change.
    // Resolve pending row heights before interpreting that input's position.
    if (panelVisible && requestedWidth > 0.f && requestedHeight > 0.f &&
        (layoutsDirty || chromeDirty)) rebuildLayouts();
}

void AnnotationPanel::rebuildLayouts()
{
    const float d = win->dpi;
    std::size_t digits = 1;
    for (const auto& row : rows) digits = std::max(digits, std::to_wstring(row.note.number).size());
    const float nextBadgeWidth = std::max(32.f, 15.f + static_cast<float>(digits) * 8.f) * d;
    const float textW = std::max(20.f * d, w - 54.f * d - nextBadgeWidth);
    if (nextBadgeWidth != badgeWidth || textW != cachedTextWidth) {
        for (auto& row : rows) { row.text.Reset(); row.number.Reset(); }
    }
    badgeWidth = nextBadgeWidth;
    cachedTextWidth = textW;
    totalRowsHeight = 0.f;
    for (auto& row : rows) {
        // Typing invalidates only this row's text. Other DirectWrite objects and
        // all unchanged number badges survive; reflow is simple O(N) arithmetic.
        if (!row.text) {
            const auto& value = row.note.note.empty() ? Lang::get(L"annotation.empty") : row.note.note;
            row.text = makeText(value, 14.f * d, textW, 54.f * d);
            DWRITE_TEXT_METRICS metrics{};
            if (row.text) row.text->GetMetrics(&metrics);
            row.height = std::max(48.f * d, std::min(metrics.height, 54.f * d) + 22.f * d);
        }
        if (!row.number)
            row.number = makeText(std::to_wstring(row.note.number), 13.f * d, badgeWidth, 30.f * d, true);
        row.top = totalRowsHeight;
        totalRowsHeight += row.height + 7.f * d;
    }
    if (!rows.empty()) totalRowsHeight -= 7.f * d;
    if (chromeDirty) {
        titleText = makeText(Lang::get(L"annotation.title") + L" · " + std::to_wstring(rows.size()),
            17.f * d, w - 84.f * d, 32.f * d);
        collapseText = makeText(Lang::get(L"annotation.collapse"), 12.f * d, 48.f * d, 40.f * d, true);
        emptyText = makeText(Lang::get(L"annotation.addHint"), 14.f * d, w - 40.f * d, 100.f * d, true);
        editTitle = makeText(selected ? Lang::get(L"annotation.edit") + L" #" + std::to_wstring(selected) : L"",
            13.f * d, w - 90.f * d, 30.f * d);
        deleteText = makeText(Lang::get(L"annotation.remove"), 12.f * d, 48.f * d, 34.f * d, true);
        footerText = makeText(Lang::get(L"annotation.saveHint"), 11.f * d, w - 28.f * d, 24.f * d);
        chromeDirty = false;
    }
    layoutsDirty = false;
    setScroll(scrollY);
    if (revealSelected) {
        revealSelected = false;
        keepSelectedVisible();
    }
}

D2D1_RECT_F AnnotationPanel::listRect() const
{
    return D2D1::RectF(10.f * win->dpi, listTop, w - 10.f * win->dpi, listBottom);
}

float AnnotationPanel::maximumScroll() const { return std::max(0.f, totalRowsHeight - (listBottom - listTop)); }

void AnnotationPanel::setScroll(float value)
{
    const auto next = std::clamp(value, 0.f, maximumScroll());
    if (next == scrollY) return;
    scrollY = next;
    win->refresh();
}

void AnnotationPanel::keepSelectedVisible()
{
    if (layoutsDirty) return; // next layout rebuilds before the following click/paint.
    for (const auto& row : rows) {
        if (row.note.number != selected) continue;
        const auto height = listBottom - listTop;
        // A compact rail may show only part of a tall row. Keep its beginning
        // visible instead of alternating between top/bottom on each keystroke.
        if (row.height >= height || row.top < scrollY) setScroll(row.top);
        else if (row.top + row.height > scrollY + height) setScroll(row.top + row.height - height);
        break;
    }
}

D2D1_RECT_F AnnotationPanel::scrollThumb() const
{
    const float viewport = listBottom - listTop;
    if (maximumScroll() <= 0.f || viewport <= 0.f) return D2D1::RectF();
    const float height = std::min(viewport, std::max(24.f * win->dpi, viewport * viewport / totalRowsHeight));
    const float top = listTop + (viewport - height) * scrollY / maximumScroll();
    return D2D1::RectF(w - 8.f * win->dpi, top, w - 3.f * win->dpi, top + height);
}

std::uint64_t AnnotationPanel::rowAt(POINT point) const
{
    const float px = point.x - x, py = point.y - y;
    if (!contains(point) || !inside(listRect(), px, py)) return 0;
    const float contentY = py - listTop + scrollY;
    for (const auto& row : rows)
        if (contentY >= row.top && contentY < row.top + row.height) return row.note.number;
    return 0;
}

void AnnotationPanel::pointerDown(POINT point, bool right)
{
    if (!contains(point) || right || !owner) return;
    prepareHitLayout();
    const float px = point.x - x, py = point.y - y, d = win->dpi;
    if (inside(D2D1::RectF(w - 62.f * d, 4.f * d, w - 8.f * d, 48.f * d), px, py)) {
        owner->hideAnnotationPanel();
        return;
    }
    if (selected && inside(D2D1::RectF(w - 62.f * d, editorHeadingTop, w - 8.f * d, editorTop), px, py)) {
        const auto number = selected;
        finishEditing();
        owner->removeAnnotation(number);
        return;
    }
    if (maximumScroll() > 0.f && px >= w - 12.f * d && py >= listTop && py <= listBottom) {
        const auto thumb = scrollThumb();
        if (!inside(thumb, px, py)) {
            const float available = (listBottom - listTop) - (thumb.bottom - thumb.top);
            if (available > 0.f) setScroll((py - listTop - (thumb.bottom - thumb.top) / 2.f) * maximumScroll() / available);
        }
        draggingScrollbar = true;
        scrollbarDragY = py;
        scrollbarDragOffset = scrollY;
        SetCapture(win->hwnd);
        return;
    }
    const auto pressed = rowAt(point);
    if (pressed) owner->selectAnnotation(pressed, false);
    pressedRow = pressed;
    if (pressedRow) SetCapture(win->hwnd);
}

void AnnotationPanel::cancelPointerGesture()
{
    const bool hadCapture = draggingScrollbar || pressedRow != 0;
    draggingScrollbar = false;
    pressedRow = 0;
    if (hadCapture && GetCapture() == win->hwnd) ReleaseCapture();
}

void AnnotationPanel::pointerMove(POINT point)
{
    if ((draggingScrollbar || pressedRow) &&
        (GetCapture() != win->hwnd || !(GetKeyState(VK_LBUTTON) & 0x8000))) cancelPointerGesture();
    if (!panelVisible) return;
    // Ling broadcasts this sentinel on WM_MOUSELEAVE; it is not a drag position.
    if (point.x == INT_MAX && point.y == INT_MAX) {
        if (hoveredRow) { hoveredRow = 0; win->refresh(); }
        return;
    }
    prepareHitLayout();
    if (draggingScrollbar) {
        const auto thumb = scrollThumb();
        const float available = (listBottom - listTop) - (thumb.bottom - thumb.top);
        if (available > 0.f) setScroll(scrollbarDragOffset + (point.y - y - scrollbarDragY) * maximumScroll() / available);
        return;
    }
    const auto hovered = rowAt(point);
    if (hovered != hoveredRow) { hoveredRow = hovered; win->refresh(); }
}

void AnnotationPanel::pointerUp(POINT point, bool right)
{
    if (right) return;
    prepareHitLayout();
    const auto pressed = pressedRow;
    cancelPointerGesture();
    if (pressed && pressed == rowAt(point)) focusEditor();
}

void AnnotationPanel::wheel(POINT point, float distance)
{
    prepareHitLayout();
    if (contains(point) && point.y - y >= listTop && point.y - y < listBottom) setScroll(scrollY - distance);
}

bool AnnotationPanel::cursor()
{
    if (!panelVisible) return false;
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(win->hwnd, &point);
    if (!contains(point)) return false;
    bool handled = false;
    win->onCursor(&handled); // TextBox supplies its I-beam/scrollbar cursor.
    if (!handled) SetCursor(LoadCursor(nullptr, rowAt(point) ? IDC_HAND : IDC_ARROW));
    return true;
}

void AnnotationPanel::paint()
{
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    if (!brush && FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(0x172033), &brush))) {
        canvas->finishPaint();
        return;
    }
    const float d = win->dpi;
    const auto color = [&](UINT32 rgb) { brush->SetColor(D2D1::ColorF(rgb)); };
    ctx->Clear(D2D1::ColorF(0xF8FAFC));
    color(0xCBD5E1);
    ctx->DrawLine({.5f * d, 0}, {.5f * d, h}, brush.Get(), d);
    color(0x172033);
    if (titleText) ctx->DrawTextLayout({16.f * d, 16.f * d}, titleText.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    color(0x64748B);
    if (collapseText) ctx->DrawTextLayout({w - 60.f * d, 5.f * d}, collapseText.Get(), brush.Get());
    ctx->PushAxisAlignedClip(listRect(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (rows.empty()) {
        if (emptyText) ctx->DrawTextLayout({20.f * d, listTop + 18.f * d}, emptyText.Get(), brush.Get());
    } else {
        for (const auto& row : rows) {
            const float top = listTop + row.top - scrollY;
            if (top + row.height < listTop || top > listBottom) continue;
            const auto box = D2D1::RoundedRect(D2D1::RectF(12.f * d, top, w - 14.f * d, top + row.height), 7.f * d, 7.f * d);
            color(row.note.number == selected ? 0xEFF6FF : row.note.number == hoveredRow ? 0xF1F5F9 : 0xFFFFFF);
            ctx->FillRoundedRectangle(box, brush.Get());
            color(row.note.number == selected ? 0x93C5FD : 0xE2E8F0);
            ctx->DrawRoundedRectangle(box, brush.Get(), d);
            const auto palette = HuhuCapture::Core::annotationColor(row.note.number);
            const float badgeTop = top + 10.f * d;
            color(palette.backgroundRgb);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(22.f * d, badgeTop,
                22.f * d + badgeWidth, badgeTop + 30.f * d), 15.f * d, 15.f * d), brush.Get());
            color(palette.textRgb);
            if (row.number) ctx->DrawTextLayout({22.f * d, badgeTop}, row.number.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            color(row.note.note.empty() ? 0x94A3B8 : 0x334155);
            if (row.text) ctx->DrawTextLayout({32.f * d + badgeWidth, top + 10.f * d}, row.text.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    ctx->PopAxisAlignedClip();
    if (maximumScroll() > 0.f) {
        color(0x94A3B8);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(scrollThumb(), 2.5f * d, 2.5f * d), brush.Get());
    }
    if (selected) {
        color(0x334155);
        if (editTitle) ctx->DrawTextLayout({14.f * d, editorTop - 27.f * d}, editTitle.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        color(0xB91C1C);
        if (deleteText) ctx->DrawTextLayout({w - 60.f * d, editorHeadingTop}, deleteText.Get(), brush.Get());
    }
    color(0x64748B);
    if (footerText) ctx->DrawTextLayout({14.f * d, footerTop}, footerText.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    canvas->finishPaint();
}
