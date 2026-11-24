#include "pch.h"
#include "ui/settings/SettingsSelectPopup.h"
#include "ui/settings/SettingsTheme.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace Theme = HuhuCapture::UI::SettingsTheme;

namespace {
constexpr float padding = 6.f;
constexpr float separatorHeight = 8.f;
constexpr float gap = 4.f;
constexpr std::size_t noItem = static_cast<std::size_t>(-1);

bool contains(const D2D1_RECT_F& rectangle, POINT point)
{
    return point.x >= rectangle.left && point.x < rectangle.right &&
        point.y >= rectangle.top && point.y < rectangle.bottom;
}
}

struct SettingsSelectPopup::State {
    HWND owner{};
    std::vector<Item> items;
    std::vector<float> tops;
    Completion completion;
    std::shared_ptr<int> lifetime{std::make_shared<int>(0)};
    std::wstring selected;
    std::size_t highlighted{}, hovered{noItem}, pressed{noItem};
    float contentHeight{}, scrollY{}, dragY{}, dragScroll{}, cachedWidth{-1.f}, cachedDpi{-1.f};
    bool closing{}, dragging{}, keyboard{true};
    Ling::Canvas* canvas{};
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> textFormat;
    std::map<std::size_t, ComPtr<IDWriteTextLayout>> labels;

    float viewport(const SettingsSelectPopup& popup) const
    {
        return (std::max)(0.f, popup.h / popup.dpi - padding * 2.f);
    }
    float maxScroll(const SettingsSelectPopup& popup) const
    {
        return (std::max)(0.f, contentHeight - viewport(popup));
    }
    D2D1_RECT_F row(const SettingsSelectPopup& popup, std::size_t index) const
    {
        const auto d = popup.dpi;
        const auto top = (padding + tops[index] - scrollY) * d;
        const auto right = popup.w - (padding + (maxScroll(popup) > 0.f ? 8.f : 0.f)) * d;
        return D2D1::RectF(padding * d, top, right, top + Theme::controlHeight * d);
    }
    std::size_t hit(const SettingsSelectPopup& popup, POINT point) const
    {
        if (point.y < padding * popup.dpi || point.y >= popup.h - padding * popup.dpi) return noItem;
        for (std::size_t index = 0; index < items.size(); ++index)
            if (contains(row(popup, index), point)) return index;
        return noItem;
    }
    D2D1_RECT_F scrollbar(const SettingsSelectPopup& popup) const
    {
        const float maximum = maxScroll(popup);
        if (maximum <= 0.f) return {};
        const float view = viewport(popup);
        const float thumb = (std::min)(view, (std::max)(24.f, view * view / contentHeight));
        const float top = padding + (view - thumb) * scrollY / maximum;
        return D2D1::RectF(popup.w - 7.f * popup.dpi, top * popup.dpi,
            popup.w - 4.f * popup.dpi, (top + thumb) * popup.dpi);
    }
};

