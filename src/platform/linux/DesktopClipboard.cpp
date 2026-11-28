#include "platform/linux/DesktopClipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <QtWaylandClient/QWaylandClientExtension>
#include <wayland-client.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace HuhuCapture::Platform::Linux {
namespace {
class DataDeviceManager final : public QWaylandClientExtension {
public:
    DataDeviceManager() : QWaylandClientExtension(3) { initialize(); }
    ~DataDeviceManager() override { release(); }
    const wl_interface* extensionInterface() const override { return &wl_data_device_manager_interface; }
    void bind(wl_registry* registry, int id, int advertised) override
    {
        release(); setVersion((std::min)({3, advertised, wl_data_device_manager_interface.version}));
        value = static_cast<wl_data_device_manager*>(wl_registry_bind(registry, static_cast<std::uint32_t>(id),
            &wl_data_device_manager_interface, static_cast<std::uint32_t>(version())));
    }
    void release() { if (value) wl_data_device_manager_destroy(std::exchange(value, nullptr)); }
    wl_data_device_manager* value{};
};

Desktop::Failure unavailable(const QString& detail) { return {Desktop::Error::Unavailable, 0, detail}; }
}

struct DesktopClipboard::State {
    struct Offer {
        State* state;
        wl_data_offer* value;
        bool matches{};
        ~Offer() { if (value) wl_data_offer_destroy(value); }
    };
    DesktopClipboard* owner;
    std::unique_ptr<DataDeviceManager> manager;
    std::vector<std::unique_ptr<Offer>> offers;
    wl_data_device* device{};
    wl_seat* seat{}; // borrowed from Qt, never destroyed here
    QPointer<QMimeData> mime;
    QPointer<QWindow> origin;
    QTimer timer;
    QByteArray marker;
    quint64 token{};
    bool wayland{}, xcb{};

    explicit State(DesktopClipboard* parent) : owner(parent)
    {
        const auto platform = QGuiApplication::platformName();
        wayland = platform.startsWith(QStringLiteral("wayland")); xcb = platform == QStringLiteral("xcb");
        timer.setSingleShot(true); timer.setInterval(3000);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] {
            complete({Desktop::Error::Timeout, 0, QStringLiteral("The desktop did not confirm this clipboard publication")});
        });
        if (wayland) {
            manager = std::make_unique<DataDeviceManager>();
            QObject::connect(manager.get(), &QWaylandClientExtension::activeChanged, owner, [this] {
                if (!manager->isActive()) {
                    releaseDevice(); manager->release();
                    complete(unavailable(QStringLiteral("The Wayland data device manager disappeared")));
                }
            });
        }
    }
    ~State()
    {
        timer.stop();
        if (manager) QObject::disconnect(manager.get(), nullptr, owner, nullptr);
        releaseDevice();
    }
    void releaseDevice()
    {
        offers.clear();
        if (device) {
            if (wl_data_device_get_version(device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION) wl_data_device_release(device);
            else wl_data_device_destroy(device);
            device = nullptr;
        }
        seat = nullptr;
    }
    void complete(Desktop::Failure failure)
    {
        if (!token) return;
        const auto finished = token;
        // A result may destroy this service. Never expose reentrant application
        // callbacks from inside a libwayland listener or while freeing offers.
        QTimer::singleShot(0, owner, [receiver = owner, finished, failure = std::move(failure)] {
            Q_EMIT receiver->verified(finished, failure);
        });
        // Queue first: if allocating that callback fails, the existing deadline
        // and token stay live instead of silently dropping the only result.
        token = 0; timer.stop(); mime.clear(); origin.clear(); marker.clear();
    }
    bool ownsCandidate() const
    {
        auto* clipboard = QGuiApplication::clipboard();
        return mime && clipboard && clipboard->ownsClipboard() && clipboard->mimeData() == mime.data();
    }
    void discard(wl_data_offer* value)
    { std::erase_if(offers, [value](const auto& item) { return item->value == value; }); }

    static void offered(void* data, wl_data_offer*, const char* mimeType) noexcept
    {
        auto& offer = *static_cast<Offer*>(data);
        if (offer.state->token && mimeType && std::strcmp(mimeType, offer.state->marker.constData()) == 0) offer.matches = true;
    }
    static void sourceActions(void*, wl_data_offer*, std::uint32_t) noexcept {}
    static void action(void*, wl_data_offer*, std::uint32_t) noexcept {}
    static constexpr wl_data_offer_listener offerListener{offered, sourceActions, action};
    static void newOffer(void* data, wl_data_device*, wl_data_offer* value) noexcept
    {
        auto& self = *static_cast<State*>(data);
        // Take unique ownership before any potentially allocating container or
        // error reporting operation. A failed push must destroy the proxy once.
        std::unique_ptr<Offer> offer(new (std::nothrow) Offer{&self, value, false});
        if (!offer) { wl_data_offer_destroy(value); return; }
        try {
            if (self.offers.size() >= 16) {
                self.complete(unavailable(QStringLiteral("The compositor exceeded the bounded clipboard offer queue")));
                return;
            }
            if (wl_data_offer_add_listener(value, &offerListener, offer.get()) < 0) {
                self.complete(unavailable(QStringLiteral("Cannot observe the Wayland clipboard offer"))); return;
            }
            self.offers.push_back(std::move(offer));
        } catch (...) {
            // Callback allocations are small and bounded; never unwind through
            // the C listener. The existing deadline produces an explicit error.
            // The scoped offer releases an unregistered proxy without reading
            // pixels. No manual destroy duplicates its RAII cleanup.
        }
    }
    static void entered(void* data, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* offer) noexcept
    { if (offer) static_cast<State*>(data)->discard(offer); }
    static void left(void*, wl_data_device*) noexcept {}
    static void motion(void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) noexcept {}
    static void dropped(void*, wl_data_device*) noexcept {}
    static void selection(void* data, wl_data_device*, wl_data_offer* value) noexcept
    {
        auto& self = *static_cast<State*>(data);
        try {
            const auto found = std::find_if(self.offers.begin(), self.offers.end(), [value](const auto& offer) { return offer->value == value; });
            const bool match = found != self.offers.end() && (*found)->matches;
            // Selection offers are only receipts. Qt's independent data device
            // owns content transfer, so this observer never requests a pipe.
            self.offers.clear();
            if (match && self.token && self.ownsCandidate()) self.complete({});
        } catch (...) { /* Do not cross the C callback; the bounded timer remains armed. */ }
    }
    static constexpr wl_data_device_listener deviceListener{newOffer, entered, left, motion, dropped, selection};
};

