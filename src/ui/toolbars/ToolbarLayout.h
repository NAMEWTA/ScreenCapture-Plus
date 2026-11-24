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
    const auto width = toolbarPixelAligned(extent.width, window->dpi);
    const auto height = toolbarPixelAligned(extent.height, window->dpi);
    if (std::abs(window->w - width * window->dpi) < .01f &&
        std::abs(window->h - height * window->dpi) < .01f) return;
    window->setSize(width, height);
}

inline float toolbarWorkWidth(const RECT& workArea, float dpi)
{
    return static_cast<float>(std::max(1L, workArea.right - workArea.left)) / dpi;
}

} // namespace ScreenCapture::UI
