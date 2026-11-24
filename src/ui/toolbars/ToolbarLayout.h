#pragma once

#include <include/Ling.h>
#include "ui/toolbars/ToolbarMetrics.h"

namespace ScreenCapture::UI {

inline void configureToolbarRow(Ling::Node* node, bool padded = true)
{
    node->setAlignItems(Ling::Align::Center);
    node->setFlexDirection(Ling::FlexDirection::Row);
    node->setFlexWrap(Ling::Wrap::Wrap);
    YGNodeStyleSetAlignContent(node->node, YGAlignFlexStart);
    if (padded) node->setPadding(toolbarBorder);
}

inline void sizeToolbarControl(Ling::Node* node, float width = toolbarButtonSize)
{
    node->setSize(width, toolbarButtonSize);
    node->setFlexGrow(0.f);
    node->setFlexShrink(0.f);
}

inline Ling::Node* makeToolbarDivider(Ling::Node* parent)
{
    auto cell = parent->makeChild<Ling::Node>();
    sizeToolbarControl(cell, toolbarDivider);
    cell->setJustifyContent(Ling::Justify::Center);
    auto line = cell->makeChild<Ling::Node>();
    line->setSize(toolbarDivider, toolbarDividerHeight);
    line->setBg(0xDDDDDDff);
    return cell;
}

inline void applyToolbarExtent(Ling::WinBase* window, const ToolbarExtent& extent)
{
    const auto width = std::ceil(extent.width * window->dpi);
    const auto height = std::ceil(extent.height * window->dpi);
    if (std::abs(window->w - width) < .01f &&
        std::abs(window->h - height) < .01f) return;
    window->w = width;
    window->h = height;
    // Reflow is layout work, not a focus change. Ling's generic setSize omits
    // NOACTIVATE and can blur the screenshot's shared text editor here.
    if (window->hwnd) {
        SetWindowPos(window->hwnd, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
            SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW);
    }
}

inline void positionToolbar(Ling::WinBase* window, int x, int y)
{
    if (window->x == x && window->y == y) return;
    window->x = x;
    window->y = y;
    if (window->hwnd) {
        SetWindowPos(window->hwnd, nullptr, x, y, 0, 0,
            SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW);
    }
}

inline float toolbarWorkWidth(const RECT& workArea, float dpi)
{
    return static_cast<float>(std::max(1L, workArea.right - workArea.left)) / dpi;
}

} // namespace ScreenCapture::UI
