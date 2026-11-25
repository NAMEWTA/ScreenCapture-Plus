#include "pch.h"
#include "app/App.h"
#include "editor/History.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/shapes/ShapeText.h"

using Microsoft::WRL::ComPtr;

ShapeText::ShapeText(WinPin* win) : ShapeBase(win), model_(6.f * win->dpi)
{
	setAttr();
	// 虚线框：2 实 2 虚，与 2.4.25 一致
	float dashes[] = { 2.f, 2.f };
	Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_FLAT,
			D2D1_CAP_STYLE_FLAT,
			D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_MITER,
			10.f,
			D2D1_DASH_STYLE_CUSTOM,
			0.f
		),
		dashes, ARRAYSIZE(dashes), dashedStrokeStyle.GetAddressOf());
}

ShapeText::~ShapeText()
{

}

void ShapeText::paint(ID2D1DeviceContext* ctx)
{
    if (model_.isEditing()) {
        // The shared input owns caret/selection/IME and draws the live text.
        // Its actual measured bounds are converted back to original pixels.
        const auto* editor = win->getTextBox();
        const float scale = win->scale;
        model_.setLayoutBounds({editor->x / scale, editor->y / scale,
            (editor->x + editor->w) / scale, (editor->y + editor->h) / scale});
        return;
    }
    if (!textLayout) return;
    const auto& bounds = model_.bounds();
    ctx->DrawTextLayout({bounds.left + model_.padding(), bounds.top + model_.padding()},
        textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void ShapeText::paintDragger(ID2D1DeviceContext* ctx)
{
    // Editing chrome is deliberately outside paint(), so it never enters an export.
    const auto& bounds = model_.bounds();
    ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
        textBrush.Get(), win->dpi, dashedStrokeStyle.Get());
}

void ShapeText::mouseDrag(const float x, const float y)
{
    model_.drag({x, y});
}

void ShapeText::mouseDown(const float x, const float y)
{
    using Handle = HuhuCapture::Editor::TextHandle;
    const auto handle = static_cast<Handle>(hoverDraggerIndex);
    if (handle == Handle::None) {
        if (!model_.place({x, y})) return;
        hoverDraggerIndex = static_cast<int>(Handle::Edit);
        startEdit();
    } else if (handle == Handle::Edit) {
        startEdit();
    } else if (handle == Handle::Move) {
        model_.beginMove({x, y});
        finishEdit();
    } else if (handle == Handle::FinishEditing) {
        finishEdit();
    }
}

void ShapeText::mouseUp(const float x, const float y)
{
    model_.finishGesture();
}

void ShapeText::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = static_cast<int>(model_.hitTest({x, y}, win->dpi));
}

void ShapeText::setCursor()
{
	if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
	else if (hoverDraggerIndex == 1) {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	}
	else if (hoverDraggerIndex == 0) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
}

void ShapeText::startEdit()
{
    if (!model_.startEditing()) return;
    // Reentry applies the currently selected tool style, as the native editor did.
    setAttr();
    const auto& bounds = model_.bounds();
    const auto& style = model_.style();
    auto* editor = win->getTextBox();
    const float dpi = win->dpi, scale = win->scale;
    editor->setPosition(Ling::Edge::Left, bounds.left * scale / dpi);
    editor->setPosition(Ling::Edge::Top, bounds.top * scale / dpi);
    // Set the immediate hit-test box before the queued Yoga layout runs. Stale
    // bounds here used to make a newly focused shared input immediately blur.
    editor->x = bounds.left * scale;
    editor->y = bounds.top * scale;
    editor->w = (bounds.right - bounds.left) * scale;
    editor->h = (bounds.bottom - bounds.top) * scale;
    editor->setColor(Ling::Color(style.rgba));
    editor->setCaretColor(Ling::Color(style.rgba));
    editor->setPadding(model_.padding() * scale / dpi);
    editor->setFontSize(style.fontSize * scale / dpi);
    editor->setBold(style.bold);
    editor->setItalic(style.italic);
    editor->setText(model_.text());
    editor->show();
    // Subscribe after setText: initial binding must not be mistaken for user input.
    textChangedTok = editor->onTextChanged.add([this](Ling::TextBox*, const std::wstring& value) {
        model_.setText(value);
        win->refresh();
    });
    focusTok = editor->onFocusChanged.add([this](Ling::TextBox*, bool focused) {
        if (!focused) finishEdit();
    });
    win->setEditingText(this);
    editor->focus();
    win->refresh();
}

void ShapeText::finishEdit()
{
    if (!model_.finishEditing()) return;
    auto* editor = win->getTextBox();
    // Detach before blur, which emits another focus event. The next text shape
    // must never inherit these subscriptions to this history-owned object.
    editor->onTextChanged.remove(textChangedTok);
    editor->onFocusChanged.remove(focusTok);
    textChangedTok = {};
    focusTok = {};
    model_.setText(editor->getText());
    editor->blur();
    editor->hide();
    win->setEditingText(nullptr);
    makeTextLayout();
    win->refresh();
    if (model_.text().empty())
        win->history->removeShapeLater(this); // Resolve the stable ticket after this callback returns.
}

void ShapeText::applyStyle()
{
    if (!model_.isEditing()) return;
    setAttr();
    const auto& style = model_.style();
    auto* editor = win->getTextBox();
    editor->setColor(Ling::Color(style.rgba));
    editor->setCaretColor(Ling::Color(style.rgba));
    editor->setFontSize(style.fontSize * win->scale / win->dpi);
    editor->setBold(style.bold);
    editor->setItalic(style.italic);
    win->refresh();
}

void ShapeText::makeTextLayout()
{
    const auto& style = model_.style();
    // Screen-text shapes do not soft-wrap, matching the auto-size shared input.
    textLayout = Ling::D2D::get()->makeTextLayout(model_.text(), style.fontSize);
    if (!textLayout) return;
    textLayout->SetFontWeight(style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, {0, INT_MAX});
    textLayout->SetFontStyle(style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, {0, INT_MAX});
}

void ShapeText::setAttr()
{
    auto* tool = win->toolSub.get();
    model_.setStyle({tool->getSliderVal(), tool->getSelectedColorValue(), tool->isTextBold, tool->isTextItalic});
    Ling::D2D::get()->deviceContext->CreateSolidColorBrush(tool->getSelectedColor(), textBrush.ReleaseAndGetAddressOf());
}
