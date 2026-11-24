#pragma once

#include <include/Ling.h>
#include "core/Annotation.h"

class WinPin;

// A docked view of the screenshot's annotation shapes. It owns no annotation
// data: a single shared TextBox edits the selected shape, and the list renders
// lightweight cached text layouts instead of creating one input per note.
class AnnotationPanel final : public Ling::Node {
public:
    explicit AnnotationPanel(Ling::WinBase* window);
    ~AnnotationPanel() override;

    void setPanelVisible(bool visible);
    void setBounds(float left, float width, float height); // physical window pixels
    void sync(std::vector<HuhuCapture::Core::AnnotationNote> notes, std::uint64_t selected);
    void select(std::uint64_t number, bool focusEditor);
    void updateNote(std::uint64_t number, const std::wstring& text);
    void focusEditor();
    void finishEditing();
    void detachOwner();
    bool isEditing() const;
    bool contains(POINT point) const;
    void pointerDown(POINT point, bool right);
    void pointerMove(POINT point);
    void pointerUp(POINT point, bool right);
    void wheel(POINT point, float distance);
    bool cursor();

private:
    struct Row {
        HuhuCapture::Core::AnnotationNote note;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> number;
        float top{}, height{};
    };

    void layout() override;
    void rebuildLayouts();
    void prepareHitLayout();
    void paint();
    void updateEditor();
    void synchronizeEditorGeometry();
    void cancelPointerGesture();
    void keepSelectedVisible();
    void setScroll(float value);
    float maximumScroll() const;
    D2D1_RECT_F listRect() const;
    D2D1_RECT_F scrollThumb() const;
    std::uint64_t rowAt(POINT point) const;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> makeText(const std::wstring& text,
        float fontSize, float width, float height, bool center = false) const;

    WinPin* owner;
    Ling::Canvas* canvas{};
    Ling::TextBox* editor{};
    winrt::event_token textToken{}, focusToken{}, blurToken{};
    std::vector<Row> rows;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> titleText, collapseText, emptyText,
        editTitle, deleteText, footerText;
    std::uint64_t selected{}, pressedRow{}, hoveredRow{};
    float requestedLeft{-1.f}, requestedWidth{-1.f}, requestedHeight{-1.f};
    float cachedDpi{-1.f};
    float scrollY{}, totalRowsHeight{}, badgeWidth{32.f}, cachedTextWidth{-1.f};
    float editorTop{}, editorHeight{}, editorHeadingTop{}, footerTop{}, listTop{}, listBottom{};
    float scrollbarDragY{}, scrollbarDragOffset{};
    bool panelVisible{}, changingText{}, layoutsDirty{true}, chromeDirty{true}, revealSelected{}, draggingScrollbar{};
};
