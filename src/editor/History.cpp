#include "pch.h"
#include "editor/History.h"
#include "editor/ShapeFactory.h"
#include "editor/WinPin.h"
#include "editor/shapes/ShapeBase.h"
#include "editor/shapes/ShapeText.h"

History::History(WinPin* win) : win(win) {}
History::~History() = default;

ShapeBase* History::createShape(std::wstring_view toolId, int x, int y)
{
    auto shape = ShapeFactory::create(toolId, win);
    auto* result = stack.append(std::move(shape),
        [this](ShapeBase* discarded) { releaseBorrows(discarded); });
    if (result) result->mouseDown(static_cast<float>(x), static_cast<float>(y));
    return result;
}

void History::undo()
{
    if (auto* shape = stack.undo()) {
        releaseBorrows(shape);
        win->refresh();
    }
}

void History::redo()
{
    if (stack.redo()) win->refresh();
}

void History::removeHoverShape()
{
    removeShape(win->shapeHover);
}

void History::removeShape(ShapeBase* target)
{
    if (stack.erase(target, [this](ShapeBase* removed) { releaseBorrows(removed); })) {
        win->refresh();
    }
}

void History::removeShapeLater(ShapeBase* target)
{
    // Both this history and the exact item must still exist when the UI queue
    // runs. A new shape at the same address must not be removed by an old task.
    const auto ticket = stack.ticket(target);
    Ling::App::get()->dq.TryEnqueue([this, ticket]() {
        if (auto* shape = ticket.get()) removeShape(shape);
    });
}

void History::releaseBorrows(ShapeBase* target)
{
    if (win->shapeHover == target) win->shapeHover = nullptr;
    if (win->newShape == target) win->newShape = nullptr;
    if (auto* text = dynamic_cast<ShapeText*>(target)) text->finishEdit();
}
