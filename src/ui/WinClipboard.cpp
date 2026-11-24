#include "pch.h"
#include "ui/WinClipboard.h"
#include "services/Lang.h"
#include "services/MediaHistory.h"
#include "capture/WinCap.h"
#include "editor/WinPin.h"

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <shellapi.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wincodec.h>

using Microsoft::WRL::ComPtr;
namespace MediaHistory = HuhuCapture::MediaHistory;

namespace {
    std::unique_ptr<WinClipboard> clipboardWindow;
    constexpr std::size_t pageSize = 20;
    constexpr UINT workerTimer = 0x4843;
    constexpr float rowHeight = 112.f;
    constexpr float rowGap = 12.f;
    constexpr std::size_t noEntry = static_cast<std::size_t>(-1);

    enum Action { None, Close, OpenFolder, Refresh, Clear, Select, Preview, Copy, Reveal, Previous, Next };

    struct NativeBitmapDeleter {
        void operator()(std::remove_pointer_t<HBITMAP>* bitmap) const {
            if (bitmap) DeleteObject(bitmap);
        }
    };
    using NativeBitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, NativeBitmapDeleter>;

    struct Target {
        int action{None};
        std::size_t index{noEntry};
        D2D1_RECT_F rect{};
        bool enabled{true};
        bool operator==(const Target& other) const { return action == other.action && index == other.index; }
    };

    bool contains(D2D1_RECT_F rect, POINT point) {
        return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
    }

    bool rowAction(int action) { return action >= Select && action <= Reveal; }

    std::wstring sizeText(std::uintmax_t bytes) {
        if (bytes < 1024) return std::format(L"{} B", bytes);
        if (bytes < 1024 * 1024) return std::format(L"{:.1f} KB", static_cast<double>(bytes) / 1024.0);
        if (bytes < 1024ULL * 1024 * 1024) return std::format(L"{:.1f} MB", static_cast<double>(bytes) / (1024 * 1024));
        return std::format(L"{:.1f} GB", static_cast<double>(bytes) / (1024ULL * 1024 * 1024));
    }

    std::wstring captureTime(const std::filesystem::path& path) {
        const auto stem = path.stem().wstring();
        if (stem.size() < 15 || stem[8] != L'-') return {};
        for (std::size_t i = 0; i < 15; ++i)
            if (i != 8 && (stem[i] < L'0' || stem[i] > L'9')) return {};
        return stem.substr(0, 4) + L"-" + stem.substr(4, 2) + L"-" + stem.substr(6, 2) + L"  " +
            stem.substr(9, 2) + L":" + stem.substr(11, 2) + L":" + stem.substr(13, 2);
    }
}

struct WinClipboard::State {
    struct Job { std::uint64_t generation; std::size_t index; std::filesystem::path path; };
    struct Thumbnail { std::uint64_t generation; std::size_t index; NativeBitmap bitmap; };
    struct Listing { std::uint64_t generation; std::vector<MediaHistory::Entry> entries; bool failed{}; };
    struct Work {
        std::mutex mutex;
        std::condition_variable changed;
        bool stop{}, reload{};
        std::uint64_t generation{};
        std::deque<Job> jobs;
        std::vector<Thumbnail> completed;
        std::optional<Listing> listing;
    };
    struct Row {
        std::size_t index{};
        std::wstring metadata;
        NativeBitmap nativeBitmap;
        ComPtr<ID2D1Bitmap> bitmap;
        bool loaded{};
    };

    std::shared_ptr<Work> work{std::make_shared<Work>()};
    std::shared_ptr<int> lifetime{std::make_shared<int>(0)};
    std::thread worker;
    Ling::Canvas* canvas{};
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1SolidColorBrush> brush;
    using TextKey = std::tuple<std::wstring, float, float, float, bool, bool>;
    std::map<TextKey, ComPtr<IDWriteTextLayout>> textCache;
    std::vector<MediaHistory::Entry> entries;
    std::vector<Row> rows;
    std::vector<Target> targets;
    std::filesystem::path folder;
    std::wstring status;
    std::uint64_t generation{};
    std::size_t page{}, selected{noEntry};
    float scrollY{}, cachedW{-1.f}, cachedH{-1.f}, cachedDpi{-1.f};
    float dragY{}, dragScroll{};
    bool loading{}, failed{}, statusError{}, closed{}, reopen{}, dragging{};
    Target focused{}, hovered{}, pressed{};