SettingsSelectPopup::SettingsSelectPopup(HWND owner, float scale, RECT screenAnchor,
    std::vector<Item> items, const std::wstring& selectedValue, Completion completion)
    : state(std::make_unique<State>())
{
    dpi = std::isfinite(scale) && scale > 0.f ? scale : 1.f;
    state->owner = owner;
    state->items = std::move(items);
    state->selected = selectedValue;
    state->completion = std::move(completion);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
        if (state->items[index].separatorBefore) state->contentHeight += separatorHeight;
        state->tops.push_back(state->contentHeight);
        state->contentHeight += Theme::controlHeight;
        if (state->items[index].value == selectedValue) state->highlighted = index;
    }

    MONITORINFO monitor{sizeof(MONITORINFO)};
    RECT work{};
    if (GetMonitorInfoW(MonitorFromRect(&screenAnchor, MONITOR_DEFAULTTONEAREST), &monitor)) work = monitor.rcWork;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    const float workWidth = (std::max)(1.f, static_cast<float>(work.right - work.left) - 16.f * dpi);
    const float desiredWidth = (std::max)(180.f * dpi, static_cast<float>(screenAnchor.right - screenAnchor.left));
    const float width = (std::min)(workWidth, (std::min)(360.f * dpi, desiredWidth));
    // Cap ordinary long lists at whole rows, so End does not leave a thin strip
    // of the previous row's glyphs visible at the top of an eight-row viewport.
    const float desiredHeight = (std::min)(state->contentHeight + padding * 2.f,
        Theme::controlHeight * 8.f + padding * 2.f) * dpi;
    const float below = (std::max)(0.f, work.bottom - 8.f * dpi - screenAnchor.bottom - gap * dpi);
    const float above = (std::max)(0.f, screenAnchor.top - gap * dpi - work.top - 8.f * dpi);
    const bool placeBelow = below >= desiredHeight || below >= above;
    const float height = (std::max)(1.f, (std::min)(desiredHeight, placeBelow ? below : above));
    setSize(std::ceil(width) / dpi, std::ceil(height) / dpi);
    setMinSize(1.f, 1.f);
    const auto left = std::clamp(screenAnchor.right - static_cast<LONG>(w),
        work.left + static_cast<LONG>(8.f * dpi),
        (std::max)(work.left + static_cast<LONG>(8.f * dpi), work.right - static_cast<LONG>(w + 8.f * dpi)));
    const auto top = placeBelow ? screenAnchor.bottom + static_cast<LONG>(gap * dpi)
        : screenAnchor.top - static_cast<LONG>(h + gap * dpi);
    setPosition(left, std::clamp(top, work.top, (std::max)(work.top, work.bottom - static_cast<LONG>(h))));
    createNativeWindow(WS_EX_TOOLWINDOW, WS_POPUP);
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    enableShadow();
    // A rounded window region also supplies the same outline on Windows 10.
    const auto radius = static_cast<int>(std::ceil(Theme::controlRadius * dpi * 2.f));
    HRGN region = CreateRoundRectRgn(0, 0, static_cast<int>(w) + 1, static_cast<int>(h) + 1, radius, radius);
    if (region && !SetWindowRgn(hwnd, region, FALSE)) DeleteObject(region);
    ensureVisible();
    show();
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    refresh();
}

SettingsSelectPopup::~SettingsSelectPopup()
{
    state->closing = true;
    state->lifetime.reset();
    state->completion = {};
    if (GetCapture() == hwnd) ReleaseCapture();
    if (hwnd && IsWindow(hwnd)) close();
    hwnd = nullptr;
}

void SettingsSelectPopup::onCreated()
{
    body->setBg(0);
    state->canvas = body->makeChild<Ling::Canvas>();
    state->canvas->setSizePercent(100.f, 100.f);
    state->canvas->enableSwapChain();
    onMouseMove.add([this](POINT point) { pointerMove(point); });
    onMouseDown.add([this](POINT point, bool right) { pointerDown(point, right); });
    onMouseUp.add([this](POINT point, bool right) { pointerUp(point, right); });
    onMouseWheel.add([this](POINT, float distance) { scroll(-distance / dpi); });
    onKeyDown.add([this](UINT key) { keyDown(key); });
    onBlur.add([this] { finish(std::nullopt, false); });
    onDestroy.add([this] { finish(std::nullopt, false); });
    onDpiChanged.add([this] { dismiss(); });
}

void SettingsSelectPopup::onMinMaxInfo(MINMAXINFO* info)
{
    info->ptMinTrackSize = {1, 1};
}

BOOL SettingsSelectPopup::setCursor()
{
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    return TRUE;
}

bool SettingsSelectPopup::isOpen() const
{
    return !state->closing && hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd);
}

std::wstring SettingsSelectPopup::highlightedValue() const
{
    return state->highlighted < state->items.size() ? state->items[state->highlighted].value : L"";
}

RECT SettingsSelectPopup::itemScreenRect(const std::wstring& value) const
{
    if (!hwnd || !IsWindow(hwnd)) return {};
    for (std::size_t index = 0; index < state->items.size(); ++index) {
        if (state->items[index].value != value) continue;
        const auto box = state->row(*this, index);
        if (box.top < padding * dpi || box.bottom > h - padding * dpi) return {};
        POINT origin{};
        ClientToScreen(hwnd, &origin);
        return {origin.x + static_cast<LONG>(box.left), origin.y + static_cast<LONG>(box.top),
            origin.x + static_cast<LONG>(box.right), origin.y + static_cast<LONG>(box.bottom)};
    }
    return {};
}

void SettingsSelectPopup::dismiss()
{
    finish(std::nullopt, false);
}

