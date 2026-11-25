// The canonical names and Win32 registration behavior come from the existing
// Setting / WinSettingShortcut and Ling Util::strToKey, commit
// 2ae45005b2863b004b4432bfe85d6e63d7ee44ca (MIT, xland). See docs/upstream.md.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform/GlobalShortcutBackend.h"
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <array>
#include <utility>

namespace HuhuCapture::Platform {
namespace {
using namespace Desktop;

struct NativeChord { UINT modifiers{}, key{}; };
UINT nativeKey(const QString& name)
{
    if (name.size() == 1) {
        const auto ch = name.front().unicode();
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return ch;
    }
    if (name.size() == 4 && name.startsWith(QStringLiteral("Num")) && name.back() >= QChar(u'0') && name.back() <= QChar(u'9'))
        return VK_NUMPAD0 + name.back().unicode() - u'0';
    if (name.startsWith(QChar(u'F'))) {
        bool valid{}; const int index = name.mid(1).toInt(&valid);
        if (valid && index >= 1 && index <= 24) return static_cast<UINT>(VK_F1 + index - 1);
    }
    struct Mapping { const char16_t* name; UINT key; };
    constexpr std::array mappings{
        Mapping{u"Up", VK_UP}, Mapping{u"Down", VK_DOWN}, Mapping{u"Left", VK_LEFT}, Mapping{u"Right", VK_RIGHT},
        Mapping{u"Enter", VK_RETURN}, Mapping{u"Esc", VK_ESCAPE}, Mapping{u"Space", VK_SPACE}, Mapping{u"Tab", VK_TAB},
        Mapping{u"Backspace", VK_BACK}, Mapping{u"Delete", VK_DELETE}, Mapping{u"Insert", VK_INSERT},
        Mapping{u"Home", VK_HOME}, Mapping{u"End", VK_END}, Mapping{u"PageUp", VK_PRIOR}, Mapping{u"PageDown", VK_NEXT},
        Mapping{u"PrintScreen", VK_SNAPSHOT}, Mapping{u"ScrollLock", VK_SCROLL}, Mapping{u"Pause", VK_PAUSE},
        Mapping{u"NumLock", VK_NUMLOCK}, Mapping{u"CapsLock", VK_CAPITAL},
        // Preserve the existing numeric operator names. New keypad digits keep
        // the distinct Num0..Num9 form, as in the old recorder.
        Mapping{u"*", VK_MULTIPLY}, Mapping{u"+", VK_ADD}, Mapping{u"-", VK_SUBTRACT},
        Mapping{u"/", VK_DIVIDE}, Mapping{u".", VK_DECIMAL}, Mapping{u"`", VK_OEM_3}, Mapping{u"~", VK_OEM_3},
        Mapping{u"=", VK_OEM_PLUS}, Mapping{u"[", VK_OEM_4}, Mapping{u"]", VK_OEM_6},
        Mapping{u"\\", VK_OEM_5}, Mapping{u";", VK_OEM_1}, Mapping{u"'", VK_OEM_7}, Mapping{u",", VK_OEM_COMMA},
    };
    for (const auto& mapping : mappings) if (name == QString::fromUtf16(mapping.name)) return mapping.key;
    if (name.size() == 1) {
        const auto value = VkKeyScanW(static_cast<WCHAR>(name.front().unicode()));
        if (value != -1) return static_cast<UINT>(value & 0xff);
    }
    return 0;
}
NativeChord nativeChord(const QStringList& keys)
{
    NativeChord chord;
    for (const auto& key : keys) {
        if (key == QStringLiteral("Ctrl")) chord.modifiers |= MOD_CONTROL;
        else if (key == QStringLiteral("Alt")) chord.modifiers |= MOD_ALT;
        else if (key == QStringLiteral("Shift")) chord.modifiers |= MOD_SHIFT;
        else if (key == QStringLiteral("Win")) chord.modifiers |= MOD_WIN;
        else chord.key = nativeKey(key);
    }
    return chord;
}
ShortcutFailure registerChord(HWND window, int id, NativeChord chord)
{
    if (!chord.key || chord.key == VK_F12) return {ShortcutError::InvalidShortcut};
    if (RegisterHotKey(window, id, chord.modifiers | MOD_NOREPEAT, chord.key)) return {};
    const auto error = GetLastError();
    return {error == ERROR_HOTKEY_ALREADY_REGISTERED ? ShortcutError::Conflict : ShortcutError::Unavailable, error};
}

class WindowsShortcut final : public GlobalShortcutBackend, public QAbstractNativeEventFilter {
public:
    explicit WindowsShortcut(GlobalShortcut& owner) : GlobalShortcutBackend(owner) {}
    ~WindowsShortcut() override { shutdown(); }
    void initialize() override
    {
        if (state_.initialized || stopped_) return;
        state_.initialized = true;
        if (!qGuiApp || QGuiApplication::platformName() != QStringLiteral("windows")) {
            state_.failure = {ShortcutError::Unsupported};
        } else {
            window_ = std::make_unique<QWindow>(); window_->setFlags(Qt::Tool);
            window_->create(); handle_ = reinterpret_cast<HWND>(window_->winId());
            if (handle_) {
                state_.editing = ShortcutEditing::LocalCapture;
                QCoreApplication::instance()->installNativeEventFilter(this); installed_ = true;
            } else state_.failure = {ShortcutError::Unavailable};
        }
        const QPointer<WindowsShortcut> life(this);
        Q_EMIT owner_.initialized(); if (life) Q_EMIT owner_.changed();
    }
    ShortcutSnapshot snapshot() const override { return state_; }
    void bind(quint64 request, const QStringList& keys, QWindow*, GlobalShortcut::Persist persist, bool) override
    {
        if (stopped_ || !handle_ || state_.editing != ShortcutEditing::LocalCapture) {
            Q_EMIT owner_.bindingFinished(request, {ShortcutError::Unavailable}); return;
        }
        if (state_.suspended || pending_) { Q_EMIT owner_.bindingFinished(request, {ShortcutError::Busy}); return; }
        pending_ = request; cancelled_ = false;
        const auto chord = nativeChord(keys);
        const bool same = state_.registered && chord.key == activeChord_.key && chord.modifiers == activeChord_.modifiers;
        const int next = activeId_ == 1 ? 2 : 1;
        if (!same) {
            if (const auto error = registerChord(handle_, next, chord)) { finish(request, error); return; }
            candidateId_ = next;
        }
        const QPointer<WindowsShortcut> life(this);
        bool saved = true;
        try { if (persist) saved = persist(); } catch (...) { saved = false; }
        if (!life) return;
        if (!saved || cancelled_ || stopped_) {
            if (candidateId_ && !UnregisterHotKey(handle_, std::exchange(candidateId_, 0))) {
                const auto error = GetLastError(); releaseNative();
                finish(request, {ShortcutError::RestoreFailed, error});
                if (life) Q_EMIT owner_.changed(); return;
            }
            finish(request, {saved ? ShortcutError::Cancelled : ShortcutError::SaveFailed}); return;
        }
        if (!same) {
            if (activeId_ && state_.registered && !UnregisterHotKey(handle_, activeId_)) {
                const auto error = GetLastError(); releaseNative();
                finish(request, {ShortcutError::Unavailable, error});
                if (life) Q_EMIT owner_.changed(); return;
            }
            activeId_ = std::exchange(candidateId_, 0); activeChord_ = chord;
        }
        clearQueued();
        state_.requestedKeys = keys; state_.actualTrigger = keys.join(QChar(u'+'));
        state_.registered = true; state_.failure = {};
        finish(request, {}); if (life) Q_EMIT owner_.changed();
    }
    void cancel(quint64 request) override { if (pending_ == request) cancelled_ = true; }
    ShortcutFailure setCaptureActive(bool active) override
    {
        if (stopped_ || !handle_) return {ShortcutError::Unavailable};
        if (state_.suspended == active) return {};
        state_.suspended = active;
        ShortcutFailure result;
        if (active) {
            if (state_.registered && activeId_) {
                if (!UnregisterHotKey(handle_, activeId_)) {
                    result = {ShortcutError::Unavailable, GetLastError()}; releaseNative();
                }
                else state_.registered = false;
            }
        } else if (activeId_ && !state_.registered) {
            result = registerChord(handle_, activeId_, activeChord_);
            state_.registered = !result;
            if (result) result.code = ShortcutError::RestoreFailed;
        }
        clearQueued(); state_.failure = result; Q_EMIT owner_.changed(); return result;
    }
    void shutdown() override
    {
        if (std::exchange(stopped_, true)) return;
        releaseNative();
    }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr* result) override
    {
        const auto* event = static_cast<const MSG*>(message);
        if (!event || event->hwnd != handle_ || event->message != WM_HOTKEY) return false;
        if (result) *result = 0;
        if (stopped_ || state_.suspended || !state_.registered || event->wParam != static_cast<WPARAM>(activeId_)) return true;
        if (static_cast<LONG>(event->time - acceptAfter_) < 0 ||
            HIWORD(event->lParam) != activeChord_.key || LOWORD(event->lParam) != activeChord_.modifiers) return true;
        Q_EMIT owner_.activated({}); return true;
    }
private:
    void releaseNative()
    {
        if (installed_ && QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
        installed_ = false;
        if (handle_) {
            if (candidateId_) UnregisterHotKey(handle_, std::exchange(candidateId_, 0));
            if (activeId_ && state_.registered) UnregisterHotKey(handle_, activeId_);
            clearQueued();
        }
        state_.registered = false; state_.editing = ShortcutEditing::Unavailable;
        // Destroying this owned HWND also releases any native registration
        // whose individual UnregisterHotKey call reported an error.
        window_.reset(); handle_ = nullptr; activeId_ = candidateId_ = 0;
    }
    void finish(quint64 request, ShortcutFailure failure)
    { pending_ = 0; state_.failure = failure; Q_EMIT owner_.bindingFinished(request, failure); }
    void clearQueued()
    {
        MSG stale{};
        while (handle_ && PeekMessageW(&stale, handle_, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {}
        acceptAfter_ = GetTickCount();
    }
    ShortcutSnapshot state_;
    std::unique_ptr<QWindow> window_;
    HWND handle_{};
    NativeChord activeChord_;
    quint64 pending_{};
    DWORD acceptAfter_{};
    int activeId_{}, candidateId_{};
    bool installed_{}, stopped_{}, cancelled_{};
};
}

std::unique_ptr<GlobalShortcutBackend> createGlobalShortcutBackend(Desktop::GlobalShortcut& owner)
{ return std::make_unique<WindowsShortcut>(owner); }

} // namespace HuhuCapture::Platform