    D2D1_RECT_F listRect(const WinClipboard& window) const {
        const auto d = window.dpi;
        return D2D1::RectF(24.f * d, 184.f * d, window.w - 24.f * d, window.h - 100.f * d);
    }
    float maxScroll(const WinClipboard& window) const {
        const auto list = listRect(window);
        const float total = rows.empty() ? 0.f : (rows.size() * (rowHeight + rowGap) - rowGap) * window.dpi;
        return (std::max)(0.f, total - (list.bottom - list.top));
    }
    D2D1_RECT_F scrollbar(const WinClipboard& window) const {
        const auto list = listRect(window);
        const auto maximum = maxScroll(window);
        if (maximum <= 0.f) return {};
        const float viewport = list.bottom - list.top;
        const float height = (std::max)(32.f * window.dpi, viewport * viewport / (viewport + maximum));
        const float top = list.top + (viewport - height) * scrollY / maximum;
        return D2D1::RectF(window.w - 13.f * window.dpi, top, window.w - 7.f * window.dpi, top + height);
    }
    void buildTargets(const WinClipboard& window) {
        const float d = window.dpi;
        const auto list = listRect(window);
        targets.clear();
        const auto add = [&](int action, D2D1_RECT_F rect, bool enabled = true, std::size_t index = noEntry) {
            targets.push_back({action, index, rect, enabled});
        };
        add(Close, D2D1::RectF(window.w - 60.f * d, 16.f * d, window.w - 16.f * d, 60.f * d));
        add(OpenFolder, D2D1::RectF(24.f * d, 108.f * d, 164.f * d, 152.f * d), !folder.empty());
        add(Refresh, D2D1::RectF(176.f * d, 108.f * d, 276.f * d, 152.f * d), !loading);
        add(Clear, D2D1::RectF(window.w - 164.f * d, 108.f * d, window.w - 24.f * d, 152.f * d), !loading && !entries.empty());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const float top = list.top + i * (rowHeight + rowGap) * d - scrollY;
            const auto right = list.right;
            add(Select, D2D1::RectF(list.left, top, right, top + rowHeight * d), true, rows[i].index);
            add(Preview, D2D1::RectF(list.left + 16.f * d, top + 16.f * d, list.left + 144.f * d, top + 96.f * d), true, rows[i].index);
            add(Copy, D2D1::RectF(right - 224.f * d, top + 34.f * d, right - 136.f * d, top + 78.f * d), true, rows[i].index);
            add(Reveal, D2D1::RectF(right - 124.f * d, top + 34.f * d, right - 12.f * d, top + 78.f * d), true, rows[i].index);
        }
        add(Previous, D2D1::RectF(window.w - 248.f * d, window.h - 60.f * d, window.w - 140.f * d, window.h - 16.f * d), !loading && page > 0);
        add(Next, D2D1::RectF(window.w - 128.f * d, window.h - 60.f * d, window.w - 24.f * d, window.h - 16.f * d), !loading && (page + 1) * pageSize < entries.size());
    }
    Target hit(const WinClipboard& window, POINT point) const {
        for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
            if (!it->enabled || (rowAction(it->action) && !contains(listRect(window), point))) continue;
            if (contains(it->rect, point)) return *it;
        }
        return {};
    }
};

WinClipboard::WinClipboard() : state(std::make_unique<State>()) {
    setTitle(Lang::get(L"clipboard.title"));
    RECT workArea{};
    const bool hasWorkArea = SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) != FALSE;
    const float availableWidth = hasWorkArea ? static_cast<float>(workArea.right - workArea.left) / dpi - 32.f : 920.f;
    const float availableHeight = hasWorkArea ? static_cast<float>(workArea.bottom - workArea.top) / dpi - 32.f : 700.f;
    const float minimumWidth = (std::min)(760.f, (std::max)(640.f, availableWidth));
    const float minimumHeight = (std::min)(560.f, (std::max)(440.f, availableHeight));
    setSize((std::max)(minimumWidth, (std::min)(920.f, availableWidth)), (std::max)(minimumHeight, (std::min)(700.f, availableHeight)));
    setMinSize(minimumWidth, minimumHeight);
    setCenter();
    onDestroy.add([this] {
        state->closed = true;
        stopWorker();
        // The worker queues destruction after its current native decode returns.
        // Closing the native window never waits for a slow thumbnail provider.
    });
    createNativeWindow();
}

