// X11 passive-grab / lock-modifier behavior adapts QHotkey's X11 backend,
// Copyright (c) 2016, Felix Barz, BSD-3-Clause. The upstream source and license
// are pinned in third_party/qhotkey/upstream.md. Checked XCB requests replace
// its process-global Xlib error handler. xcb-keysyms and xkbcommon own keyboard
// mapping; the existing PortalShortcuts owns the complete Wayland protocol.
#include "platform/GlobalShortcutBackend.h"
#include "platform/linux/portal/PortalShortcuts.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <QWindow>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace HuhuCapture::Platform {
namespace {
using namespace Desktop;
using Linux::PortalFailure;
Q_LOGGING_CATEGORY(shortcutTrace, "huhucapture.shortcuts.x11", QtWarningMsg)

template<class T> struct Free { void operator()(T* value) const noexcept { std::free(value); } };
template<class T> using CPointer = std::unique_ptr<T, Free<T>>;

QString symbolName(const QString& key)
{
    struct Mapping { const char16_t* stored; const char16_t* symbol; };
    constexpr std::array mappings{
        Mapping{u"Enter", u"Return"}, Mapping{u"Esc", u"Escape"}, Mapping{u"Space", u"space"},
        Mapping{u"Backspace", u"BackSpace"}, Mapping{u"PageUp", u"Prior"}, Mapping{u"PageDown", u"Next"},
        Mapping{u"PrintScreen", u"Print"}, Mapping{u"ScrollLock", u"Scroll_Lock"},
        Mapping{u"NumLock", u"Num_Lock"}, Mapping{u"CapsLock", u"Caps_Lock"},
        // These punctuation names denote numeric operators in legacy config.
        Mapping{u"*", u"KP_Multiply"}, Mapping{u"+", u"KP_Add"}, Mapping{u"-", u"KP_Subtract"},
        Mapping{u"/", u"KP_Divide"}, Mapping{u".", u"KP_Decimal"},
    };
    for (const auto& mapping : mappings) if (key == QString::fromUtf16(mapping.stored)) return QString::fromUtf16(mapping.symbol);
    if (key.size() == 4 && key.startsWith(QStringLiteral("Num")) && key.back() >= QChar(u'0') && key.back() <= QChar(u'9'))
        return QStringLiteral("KP_") + key.back();
    if (key.size() == 1) {
        const auto value = key.front().isLetter() ? key.front().toLower().unicode() : key.front().unicode();
        const auto symbol = xkb_utf32_to_keysym(value);
        std::array<char, 128> name{};
        return xkb_keysym_get_name(symbol, name.data(), name.size()) > 0 ? QString::fromLatin1(name.data()) : QString();
    }
    return key;
}
QString portalTrigger(const QStringList& keys)
{
    QStringList output;
    for (const auto& key : keys) {
        if (key == QStringLiteral("Ctrl")) output.push_back(QStringLiteral("CTRL"));
        else if (key == QStringLiteral("Alt")) output.push_back(QStringLiteral("ALT"));
        else if (key == QStringLiteral("Shift")) output.push_back(QStringLiteral("SHIFT"));
        else if (key == QStringLiteral("Win")) output.push_back(QStringLiteral("LOGO"));
        else {
            const auto name = symbolName(key);
            if (name.isEmpty() || xkb_keysym_from_name(name.toLatin1().constData(), XKB_KEYSYM_NO_FLAGS) == XKB_KEY_NoSymbol) return {};
            output.push_back(name);
        }
    }
    return output.join(QChar(u'+'));
}
ShortcutFailure portalFailure(const PortalFailure& value)
{
    using Code = PortalFailure::Code;
    switch (value.code) {
    case Code::None: return {};
    case Code::Cancelled: case Code::ParentDestroyed: return {ShortcutError::Cancelled, value.nativeCode};
    case Code::Denied: return {ShortcutError::Denied, value.nativeCode};
    case Code::Unsupported: return {ShortcutError::Unsupported, value.nativeCode};
    case Code::Busy: return {ShortcutError::Busy, value.nativeCode};
    case Code::InvalidState: return {ShortcutError::InvalidShortcut, value.nativeCode};
    case Code::Unavailable: case Code::Failed: return {ShortcutError::Unavailable, value.nativeCode};
    }
    return {ShortcutError::Unavailable, value.nativeCode};
}

struct Grab {
    xcb_window_t root{};
    xcb_keycode_t key{};
    uint16_t modifiers{};
    friend bool operator==(const Grab&, const Grab&) = default;
};
QString describe(const std::vector<Grab>& grabs)
{
    QStringList result;
    for (const auto& grab : grabs) result.push_back(QStringLiteral("%1:%2:%3").arg(grab.root).arg(grab.key).arg(grab.modifiers));
    return result.join(QChar(u','));
}

class LinuxShortcut final : public GlobalShortcutBackend {
public:
    explicit LinuxShortcut(GlobalShortcut& owner) : GlobalShortcutBackend(owner) {}
    ~LinuxShortcut() override { shutdown(); }
    void initialize() override
    {
        if (initializing_ || stopped_) return;
        if (state_.initialized) {
            if (portal_ && !portal_->available() && !pending_) {
                initializing_ = true; state_.initialized = false; portal_->initialize();
            }
            return;
        }
        initializing_ = true;
        const auto platform = QGuiApplication::platformName();
        bool portal = platform.startsWith(QStringLiteral("wayland")) ||
            (platform == QStringLiteral("xcb") &&
             (qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0 ||
              !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")));
        if (!portal && platform == QStringLiteral("xcb")) {
            connection_ = xcb_connect(nullptr, nullptr);
            if (!connection_ || xcb_connection_has_error(connection_)) { completeInitialization({ShortcutError::Unavailable}); return; }
            // New XWayland servers identify themselves explicitly. Older ones
            // are conservatively recognized by the actual session environment.
            CPointer<xcb_query_extension_reply_t> extension(xcb_query_extension_reply(connection_,
                xcb_query_extension(connection_, 8, "XWAYLAND"), nullptr));
            portal = extension && extension->present;
        }
        if (portal) {
            if (connection_) xcb_disconnect(std::exchange(connection_, nullptr));
            initializePortal(); return;
        }
        if (platform != QStringLiteral("xcb")) { completeInitialization({ShortcutError::Unsupported}); return; }
        symbols_ = xcb_key_symbols_alloc(connection_);
        if (!symbols_) { completeInitialization({ShortcutError::Unavailable}); return; }
        const auto* setup = xcb_get_setup(connection_);
        if (!setup) { completeInitialization({ShortcutError::Unavailable}); return; }
        for (auto screen = xcb_setup_roots_iterator(setup); screen.rem; xcb_screen_next(&screen)) roots_.push_back(screen.data->root);
        if (roots_.empty() || roots_.size() > 16) { completeInitialization({ShortcutError::Unsupported}); return; }
        notifier_ = std::make_unique<QSocketNotifier>(xcb_get_file_descriptor(connection_), QSocketNotifier::Read, this);
        connect(notifier_.get(), &QSocketNotifier::activated, this, [this] {
            qCDebug(shortcutTrace) << "socket readable";
            drainEvents();
        });
        qCDebug(shortcutTrace) << "initialized XCB fd" << xcb_get_file_descriptor(connection_) << "roots" << roots_.size();
        state_.editing = ShortcutEditing::LocalCapture; completeInitialization({});
    }
    ShortcutSnapshot snapshot() const override { return state_; }
    void bind(quint64 request, const QStringList& keys, QWindow* parent, GlobalShortcut::Persist persist, bool unparented) override
    {
        if (stopped_ || !state_.initialized || state_.editing == ShortcutEditing::Unavailable) {
            Q_EMIT owner_.bindingFinished(request, {ShortcutError::Unavailable}); return;
        }
        if (pending_ || state_.suspended) { Q_EMIT owner_.bindingFinished(request, {ShortcutError::Busy}); return; }
        pending_ = request; pendingKeys_ = keys; cancelled_ = false; commitRejected_ = false;
        if (portal_) {
            const auto trigger = portalTrigger(keys);
            if (trigger.isEmpty()) { finish({ShortcutError::InvalidShortcut}); return; }
            const QVector<Linux::PortalShortcutRequest> requested{{QStringLiteral("capture"), description_, trigger}};
            const QPointer<LinuxShortcut> life(this);
            auto commit = [life, persist = std::move(persist)] {
                if (!life || life->stopped_ || life->cancelled_) return false;
                bool accepted = true;
                try { if (persist) accepted = persist(); } catch (...) { accepted = false; }
                if (life) life->commitRejected_ = !accepted;
                return accepted && life && !life->stopped_ && !life->cancelled_;
            };
            portalRequest_ = unparented && !parent ? portal_->restoreShortcuts(requested, std::move(commit)) :
                portal_->bindShortcuts(parent, requested, std::move(commit));
            return;
        }
        std::vector<Grab> candidate;
        auto failure = acquire(keys, candidate, active_);
        if (failure) { finish(failure); return; }
        candidate_ = candidate;
        const QPointer<LinuxShortcut> life(this);
        bool accepted = true;
        try { if (persist) accepted = persist(); } catch (...) { accepted = false; }
        if (!life) return;
        if (!accepted || cancelled_ || stopped_) {
            const auto released = releaseExcept(candidate_, active_); candidate_.clear();
            if (released) { closeX11(); finish({ShortcutError::RestoreFailed, released.nativeCode}); }
            else finish({accepted ? ShortcutError::Cancelled : ShortcutError::SaveFailed});
            return;
        }
        const auto released = releaseExcept(active_, candidate);
        if (released) { closeX11(); finish(released); if (life) Q_EMIT owner_.changed(); return; }
        active_ = std::move(candidate); candidate_.clear();
        drainStaleKeys();
        state_.requestedKeys = keys; state_.actualTrigger = keys.join(QChar(u'+'));
        state_.registered = true; state_.failure = {};
        qCDebug(shortcutTrace) << "binding active" << describe(active_) << "generation" << keyGeneration_;
        finish({}); if (life) Q_EMIT owner_.changed();
    }
    void cancel(quint64 request) override
    {
        if (pending_ != request) return;
        cancelled_ = true;
        if (portal_ && portalRequest_) portal_->cancelRequest(portalRequest_);
    }
    ShortcutFailure setCaptureActive(bool active) override
    {
        if (stopped_) return {ShortcutError::Unavailable};
        if (state_.suspended == active) return {};
        state_.suspended = active; heldKey_ = 0;
        ShortcutFailure failure;
        if (!portal_) {
            if (active) {
                failure = releaseExcept(active_, {}); active_.clear(); state_.registered = false;
                if (failure) closeX11();
            } else if (!state_.requestedKeys.isEmpty()) {
                failure = acquire(state_.requestedKeys, active_, {}); state_.registered = !failure;
                if (failure) failure.code = ShortcutError::RestoreFailed;
            }
            drainStaleKeys();
        }
        state_.failure = failure; Q_EMIT owner_.changed(); return failure;
    }
    void shutdown() override
    {
        if (std::exchange(stopped_, true)) return;
        if (portal_) { portal_->cancelInitialization(); portal_->closeBindings(); }
        closeX11();
        state_.registered = false; state_.editing = ShortcutEditing::Unavailable;
    }
private:
    void completeInitialization(ShortcutFailure failure)
    {
        initializing_ = false; state_.initialized = true; state_.failure = failure;
        if (failure) state_.editing = ShortcutEditing::Unavailable;
        const QPointer<LinuxShortcut> life(this);
        Q_EMIT owner_.initialized(); if (life) Q_EMIT owner_.changed();
    }
    void initializePortal()
    {
        state_.portal = true; portal_ = std::make_unique<Linux::PortalShortcuts>();
        connect(portal_.get(), &Linux::PortalShortcuts::initialized, this,
            [this](const Linux::PortalShortcutCapabilities& capabilities, const PortalFailure& failure) {
                if (stopped_) {
                    initializing_ = false; state_.initialized = true; Q_EMIT owner_.initialized(); return;
                }
                if (capabilities.globalShortcuts.available() && !failure) state_.editing = ShortcutEditing::SystemDialog;
                completeInitialization(portalFailure(failure));
            });
        connect(portal_.get(), &Linux::PortalShortcuts::bindingFinished, this,
            [this](quint64 request, const QVector<Linux::PortalShortcutBinding>&, const PortalFailure& failure) {
                if (!portalRequest_ || portalRequest_ != request) return;
                portalRequest_ = 0;
                auto result = portalFailure(failure);
                if (cancelled_ || stopped_) result = {ShortcutError::Cancelled};
                else if (commitRejected_) result = {ShortcutError::SaveFailed};
                if (!result) state_.requestedKeys = pendingKeys_;
                readPortalBindings(); const QPointer<LinuxShortcut> life(this);
                finish(result); if (life) Q_EMIT owner_.changed();
            });
        connect(portal_.get(), &Linux::PortalShortcuts::bindingsChanged, this,
            [this] { if (!stopped_) { readPortalBindings(); Q_EMIT owner_.changed(); } });
        connect(portal_.get(), &Linux::PortalShortcuts::bindingsClosed, this, [this](const PortalFailure& failure) {
            state_.registered = false; state_.actualTrigger.clear(); state_.failure = portalFailure(failure);
            if (!portal_->available()) state_.editing = ShortcutEditing::Unavailable;
            if (!stopped_) Q_EMIT owner_.changed();
        });
        connect(portal_.get(), &Linux::PortalShortcuts::bindingsRefreshFailed, this, [this](const PortalFailure& failure) {
            state_.failure = portalFailure(failure); if (!stopped_) Q_EMIT owner_.changed();
        });
        connect(portal_.get(), &Linux::PortalShortcuts::activated, this, [this](const QString& id, quint64, const QString& token) {
            if (!stopped_ && !state_.suspended && state_.registered && id == QStringLiteral("capture")) Q_EMIT owner_.activated(token);
        });
        portal_->initialize();
    }
    void readPortalBindings()
    {
        state_.registered = false; state_.actualTrigger.clear();
        for (const auto& binding : portal_->bindings()) if (binding.id == QStringLiteral("capture") && !binding.triggerDescription.isEmpty()) {
            state_.registered = portal_->hasActiveBindings(); state_.actualTrigger = binding.triggerDescription; break;
        }
    }
    void finish(ShortcutFailure failure)
    {
        const auto request = std::exchange(pending_, 0); pendingKeys_.clear();
        state_.failure = failure; if (request) Q_EMIT owner_.bindingFinished(request, failure);
    }
    std::vector<uint16_t> masksFor(xcb_get_modifier_mapping_reply_t* mapping, std::initializer_list<xcb_keysym_t> wanted) const
    {
        std::vector<uint16_t> result;
        const auto* codes = xcb_get_modifier_mapping_keycodes(mapping);
        for (unsigned modifier = 0; modifier < 8; ++modifier) {
            bool found{};
            for (unsigned index = 0; index < mapping->keycodes_per_modifier && !found; ++index) {
                const auto code = codes[modifier * mapping->keycodes_per_modifier + index];
                if (!code) continue;
                for (int column = 0; column < 4 && !found; ++column) {
                    const auto symbol = xcb_key_symbols_get_keysym(symbols_, code, column);
                    found = std::find(wanted.begin(), wanted.end(), symbol) != wanted.end();
                }
            }
            if (found) result.push_back(static_cast<uint16_t>(1u << modifier));
        }
        return result;
    }
    ShortcutFailure acquire(const QStringList& keys, std::vector<Grab>& output, const std::vector<Grab>& retained)
    {
        if (!connection_ || !symbols_ || xcb_connection_has_error(connection_)) return {ShortcutError::Unavailable};
        CPointer<xcb_get_modifier_mapping_reply_t> mapping(xcb_get_modifier_mapping_reply(connection_, xcb_get_modifier_mapping(connection_), nullptr));
        if (!mapping) return {ShortcutError::Unavailable};
        std::vector<uint16_t> modifiers{0}; xcb_keysym_t symbol{};
        for (const auto& key : keys) {
            std::vector<uint16_t> choices;
            if (key == QStringLiteral("Ctrl")) choices = {XCB_MOD_MASK_CONTROL};
            else if (key == QStringLiteral("Shift")) choices = {XCB_MOD_MASK_SHIFT};
            else if (key == QStringLiteral("Alt")) choices = masksFor(mapping.get(), {XKB_KEY_Alt_L, XKB_KEY_Alt_R});
            else if (key == QStringLiteral("Win")) choices = masksFor(mapping.get(), {XKB_KEY_Super_L, XKB_KEY_Super_R});
            else { symbol = xkb_keysym_from_name(symbolName(key).toLatin1().constData(), XKB_KEYSYM_NO_FLAGS); continue; }
            if (choices.empty()) return {ShortcutError::Unsupported};
            std::vector<uint16_t> next;
            for (const auto base : modifiers) for (const auto choice : choices) next.push_back(static_cast<uint16_t>(base | choice));
            modifiers = std::move(next);
        }
        if (!symbol) return {ShortcutError::InvalidShortcut};
        CPointer<xcb_keycode_t> keycodes(xcb_key_symbols_get_keycode(symbols_, symbol));
        if (!keycodes || !*keycodes) return {ShortcutError::Unsupported};
        auto locks = masksFor(mapping.get(), {XKB_KEY_Num_Lock, XKB_KEY_Caps_Lock, XKB_KEY_Shift_Lock, XKB_KEY_Scroll_Lock});
        std::vector<uint16_t> lockChoices{0};
        for (const auto lock : locks) {
            const auto size = lockChoices.size();
            for (std::size_t index = 0; index < size; ++index) lockChoices.push_back(static_cast<uint16_t>(lockChoices[index] | lock));
        }
        std::vector<Grab> candidate;
        for (std::size_t index = 0; keycodes.get()[index]; ++index) {
            if (index >= 32) return {ShortcutError::Unsupported};
            for (const auto root : roots_) for (const auto base : modifiers) for (const auto lock : lockChoices) {
                const Grab grab{root, keycodes.get()[index], static_cast<uint16_t>(base | lock)};
                if (std::find(candidate.begin(), candidate.end(), grab) == candidate.end()) candidate.push_back(grab);
                if (candidate.size() > 256) return {ShortcutError::Unsupported};
            }
        }
        std::vector<Grab> acquired;
        for (const auto& grab : candidate) {
            if (std::find(retained.begin(), retained.end(), grab) != retained.end()) continue;
            CPointer<xcb_generic_error_t> error(xcb_request_check(connection_, xcb_grab_key_checked(connection_, false,
                grab.root, grab.modifiers, grab.key, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC)));
            if (error || xcb_connection_has_error(connection_)) {
                const auto failure = ShortcutFailure{error && error->error_code == XCB_ACCESS ? ShortcutError::Conflict : ShortcutError::Unavailable,
                    error ? error->error_code : xcb_connection_has_error(connection_)};
                const auto released = releaseExcept(acquired, {});
                if (released) { closeX11(); return released; }
                return failure;
            }
            acquired.push_back(grab);
        }
        xcb_flush(connection_); output = std::move(candidate); return {};
    }
    ShortcutFailure releaseExcept(const std::vector<Grab>& grabs, const std::vector<Grab>& retained)
    {
        if (!connection_) return grabs.empty() ? ShortcutFailure{} : ShortcutFailure{ShortcutError::Unavailable};
        std::vector<xcb_void_cookie_t> requests;
        for (const auto& grab : grabs) if (std::find(retained.begin(), retained.end(), grab) == retained.end())
            requests.push_back(xcb_ungrab_key_checked(connection_, grab.key, grab.root, grab.modifiers));
        ShortcutFailure failure;
        for (const auto request : requests) {
            CPointer<xcb_generic_error_t> error(xcb_request_check(connection_, request));
            if (error && !failure) failure = {ShortcutError::Unavailable, error->error_code};
        }
        if (xcb_connection_has_error(connection_)) failure = {ShortcutError::Unavailable, xcb_connection_has_error(connection_)};
        return failure;
    }
    void closeX11()
    {
        notifier_.reset();
        if (symbols_) xcb_key_symbols_free(std::exchange(symbols_, nullptr));
        if (connection_) xcb_disconnect(std::exchange(connection_, nullptr));
        active_.clear(); candidate_.clear(); state_.registered = false;
        if (!portal_) state_.editing = ShortcutEditing::Unavailable;
    }
    void drainStaleKeys()
    {
        heldKey_ = 0; lastReleaseTime_ = 0;
        ++keyGeneration_;
        qCDebug(shortcutTrace) << "discard superseded input" << "generation" << keyGeneration_;
        // Poll through this client's already queued events, retaining mapping
        // notifications. New activations are delivered after registration.
        drainEvents(true);
    }
    void drainEvents(bool discardKeys = false)
    {
        if (!connection_ || stopped_) return;
        while (auto* value = xcb_poll_for_event(connection_)) {
            CPointer<xcb_generic_event_t> event(value);
            if (event->response_type & 0x80) { qCDebug(shortcutTrace) << "ignored SendEvent" << (event->response_type & 0x7f); continue; }
            const auto type = event->response_type & 0x7f;
            if (type == XCB_MAPPING_NOTIFY) {
                auto* mapping = reinterpret_cast<xcb_mapping_notify_event_t*>(event.get());
                qCDebug(shortcutTrace) << "mapping notification" << mapping->request << mapping->first_keycode << mapping->count;
                if (mapping->request == XCB_MAPPING_KEYBOARD || mapping->request == XCB_MAPPING_MODIFIER) {
                    xcb_refresh_keyboard_mapping(symbols_, mapping);
                    scheduleRemap();
                }
                continue;
            }
            if (discardKeys || state_.suspended || !state_.registered) {
                if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE)
                    qCDebug(shortcutTrace) << "input discarded" << type << discardKeys << state_.suspended << state_.registered;
                continue;
            }
            if (type == XCB_KEY_RELEASE) {
                const auto* key = reinterpret_cast<const xcb_key_release_event_t*>(event.get());
                if (heldKey_ == key->detail) heldKey_ = 0;
                lastReleaseTime_ = key->time; lastReleaseKey_ = key->detail;
            } else if (type == XCB_KEY_PRESS) {
                const auto* key = reinterpret_cast<const xcb_key_press_event_t*>(event.get());
                const Grab pressed{key->root, key->detail, static_cast<uint16_t>(key->state & 0xff)};
                const bool matches = std::find(active_.begin(), active_.end(), pressed) != active_.end();
                // Only the configured grab's code is logged. Other keys that
                // arrive while a passive grab is held are never transcribed.
                qCDebug(shortcutTrace) << "key press" << "configured" << matches << "root" << key->root
                    << "modifiers" << key->state << "held" << (heldKey_ == key->detail)
                    << "repeat" << (lastReleaseKey_ == key->detail && lastReleaseTime_ == key->time)
                    << "generation" << keyGeneration_;
                if (!matches || heldKey_ == key->detail) continue;
                heldKey_ = key->detail;
                if (lastReleaseKey_ == key->detail && lastReleaseTime_ == key->time) continue;
                const auto generation = keyGeneration_;
                QTimer::singleShot(0, this, [this, generation, pressed] {
                    const bool stillBound = std::find(active_.begin(), active_.end(), pressed) != active_.end();
                    const bool deliver = !stopped_ && state_.registered && !state_.suspended && generation == keyGeneration_ && stillBound;
                    qCDebug(shortcutTrace) << "queued activation" << "deliver" << deliver << "still bound" << stillBound
                        << "generation" << generation << keyGeneration_;
                    if (deliver) Q_EMIT owner_.activated({});
                });
            }
        }
        if (xcb_connection_has_error(connection_)) {
            if (notifier_) notifier_->setEnabled(false);
            state_.registered = false; state_.editing = ShortcutEditing::Unavailable;
            state_.failure = {ShortcutError::Unavailable, xcb_connection_has_error(connection_)};
            Q_EMIT owner_.changed();
        }
    }
    void scheduleRemap()
    {
        if (remapQueued_) return;
        remapQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            remapQueued_ = false;
            if (stopped_ || state_.suspended || state_.requestedKeys.isEmpty() || pending_) return;
            std::vector<Grab> candidate;
            auto failure = acquire(state_.requestedKeys, candidate, active_);
            // Device switches can update unrelated keycodes/lock variants too.
            // Judge queued input by its exact grab, not by equality of the whole
            // map: an added variant cannot invalidate an already valid press.
            qCDebug(shortcutTrace) << "mapping recheck" << "before" << describe(active_)
                << "after" << describe(candidate) << "failure" << static_cast<int>(failure.code);
            if (!failure) {
                failure = releaseExcept(active_, candidate);
                if (failure) closeX11();
                else { active_ = std::move(candidate); state_.registered = true; }
            } else {
                // Old keycodes no longer prove the requested shortcut. Stop
                // dispatching rather than label a changed physical key valid.
                if (releaseExcept(active_, {})) closeX11();
                active_.clear(); state_.registered = false;
                failure.code = ShortcutError::RestoreFailed;
            }
            state_.failure = failure;
            if (!std::any_of(active_.begin(), active_.end(), [this](const Grab& grab) { return grab.key == heldKey_; })) {
                heldKey_ = 0; lastReleaseTime_ = 0;
            }
            const QPointer<LinuxShortcut> life(this);
            // Checked replies may buffer events in XCB. Process them against
            // the new real grab set, without throwing away still-valid input.
            drainEvents();
            if (life) Q_EMIT owner_.changed();
        });
    }
    ShortcutSnapshot state_;
    std::unique_ptr<Linux::PortalShortcuts> portal_;
    xcb_connection_t* connection_{};
    xcb_key_symbols_t* symbols_{};
    std::unique_ptr<QSocketNotifier> notifier_;
    std::vector<xcb_window_t> roots_;
    std::vector<Grab> active_, candidate_;
    QStringList pendingKeys_;
    quint64 pending_{}, portalRequest_{}, keyGeneration_{};
    xcb_timestamp_t lastReleaseTime_{};
    xcb_keycode_t heldKey_{}, lastReleaseKey_{};
    bool stopped_{}, initializing_{}, cancelled_{}, commitRejected_{}, remapQueued_{};
};
}

std::unique_ptr<GlobalShortcutBackend> createGlobalShortcutBackend(Desktop::GlobalShortcut& owner)
{ return std::make_unique<LinuxShortcut>(owner); }

} // namespace HuhuCapture::Platform
