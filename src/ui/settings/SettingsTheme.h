#pragma once

#include <include/Ling.h>
#include <cstdint>
#include <string>

// Desktop adaptation of VoltAgent/awesome-design-md/design-md/apple. Keep these
// neutral settings surfaces separate from the illustrated product logo palette.
namespace HuhuCapture::UI::SettingsTheme {

inline constexpr std::uint32_t canvas = 0xF5F5F7FF;
inline constexpr std::uint32_t surface = 0xFFFFFFFF;
inline constexpr std::uint32_t ink = 0x1D1D1FFF;
inline constexpr std::uint32_t secondary = 0x636366FF;
inline constexpr std::uint32_t hairline = 0xE0E0E0FF;
inline constexpr std::uint32_t blue = 0x0066CCFF;
inline constexpr std::uint32_t focusBlue = 0x0071E3FF;
inline constexpr std::uint32_t blueTint = 0xEAF3FFFF;
inline constexpr std::uint32_t warning = 0xB42318FF;
inline constexpr float controlHeight = 44.f;

inline Ling::Label* label(Ling::Node* parent, const std::wstring& text,
    float size = 17.f, float height = 26.f, std::uint32_t color = ink)
{
    auto* result = parent->makeChild<Ling::Label>();
    result->setFontFamily(L"Segoe UI");
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
    auto* heading = label(parent, title, 28.f, 42.f);
    heading->setFontFamily(L"Segoe UI Semibold");
    auto* subtitle = label(parent, description, 14.f, 24.f, secondary);
    subtitle->setMarginBottom(20.f);
}

inline Ling::Node* card(Ling::Node* parent)
{
    auto* result = parent->makeChild<Ling::Node>();
    result->setBg(surface);
    result->setBorder(1.f, hairline);
    result->setBorderRadius(18.f);
    result->setPadding(24.f, 8.f, 24.f, 8.f);
    result->setFlexDirection(Ling::FlexDirection::Column);
    result->setFlexShrink(0.f);
    return result;
}

inline Ling::Node* row(Ling::Node* parent, float height = 64.f)
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

inline void styleButton(Ling::Button* button, float width = 160.f, bool primary = false)
{
    button->setSize(width, controlHeight);
    button->setFlexShrink(0.f);
    button->setFontFamily(L"Segoe UI");
    button->setFontSize(16.f);
    button->setBorderRadius(primary ? 22.f : 11.f);
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
    page->setHeightPercent(100.f);
    page->setPadding(32.f, 56.f, 32.f, 24.f);
    page->setFlexDirection(Ling::FlexDirection::Column);
}

} // namespace HuhuCapture::UI::SettingsTheme