WinClipboard::~WinClipboard() {
    stopWorker();
    if (state->worker.joinable()) state->worker.join();
    state->lifetime.reset();
}

void WinClipboard::init() {
    if (clipboardWindow) {
        if (clipboardWindow->state->closed) clipboardWindow->state->reopen = true;
        else {
            clipboardWindow->reload();
            clipboardWindow->show();
            SetForegroundWindow(clipboardWindow->hwnd);
        }
        return;
    }
    clipboardWindow.reset(new WinClipboard());
}

void WinClipboard::dispose() { clipboardWindow.reset(); }
WinClipboard* WinClipboard::get() { return clipboardWindow.get(); }
bool WinClipboard::isLoading() const { return state->loading; }
std::size_t WinClipboard::entryCount() const { return state->entries.size(); }
std::size_t WinClipboard::thumbnailCount() const {
    return static_cast<std::size_t>(std::count_if(state->rows.begin(), state->rows.end(), [](const State::Row& row) {
        return row.nativeBitmap || row.bitmap;
    }));
}

void WinClipboard::onCreated() {
    enableShadow();
    enableBorderResize();
    body->setBg(0xF5F5F7FF);
    state->canvas = body->makeChild<Ling::Canvas>();
    state->canvas->setSizePercent(100.f, 100.f);
    state->canvas->enableSwapChain();
    try { state->folder = MediaHistory::directory(); }
    catch (...) { state->folder.clear(); }

    onMouseMove.add([this](POINT point) { pointerMove(point); });
    onMouseDown.add([this](POINT point, bool right) { pointerDown(point, right); });
    onMouseUp.add([this](POINT point, bool right) { pointerUp(point, right); });
    onMouseWheel.add([this](POINT point, float distance) {
        if (contains(state->listRect(*this), point)) scroll(-distance);
    });
    onKeyDown.add([this](UINT key) { keyDown(key); });
    onBlur.add([this] {
        state->pressed = {};
        state->dragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        refresh();
    });
    onTimer.add([this](UINT id) { if (id == workerTimer) pollWorker(); });

    const auto work = state->work;
    const std::weak_ptr<int> lifetime = state->lifetime;
    const auto dispatcher = Ling::App::get()->dq;
    auto* const owner = this;
    state->worker = std::thread([work, dispatcher, lifetime, owner] {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        for (;;) {
            State::Job job{};
            bool reload = false;
            {
                std::unique_lock lock(work->mutex);
                work->changed.wait(lock, [&] { return work->stop || work->reload || !work->jobs.empty(); });
                if (work->stop) break;
                reload = std::exchange(work->reload, false);
                if (reload) job.generation = work->generation;
                else { job = std::move(work->jobs.front()); work->jobs.pop_front(); }
            }
            if (reload) {
                State::Listing result{job.generation};
                try {
                    std::error_code error;
                    result.entries = MediaHistory::list(&error);
                    result.failed = static_cast<bool>(error);
                } catch (...) { result.failed = true; }
                std::lock_guard lock(work->mutex);
                if (!work->stop && result.generation == work->generation) work->listing = std::move(result);
            } else {
                NativeBitmap bitmap;
                if (SUCCEEDED(apartment)) {
                    try { bitmap.reset(MediaHistory::loadThumbnail(job.path, 320)); }
                    catch (...) { /* Keep the file available even when preview decoding fails. */ }
                }
                std::lock_guard lock(work->mutex);
                if (!work->stop && job.generation == work->generation)
                    work->completed.push_back({job.generation, job.index, std::move(bitmap)});
            }
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
        // No UI or language objects are accessed on this worker. The weak token
        // also prevents a late callback from touching a new window at this address.
        dispatcher.TryEnqueue([owner, lifetime] {
            if (lifetime.expired() || clipboardWindow.get() != owner || !owner->state->closed) return;
            const bool reopen = owner->state->reopen;
            clipboardWindow.reset();
            if (reopen) WinClipboard::init();
            else {
                auto* app = Ling::App::get();
                if (app && app->args[L"--auto-quit"] == L"true" && !WinPin::hasWindow() && !WinCap::get())
                    app->quit(0);
            }
        });
    });
    setTimer(80, workerTimer);
    reload();
    show();
}

void WinClipboard::stopWorker() {
    auto& work = *state->work;
    {
        std::lock_guard lock(work.mutex);
        work.stop = true;
        work.jobs.clear();
        work.completed.clear();
        work.listing.reset();
    }
    work.changed.notify_one();
}

void WinClipboard::reload() {
    if (state->closed) return;
    state->loading = true;
    state->failed = false;
    state->statusError = false;
    state->status.clear();
    state->rows.clear();
    state->entries.clear();
    state->page = 0;
    state->scrollY = 0;
    state->selected = noEntry;
    state->focused = {};
    state->pressed = {};
    auto& work = *state->work;
    {
        std::lock_guard lock(work.mutex);
        state->generation = ++work.generation;
        work.jobs.clear();
        work.completed.clear();
        work.listing.reset();
        work.reload = true;
    }
    work.changed.notify_one();
    refresh();
}

void WinClipboard::showPage(std::size_t page) {
    const auto last = state->entries.empty() ? 0 : (state->entries.size() - 1) / pageSize;
    state->page = (std::min)(page, last);
    state->scrollY = 0;
    state->hovered = {};
    state->pressed = {};
    state->rows.clear();
    auto& work = *state->work;
    {
        std::lock_guard lock(work.mutex);
        state->generation = ++work.generation;
        work.jobs.clear();
        work.completed.clear();
        work.listing.reset();
        work.reload = false;
        const auto end = (std::min)((state->page + 1) * pageSize, state->entries.size());
        for (auto index = state->page * pageSize; index < end; ++index) {
            const auto& entry = state->entries[index];
            State::Row row;
            row.index = index;
            row.metadata = Lang::get(entry.video ? L"clipboard.video" : L"clipboard.image") + L" · " +
                captureTime(entry.path) + L" · " + sizeText(entry.bytes);
            state->rows.push_back(std::move(row));
            work.jobs.push_back({state->generation, index, entry.path});
        }
    }
    work.changed.notify_one();
    refresh();
}

void WinClipboard::pollWorker() {
    if (state->closed) return;
    std::optional<State::Listing> listing;
    std::vector<State::Thumbnail> completed;
    {
        std::lock_guard lock(state->work->mutex);
        listing = std::move(state->work->listing);
        state->work->listing.reset();
        completed.swap(state->work->completed);
    }
    bool changed = false;
    if (listing && listing->generation == state->generation) {
        state->loading = false;
        state->failed = listing->failed;
        if (listing->failed) {
            state->status = Lang::get(L"clipboard.loadFailed");
            state->statusError = true;
        }
        state->entries = std::move(listing->entries);
        state->selected = state->entries.empty() ? noEntry : 0;
        showPage(0);
        changed = true;
    }
    for (auto& thumbnail : completed) {
        if (thumbnail.generation != state->generation) continue;
        for (auto& row : state->rows) {
            if (row.index != thumbnail.index) continue;
            row.nativeBitmap = std::move(thumbnail.bitmap);
            row.loaded = true;
            changed = true;
            break;
        }
    }
    if (changed) refresh();
}

void WinClipboard::layout() {
    Ling::WinBase::layout();
    if (!state->canvas || state->closed) return;
    if (w != state->cachedW || h != state->cachedH || dpi != state->cachedDpi) {
        state->cachedW = w;
        state->cachedH = h;
        state->cachedDpi = dpi;
        state->textCache.clear();
        state->scrollY = (std::min)(state->scrollY, state->maxScroll(*this));
    }
    state->buildTargets(*this);
    paint();
}

void WinClipboard::paint() {
    auto* ctx = state->canvas->startPaint();
    if (!ctx) return;
    if (!state->brush && FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(0x1D1D1F), &state->brush))) {
        state->canvas->finishPaint();
        return;
    }
    const float d = dpi;
    auto* brush = state->brush.Get();
    const auto color = [&](UINT32 rgb) { brush->SetColor(D2D1::ColorF(rgb)); };
    const auto text = [&](const std::wstring& value, D2D1_RECT_F box, float size, UINT32 rgb,
                          bool strong = false, bool center = false) {
        if (value.empty() || box.right <= box.left || box.bottom <= box.top) return;
        if (state->textCache.size() > 160) state->textCache.clear();
        State::TextKey key{value, size * d, box.right - box.left, box.bottom - box.top, strong, center};
        auto [it, inserted] = state->textCache.try_emplace(std::move(key));
        if (inserted) {
            auto* factory = Ling::D2D::get()->dwriteFactory.Get();
            const auto format = Ling::D2D::get()->baseTextFormat.Get();
            factory->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), format,
                box.right - box.left, box.bottom - box.top, &it->second);
            if (it->second) {
                auto* layout = it->second.Get();
                const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(value.size())};
                layout->SetFontFamilyName(L"Segoe UI", range);
                layout->SetFontSize(size * d, range);
                layout->SetFontWeight(strong ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, range);
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                layout->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
                ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis))) {
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    layout->SetTrimming(&trimming, ellipsis.Get());
                }
            }
        }
        color(rgb);
        if (it->second) ctx->DrawTextLayout({box.left, box.top}, it->second.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto button = [&](const Target& target, const std::wstring& label, bool primary = false) {
        const bool hover = target == state->hovered;
        const bool pressed = target == state->pressed;
        const auto shape = D2D1::RoundedRect(target.rect, 11.f * d, 11.f * d);
        color(!target.enabled ? 0xEBEBEF : primary ? (pressed ? 0x0052A5 : 0x0066CC) : (hover || pressed ? 0xE8E8ED : 0xFFFFFF));
        ctx->FillRoundedRectangle(shape, brush);
        if (!primary) {
            color(0xE0E0E0);
            ctx->DrawRoundedRectangle(shape, brush, d);
        }
        if (target.enabled && target == state->focused) {
            color(0x0071E3);
            auto focus = target.rect;
            focus.left -= 3.f * d; focus.top -= 3.f * d;
            focus.right += 3.f * d; focus.bottom += 3.f * d;
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(focus, 14.f * d, 14.f * d), brush, 2.f * d);
        }
        text(label, target.rect, 16.f, !target.enabled ? 0x8B8B91 : primary ? 0xFFFFFF : 0x0066CC, false, true);
    };

    ctx->Clear(D2D1::ColorF(0xF5F5F7));
    text(Lang::get(L"clipboard.title"), D2D1::RectF(24.f * d, 21.f * d, w - 88.f * d, 63.f * d), 28.f, 0x1D1D1F, true);
    text(state->folder.wstring(), D2D1::RectF(24.f * d, 67.f * d, w - 24.f * d, 93.f * d), 14.f, 0x6E6E73);
    for (const auto& target : state->targets) {
        switch (target.action) {
        case Close: {
            if (target == state->hovered || target == state->focused) {
                color(0xE8E8ED);
                ctx->FillRoundedRectangle(D2D1::RoundedRect(target.rect, 11.f * d, 11.f * d), brush);
            }
            if (target == state->focused) {
                color(0x0071E3);
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(target.rect, 11.f * d, 11.f * d), brush, 2.f * d);
            }
            color(0x6E6E73);
            const auto cx = (target.rect.left + target.rect.right) / 2.f;
            const auto cy = (target.rect.top + target.rect.bottom) / 2.f;
            ctx->DrawLine({cx - 5.f * d, cy - 5.f * d}, {cx + 5.f * d, cy + 5.f * d}, brush, 1.7f * d);
            ctx->DrawLine({cx + 5.f * d, cy - 5.f * d}, {cx - 5.f * d, cy + 5.f * d}, brush, 1.7f * d);
            break;
        }
        case OpenFolder: button(target, Lang::get(L"clipboard.openFolder")); break;
        case Refresh: button(target, Lang::get(L"clipboard.refresh")); break;
        case Clear: button(target, Lang::get(L"clipboard.clear")); break;
        case Previous: button(target, Lang::get(L"clipboard.previous")); break;
        case Next: button(target, Lang::get(L"clipboard.next")); break;
        default: break;
        }
    }

    const auto count = std::to_wstring(state->entries.size()) + L" " + Lang::get(L"clipboard.items");
    const auto pages = state->entries.empty() ? 1 : (state->entries.size() + pageSize - 1) / pageSize;
    text(count + L"  ·  " + std::to_wstring(state->page + 1) + L" / " + std::to_wstring(pages),
        D2D1::RectF(28.f * d, 157.f * d, w - 28.f * d, 181.f * d), 14.f, 0x6E6E73);
    const auto list = state->listRect(*this);
    ctx->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (state->rows.empty()) {
        const auto center = (list.top + list.bottom) / 2.f;
        const auto heading = state->loading ? L"clipboard.loading" : state->failed ? L"clipboard.loadFailed" : L"clipboard.empty";
        text(Lang::get(heading), D2D1::RectF(list.left + 24.f * d, center - 42.f * d, list.right - 24.f * d, center), 21.f, 0x1D1D1F, true, true);
        if (!state->loading && !state->failed)
            text(Lang::get(L"clipboard.emptyHint"), D2D1::RectF(list.left + 24.f * d, center + 4.f * d, list.right - 24.f * d, center + 36.f * d), 16.f, 0x6E6E73, false, true);
    }
    for (std::size_t i = 0; i < state->rows.size(); ++i) {
        auto& row = state->rows[i];
        const auto& entry = state->entries[row.index];
        const float top = list.top + i * (rowHeight + rowGap) * d - state->scrollY;
        if (top + rowHeight * d <= list.top || top >= list.bottom) continue;
        const auto card = D2D1::RoundedRect(D2D1::RectF(list.left, top, list.right, top + rowHeight * d), 18.f * d, 18.f * d);
        color(0xFFFFFF);
        ctx->FillRoundedRectangle(card, brush);
        color(row.index == state->selected ? 0x80B6ED : 0xE0E0E0);
        ctx->DrawRoundedRectangle(card, brush, d);
        const auto thumbnail = D2D1::RectF(list.left + 16.f * d, top + 16.f * d, list.left + 144.f * d, top + 96.f * d);
        color(0xF5F5F7);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(thumbnail, 8.f * d, 8.f * d), brush);
        if (row.nativeBitmap) {
            if (!state->wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(state->wic.GetAddressOf()));
            ComPtr<IWICBitmap> image;
            if (state->wic && SUCCEEDED(state->wic->CreateBitmapFromHBITMAP(row.nativeBitmap.get(), nullptr, WICBitmapIgnoreAlpha, &image)))
                static_cast<ID2D1RenderTarget*>(ctx)->CreateBitmapFromWicBitmap(image.Get(), row.bitmap.ReleaseAndGetAddressOf());
            row.nativeBitmap.reset();
        }
        if (row.bitmap) {
            const auto size = row.bitmap->GetSize();
            if (size.width > 0.f && size.height > 0.f) {
                const auto scale = (std::min)((thumbnail.right - thumbnail.left) / size.width, (thumbnail.bottom - thumbnail.top) / size.height);
                const auto left = (thumbnail.left + thumbnail.right - size.width * scale) / 2.f;
                const auto imageTop = (thumbnail.top + thumbnail.bottom - size.height * scale) / 2.f;
                ctx->DrawBitmap(row.bitmap.Get(), D2D1::RectF(left, imageTop, left + size.width * scale, imageTop + size.height * scale), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else {
            text(entry.video ? L"MP4 / GIF" : L"PNG", thumbnail, 14.f, 0x6E6E73, true, true);
        }
        if (entry.video) {
            const auto badge = D2D1::RectF(thumbnail.right - 48.f * d, thumbnail.bottom - 25.f * d, thumbnail.right - 4.f * d, thumbnail.bottom - 4.f * d);
            color(0x0066CC);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(badge, 5.f * d, 5.f * d), brush);
            const auto extension = entry.path.extension().wstring();
            text(extension == L".gif" ? L"GIF" : L"MP4", badge, 12.f, 0xFFFFFF, true, true);
        }
        const float textLeft = list.left + 160.f * d;
        const float textRight = list.right - 240.f * d;
        text(entry.name, D2D1::RectF(textLeft, top + 25.f * d, textRight, top + 56.f * d), 17.f, 0x1D1D1F, true);
        text(row.metadata, D2D1::RectF(textLeft, top + 60.f * d, textRight, top + 90.f * d), 14.f, 0x6E6E73);
        for (const auto& target : state->targets) {
            if (target.index != row.index) continue;
            if (target.action == Copy) button(target, Lang::get(L"clipboard.copy"), true);
            else if (target.action == Reveal) button(target, Lang::get(L"clipboard.reveal"));
            else if (target.action == Preview && target == state->focused) {
                color(0x0071E3);
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(target.rect, 8.f * d, 8.f * d), brush, 2.f * d);
            }
        }
    }
    ctx->PopAxisAlignedClip();
    if (state->maxScroll(*this) > 0.f) {
        color(state->dragging ? 0x6E6E73 : 0xB6B6BC);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(state->scrollbar(*this), 3.f * d, 3.f * d), brush);
    }
    text(state->status, D2D1::RectF(24.f * d, h - 94.f * d, w - 24.f * d, h - 65.f * d), 14.f, state->statusError ? 0xB42318 : 0x0066CC);
    text(Lang::get(L"clipboard.shortcuts"), D2D1::RectF(24.f * d, h - 58.f * d, w - 270.f * d, h - 18.f * d), 14.f, 0x6E6E73);
    state->canvas->finishPaint();
}

