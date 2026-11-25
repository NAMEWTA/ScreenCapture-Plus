#pragma once

#include "ui/SettingsMetrics.h"
#include <include/Ling.h>
#include <cstdint>
#include <string>

// Desktop adaptation of VoltAgent/awesome-design-md/design-md/apple. Keep these
// neutral settings surfaces separate from the illustrated product logo palette.
namespace HuhuCapture::UI::SettingsTheme {

using namespace HuhuCapture::UI::SettingsMetrics;
inline constexpr wchar_t fontFamily[] = L"Segoe UI";
inline constexpr wchar_t semiboldFamily[] = L"Segoe UI Semibold";

inline Ling::Label* label(Ling::Node* parent, const std::wstring& text,
    float size = bodySize, float height = bodyLineHeight, std::uint32_t color = ink)
{
    auto* result = parent->makeChild<Ling::Label>();
    result->setFontFamily(fontFamily);
    result->setFontSize(size);
    result->setColor(color);
    result->setText(text);
    result->setHeight(height);
    result->setFlexShrink(0.f);
    result->setJustifyContent(Ling::Justify::Center);
    return result;
}

inline void pageHeading(Ling::Node* parent, const std::wstring& title,
    const std::wstring& description)
{
    auto* heading = label(parent, title, headingSize, headingLineHeight);
    heading->setFontFamily(semiboldFamily);
    auto* subtitle = label(parent, description, captionSize, captionLineHeight, secondary);
    subtitle->setMarginTop(2.f);
    subtitle->setMarginBottom(groupGap);
}

inline Ling::Node* card(Ling::Node* parent)
{
    auto* result = parent->makeChild<Ling::Node>();
    result->setBg(surface);
    result->setBorder(1.f, hairline);
    result->setBorderRadius(cardRadius);
    result->setPadding(cardInset, 6.f, cardInset, 6.f);
    result->setFlexDirection(Ling::FlexDirection::Column);
    result->setFlexShrink(0.f);
    return result;
}

inline Ling::Node* row(Ling::Node* parent, float height = rowHeight)
{
    auto* result = parent->makeChild<Ling::Node>();
    result->setFlexDirection(Ling::FlexDirection::Row);
    result->setAlignItems(Ling::Align::Center);
    result->setHeight(height);
    result->setFlexShrink(0.f);
    return result;
}

inline void divider(Ling::Node* parent)
{
    auto* result = parent->makeChild<Ling::Node>();
    result->setHeight(1.f);
    result->setFlexShrink(0.f);
    result->setBg(hairline);
}

inline void styleButton(Ling::Button* button, float width = 120.f, bool primary = false)
{
    button->setSize(width, controlHeight);
    button->setFlexShrink(0.f);
    button->setFontFamily(fontFamily);
    button->setFontSize(bodySize);
    button->setJustifyContent(Ling::Justify::Center);
    button->setAlignItems(Ling::Align::Center);
    button->setBorderRadius(controlRadius);
    button->setBorder(1.f, primary ? blue : hairline);
    button->setColor(primary ? surface : ink);
    button->setHoverColor(primary ? surface : blue);
    button->setBg(primary ? blue : surface);
    button->setHoverBg(primary ? focusBlue : canvas);
    button->setHoverBorderColor(focusBlue);
}

inline void stylePage(Ling::Node* page)
{
    page->setFlexGrow(1.f);
    page->setFlexShrink(1.f);
    // The window's content host supplies the shared, centered column and gutters.
    page->setPadding(0.f);
    page->setFlexDirection(Ling::FlexDirection::Column);
}

} // namespace HuhuCapture::UI::SettingsTheme