void SettingsSelectPopup::finish(std::optional<std::wstring> value, bool restoreOwnerFocus)
{
    if (state->closing) return;
    state->closing = true;
    state->dragging = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (hwnd && IsWindow(hwnd)) hide();
    if (restoreOwnerFocus && IsWindow(state->owner)) {
        SetForegroundWindow(state->owner);
        SetFocus(state->owner);
    }
    auto callback = std::move(state->completion);
    const std::weak_ptr<int> lifetime = state->lifetime;
    if (callback && Ling::App::get()) {
        Ling::App::get()->dq.TryEnqueue([lifetime, callback = std::move(callback), value = std::move(value)]() mutable {
            if (!lifetime.expired()) callback(std::move(value));
        });
    }
}

void SettingsSelectPopup::ensureVisible()
{
    if (state->highlighted >= state->items.size()) return;
    const float top = state->tops[state->highlighted];
    const float bottom = top + Theme::controlHeight;
    if (top < state->scrollY) state->scrollY = top;
    else if (bottom > state->scrollY + state->viewport(*this)) state->scrollY = bottom - state->viewport(*this);
    state->scrollY = std::clamp(state->scrollY, 0.f, state->maxScroll(*this));
}

void SettingsSelectPopup::keyDown(UINT key)
{
    if (state->closing) return;
    if (key == VK_ESCAPE || key == VK_TAB || (key == VK_F4 && GetKeyState(VK_MENU) < 0)) {
        finish(std::nullopt, true);
        if (key == VK_TAB && IsWindow(state->owner)) PostMessageW(state->owner, WM_KEYDOWN, VK_TAB, 0);
        return;
    }
    if (state->items.empty()) return;
    if (key == VK_RETURN || key == VK_SPACE) {
        finish(state->items[state->highlighted].value, true);
        return;
    }
    const auto last = state->items.size() - 1;
    const auto page = static_cast<std::size_t>((std::max)(1.f, std::floor(state->viewport(*this) / Theme::controlHeight)));
    switch (key) {
    case VK_UP: if (state->highlighted > 0) --state->highlighted; break;
    case VK_DOWN: state->highlighted = (std::min)(last, state->highlighted + 1); break;
    case VK_HOME: state->highlighted = 0; break;
    case VK_END: state->highlighted = last; break;
    case VK_PRIOR: state->highlighted -= (std::min)(state->highlighted, page); break;
    case VK_NEXT: state->highlighted = (std::min)(last, state->highlighted + page); break;
    default: return;
    }
    state->keyboard = true;
    state->hovered = noItem;
    ensureVisible();
    refresh();
}

void SettingsSelectPopup::pointerMove(POINT point)
{
    if (state->closing) return;
    if (point.x == INT_MAX || point.y == INT_MAX) {
        state->hovered = noItem;
        refresh();
        return;
    }
    if (state->dragging) {
        const auto thumb = state->scrollbar(*this);
        const float travel = state->viewport(*this) - (thumb.bottom - thumb.top) / dpi;
        if (travel > 0.f) state->scrollY = std::clamp(state->dragScroll +
            (point.y / dpi - state->dragY) * state->maxScroll(*this) / travel, 0.f, state->maxScroll(*this));
        refresh();
        return;
    }
    const auto hovered = state->hit(*this, point);
    if (hovered == state->hovered) return;
    state->hovered = hovered;
    state->keyboard = false;
    if (hovered != noItem) state->highlighted = hovered;
    refresh();
}

void SettingsSelectPopup::pointerDown(POINT point, bool right)
{
    if (state->closing || right) return;
    const auto thumb = state->scrollbar(*this);
    if (state->maxScroll(*this) > 0.f && point.x >= w - 11.f * dpi && point.x < w &&
        point.y >= padding * dpi && point.y < h - padding * dpi) {
        if (!contains(thumb, point)) scroll(point.y < thumb.top ? -state->viewport(*this) : state->viewport(*this));
        state->dragging = true;
        state->dragY = point.y / dpi;
        state->dragScroll = state->scrollY;
        SetCapture(hwnd);
        return;
    }
    state->pressed = state->hit(*this, point);
    if (state->pressed != noItem) SetCapture(hwnd);
    refresh();
}

void SettingsSelectPopup::pointerUp(POINT point, bool right)
{
    if (state->closing || right) return;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (state->dragging) { state->dragging = false; return; }
    const auto pressed = std::exchange(state->pressed, noItem);
    const auto hit = state->hit(*this, point);
    if (pressed != noItem && pressed == hit) finish(state->items[hit].value, true);
    else refresh();
}