void WinClipboard::scroll(float amount) {
    state->scrollY = std::clamp(state->scrollY + amount, 0.f, state->maxScroll(*this));
    state->buildTargets(*this);
    state->hovered = {};
    refresh();
}

void WinClipboard::selectEntry(std::size_t index) {
    if (state->entries.empty()) return;
    index = (std::min)(index, state->entries.size() - 1);
    if (index / pageSize != state->page) showPage(index / pageSize);
    state->selected = index;
    const auto list = state->listRect(*this);
    const float top = static_cast<float>(index % pageSize) * (rowHeight + rowGap) * dpi;
    const auto viewport = list.bottom - list.top;
    if (top < state->scrollY) state->scrollY = top;
    else if (top + rowHeight * dpi > state->scrollY + viewport) state->scrollY = top + rowHeight * dpi - viewport;
    state->scrollY = std::clamp(state->scrollY, 0.f, state->maxScroll(*this));
    state->buildTargets(*this);
    refresh();
}

void WinClipboard::pointerMove(POINT position) {
    if (state->dragging && GetCapture() != hwnd) state->dragging = false;
    if (position.x == INT_MAX && position.y == INT_MAX) {
        state->hovered = {};
        refresh();
        return;
    }
    if (state->dragging) {
        const auto list = state->listRect(*this);
        const auto thumb = state->scrollbar(*this);
        const auto available = list.bottom - list.top - (thumb.bottom - thumb.top);
        if (available > 0.f) scroll(state->dragScroll + (position.y - state->dragY) * state->maxScroll(*this) / available - state->scrollY);
        return;
    }
    state->buildTargets(*this);
    const auto target = state->hit(*this, position);
    if (!(target == state->hovered)) { state->hovered = target; refresh(); }
}

