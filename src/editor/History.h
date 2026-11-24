#pragma once

#include <string_view>
#include "core/HistoryStack.h"

class ShapeBase;
class WinPin;

// Adapts the UI-independent history to the editor's borrowed shape pointers.
class History
{
public:
    explicit History(WinPin* win);
    ~History();
    ShapeBase* createShape(std::wstring_view toolId, int x, int y);
    void undo();
    void redo();
    void removeHoverShape();
    void removeShape(ShapeBase* target);
    void removeShapeLater(ShapeBase* target);

    // Only active shapes are rendered, hit-tested, numbered and composited.
    auto shapes() const noexcept { return stack.activeItems(); }

private:
    void releaseBorrows(ShapeBase* target);

    WinPin* win;
    HuhuCapture::Core::HistoryStack<ShapeBase> stack;
};