void SettingsSelectPopup::scroll(float distance)
{
    if (state->closing) return;
    state->scrollY = std::clamp(state->scrollY + distance, 0.f, state->maxScroll(*this));
    state->hovered = noItem;
    state->keyboard = false;
    refresh();
}

void SettingsSelectPopup::layout()
{
    Ling::WinBase::layout();
    if (!state->canvas || state->closing) return;
    if (state->cachedWidth != w || state->cachedDpi != dpi) {
        if (state->cachedDpi != dpi) state->textFormat.Reset();
        state->cachedWidth = w;
        state->cachedDpi = dpi;
        state->labels.clear();
    }
    paint();
}

void SettingsSelectPopup::paint()
{
    auto* context = state->canvas->startPaint();
    if (!context) return;
    if (!state->brush && FAILED(context->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f), state->brush.GetAddressOf()))) {
        state->canvas->finishPaint();
        return;
    }
    auto* factory = Ling::D2D::get()->dwriteFactory.Get();
    if (!state->textFormat) {
        // The trimming sign keeps the format used to create it. Build both it
        // and the text layout from the final font so ellipses scale with labels.
        if (FAILED(factory->CreateTextFormat(Theme::fontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, Theme::bodySize * dpi,
            L"zh-CN", state->textFormat.GetAddressOf()))) {
            state->canvas->finishPaint();
            return;
        }
        state->textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        state->textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    auto* brush = state->brush.Get();
    const auto color = [brush](std::uint32_t rgba) { brush->SetColor(D2D1::ColorF(rgba >> 8, (rgba & 0xff) / 255.f)); };
    const float d = dpi;
    const auto outline = D2D1::RoundedRect(D2D1::RectF(.5f * d, .5f * d, w - .5f * d, h - .5f * d),
        Theme::controlRadius * d, Theme::controlRadius * d);
    context->Clear(D2D1::ColorF(Theme::surface >> 8));
    color(Theme::hairline);
    context->DrawRoundedRectangle(outline, brush, d);
    context->PushAxisAlignedClip(D2D1::RectF(padding * d, padding * d, w - padding * d, h - padding * d),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
        const auto& item = state->items[index];
        const auto box = state->row(*this, index);
        if (box.bottom <= padding * d || box.top >= h - padding * d) continue;
        if (item.separatorBefore) {
            color(Theme::hairline);
            const float separatorY = box.top - separatorHeight * .5f * d;
            context->DrawLine({12.f * d, separatorY}, {w - 12.f * d, separatorY}, brush, d);
        }
        const bool active = index == state->hovered || (state->keyboard && index == state->highlighted);
        if (active || index == state->pressed) {
            color(index == state->pressed ? Theme::hairline : Theme::blueTint);
            context->FillRoundedRectangle(D2D1::RoundedRect(box, 5.f * d, 5.f * d), brush);
        }
        if (item.value == state->selected) {
            color(Theme::blue);
            const float cx = box.left + 13.f * d, cy = (box.top + box.bottom) * .5f;
            context->DrawLine({cx - 4.f * d, cy}, {cx - 1.f * d, cy + 3.f * d}, brush, 1.6f * d);
            context->DrawLine({cx - 1.f * d, cy + 3.f * d}, {cx + 5.f * d, cy - 4.f * d}, brush, 1.6f * d);
        }
        if (state->labels.size() > 32) state->labels.clear();
        auto [found, inserted] = state->labels.try_emplace(index);
        if (inserted) {
            auto* format = state->textFormat.Get();
            const auto textWidth = (std::max)(1.f, box.right - box.left - 36.f * d);
            factory->CreateTextLayout(item.label.c_str(), static_cast<UINT32>(item.label.size()), format,
                textWidth, Theme::controlHeight * d, found->second.GetAddressOf());
            if (found->second) {
                ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf()))) {
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    found->second->SetTrimming(&trimming, ellipsis.Get());
                }
            }
        }
        color(Theme::ink);
        if (found->second) context->DrawTextLayout({box.left + 26.f * d, box.top},
            found->second.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    context->PopAxisAlignedClip();
    if (state->maxScroll(*this) > 0.f) {
        color(Theme::secondary);
        context->FillRoundedRectangle(D2D1::RoundedRect(state->scrollbar(*this), 1.5f * d, 1.5f * d), brush);
    }
    state->canvas->finishPaint();
}