void WinClipboard::pointerDown(POINT position, bool right) {
    if (right) return;
    SetFocus(hwnd);
    state->buildTargets(*this);
    const auto list = state->listRect(*this);
    if (state->maxScroll(*this) > 0.f && position.x >= w - 18.f * dpi && position.y >= list.top && position.y < list.bottom) {
        const auto thumb = state->scrollbar(*this);
        if (!contains(thumb, position)) scroll((position.y < thumb.top ? -1.f : 1.f) * (list.bottom - list.top));
        state->dragging = true;
        state->dragY = static_cast<float>(position.y);
        state->dragScroll = state->scrollY;
        SetCapture(hwnd);
        return;
    }
    state->pressed = state->hit(*this, position);
    state->focused = state->pressed;
    if (rowAction(state->pressed.action)) state->selected = state->pressed.index;
    if (state->pressed.action != None) SetCapture(hwnd);
    refresh();
}

void WinClipboard::pointerUp(POINT position, bool right) {
    if (right) return;
    const auto pressed = state->pressed;
    state->pressed = {};
    state->dragging = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    state->buildTargets(*this);
    if (pressed.action != None && pressed == state->hit(*this, position)) activate(pressed.action, pressed.index);
    if (!state->closed) refresh();
}

void WinClipboard::keyDown(UINT key) {
    if (state->closed) return;
    state->buildTargets(*this);
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_ESCAPE) { close(); return; }
    if (key == VK_F5) { reload(); return; }
    if (control && key == 'C') { activate(Copy, state->selected); return; }
    if (key == VK_TAB) {
        std::vector<Target> stops;
        for (const auto& target : state->targets)
            if (target.enabled && target.action != Select) stops.push_back(target);
        if (stops.empty()) return;
        const auto found = std::find(stops.begin(), stops.end(), state->focused);
        const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        std::size_t next;
        if (found == stops.end()) next = back ? stops.size() - 1 : 0;
        else {
            const auto index = static_cast<std::size_t>(found - stops.begin());
            next = back ? (index + stops.size() - 1) % stops.size() : (index + 1) % stops.size();
        }
        state->focused = stops[next];
        if (rowAction(state->focused.action)) selectEntry(state->focused.index);
        refresh();
        return;
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        if (state->focused.action != None && state->focused.action != Select)
            activate(state->focused.action, state->focused.index);
        else activate(Preview, state->selected);
        return;
    }
    if (key == VK_PRIOR || key == VK_NEXT) {
        if (control) activate(key == VK_PRIOR ? Previous : Next, noEntry);
        else {
            const auto list = state->listRect(*this);
            scroll((key == VK_PRIOR ? -1.f : 1.f) * (list.bottom - list.top));
        }
        return;
    }
    if (state->entries.empty()) return;
    auto index = state->selected == noEntry ? std::size_t{0} : state->selected;
    if (key == VK_UP) { if (index > 0) --index; }
    else if (key == VK_DOWN) index = (std::min)(index + 1, state->entries.size() - 1);
    else if (key == VK_HOME) index = 0;
    else if (key == VK_END) index = state->entries.size() - 1;
    else return;
    selectEntry(index);
    state->focused = {Preview, index};
    refresh();
}