DesktopClipboard::DesktopClipboard(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
DesktopClipboard::~DesktopClipboard() = default;

Desktop::Failure DesktopClipboard::begin(quint64 token, QWindow* origin, QMimeData* mime, const QByteArray& marker)
{
    cancel();
    if (!token || !mime || marker.isEmpty()) return {Desktop::Error::InvalidArgument, 0, QStringLiteral("Invalid clipboard publication")};
    if (!state_->wayland && !state_->xcb) return unavailable(QStringLiteral("This QPA backend has no supported system clipboard"));
    if (state_->wayland) {
        auto* native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        auto* focused = QGuiApplication::focusWindow();
        while (focused && focused != origin) focused = focused->transientParent();
        if (!origin || !origin->isVisible() || !focused || !native || !native->lastInputSerial() || !native->lastInputSeat())
            return {Desktop::Error::RequiresInteraction, 0, QStringLiteral("Copy again from the focused application window")};
        if (!state_->manager || !state_->manager->isActive() || !state_->manager->value)
            return unavailable(QStringLiteral("The compositor has not provided a clipboard data device"));
        if (state_->seat != native->lastInputSeat()) {
            state_->releaseDevice(); state_->seat = native->lastInputSeat();
            state_->device = wl_data_device_manager_get_data_device(state_->manager->value, state_->seat);
            if (!state_->device || wl_data_device_add_listener(state_->device, &State::deviceListener, state_.get()) < 0) {
                state_->releaseDevice(); return unavailable(QStringLiteral("Cannot observe this Wayland seat's clipboard"));
            }
        }
    }
    state_->token = token; state_->mime = mime; state_->origin = origin; state_->marker = marker;
    state_->timer.start(); return {};
}

void DesktopClipboard::submitted()
{
    if (!state_->token) return;
    if (!state_->ownsCandidate()) {
        state_->complete({Desktop::Error::ClipboardRejected, 0, QStringLiteral("Qt did not obtain clipboard ownership")}); return;
    }
    if (state_->xcb) {
        // Fixed Qt's X11 mimeData() compares the real X selection owner before
        // returning our object. ownsClipboard() alone only tracks a timestamp.
        state_->complete({});
    }
    // Wayland ownsClipboard()/mimeData() only describe local state. Completion
    // must wait for this marker in an actual compositor selection event.
}

void DesktopClipboard::cancel()
{
    state_->timer.stop(); state_->token = 0; state_->mime.clear(); state_->origin.clear(); state_->marker.clear();
}

} // namespace HuhuCapture::Platform::Linux