void WinClipboard::activate(int action, std::size_t index) {
    if (state->closed) return;
    if (action == Close) { close(); return; }
    if (action == Refresh) { reload(); return; }
    if (action == Previous || action == Next) {
        if (state->loading) return;
        if (action == Previous && state->page > 0) showPage(state->page - 1);
        else if (action == Next && (state->page + 1) * pageSize < state->entries.size()) showPage(state->page + 1);
        state->selected = state->rows.empty() ? noEntry : state->rows.front().index;
        state->focused = {action};
        return;
    }
    if (action == Select) { selectEntry(index); return; }
    if (action == Clear) {
        if (state->loading || state->entries.empty()) return;
        if (MessageBoxW(hwnd, Lang::get(L"clipboard.clearConfirm").c_str(), title.c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        const bool success = MediaHistory::clear(hwnd);
        reload();
        if (!success) { state->status = Lang::get(L"clipboard.failed"); state->statusError = true; }
        return;
    }
    bool success = false;
    try {
        if (action == OpenFolder && !state->folder.empty()) {
            success = MediaHistory::reveal(hwnd, state->folder);
        } else if (index < state->entries.size()) {
            const auto& entry = state->entries[index];
            if (action == Copy) success = MediaHistory::copy(hwnd, entry);
            else if (action == Reveal) success = MediaHistory::reveal(hwnd, entry.path);
            else if (action == Preview) {
                std::error_code error;
                if (std::filesystem::is_regular_file(entry.path, error) && !error)
                    success = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", entry.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
            } else return;
        } else return;
    } catch (...) { success = false; }
    state->statusError = !success;
    state->status = success ? (action == Copy ? Lang::get(L"clipboard.copied") : L"") : Lang::get(L"clipboard.failed");
    refresh();
}

LRESULT WinClipboard::onHitTest(POINT position) {
    ScreenToClient(hwnd, &position);
    if (!isMaximized) {
        const auto border = borderHitTest(position);
        if (border != HTCLIENT) return border;
    }
    if (position.y > 0 && position.y < 64.f * dpi && position.x < w - 76.f * dpi) return HTCAPTION;
    return HTCLIENT;
}

BOOL WinClipboard::setCursor() {
    POINT position{};
    GetCursorPos(&position);
    ScreenToClient(hwnd, &position);
    state->buildTargets(*this);
    const auto target = state->hit(*this, position);
    SetCursor(LoadCursor(nullptr, target.action != None && target.action != Select ? IDC_HAND : IDC_ARROW));
    return TRUE;
}
