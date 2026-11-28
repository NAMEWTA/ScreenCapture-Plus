#include "app/LanController.h"

#include "services/transfer/IdentityStore.h"
#include "services/transfer/ReceiveArchiveSink.h"
#include "ui/qt/ReceivePrompt.h"
#include "ui/qt/TransferPresentation.h"
#include "ui/qt/TransferWindow.h"

#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkInterface>
#include <QPointer>
#include <QSysInfo>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <functional>
#include <optional>
#include <new>
#include <stop_token>
#include <tuple>
#include <utility>

namespace HuhuCapture::Application {
namespace {
namespace ui = UI::Qt;
namespace presentation = UI::Qt::TransferPresentation;

struct InterfaceChoice {
    QString name;
    QString hardwareAddress;
    QStringList addresses;
};

struct InterfaceAddress {
    QString name;
    QString displayName;
    QString hardwareAddress;
    QString address;
    bool available{};
    bool virtualNetwork{};
};

struct Preferences {
    bool enabled{};
    bool discovery{true};
    QString alias;
    quint16 port{53317};
    QVector<InterfaceChoice> interfaces;
};

bool boundedText(const QString& value, qsizetype maximum)
{ return !value.isEmpty() && !value.contains(QChar::Null) && value.toUtf8().size() <= maximum; }

bool concreteIPv4(const QString& value)
{
    const QHostAddress address(value);
    return address.protocol() == QAbstractSocket::IPv4Protocol && !address.isMulticast() &&
        address.toIPv4Address() != 0 && address.toIPv4Address() != 0xffffffffu && address.toString() == value;
}

QVector<InterfaceAddress> interfaceAddresses()
{
    QVector<InterfaceAddress> result;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& interface : interfaces) {
        const auto flags = interface.flags();
        const bool available = flags.testFlag(QNetworkInterface::IsUp) && flags.testFlag(QNetworkInterface::IsRunning);
        const bool virtualNetwork = flags.testFlag(QNetworkInterface::IsPointToPoint) ||
            interface.type() == QNetworkInterface::Virtual || interface.type() == QNetworkInterface::Ppp ||
            interface.type() == QNetworkInterface::Slip;
        for (const auto& entry : interface.addressEntries()) {
            const auto address = entry.ip().toString();
            if (!concreteIPv4(address)) continue;
            // Unknown adapters, loopback and VPNs stay visible, but no adapter
            // is selected by default. A name heuristic never grants access.
            result.append({interface.name(), interface.humanReadableName(), interface.hardwareAddress(),
                address, available, virtualNetwork});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.name, left.address) < std::tie(right.name, right.address);
    });
    for (auto& entry : result) {
        const auto occurrences = std::count_if(result.cbegin(), result.cend(), [&](const auto& other) {
            return other.address == entry.address;
        });
        // The native contract binds concrete addresses, not interface indices.
        // Ambiguous overlapping interfaces cannot claim selected-NIC isolation.
        if (occurrences != 1) entry.available = false;
    }
    return result;
}

bool matches(const InterfaceChoice& choice, const InterfaceAddress& interface)
{
    return choice.name == interface.name && (choice.hardwareAddress.isEmpty() ||
        choice.hardwareAddress.compare(interface.hardwareAddress, ::Qt::CaseInsensitive) == 0);
}

QStringList selectedAddresses(const Preferences& preferences, const QVector<InterfaceAddress>& interfaces)
{
    QStringList result;
    for (const auto& choice : preferences.interfaces) {
        QStringList available, exact;
        for (const auto& interface : interfaces) {
            if (!interface.available || !matches(choice, interface)) continue;
            available.append(interface.address);
            if (choice.addresses.contains(interface.address)) exact.append(interface.address);
        }
        // A selected NIC keeps its identity when DHCP changes its addresses.
        // If a selected alias is still present, do not broaden to other aliases.
        result.append(exact.isEmpty() ? available : exact);
    }
    result.removeDuplicates(); result.sort();
    return result;
}

bool parsePreferences(const QJsonValue& value, Preferences& result)
{
    result.alias = QSysInfo::machineHostName();
    if (!boundedText(result.alias, 256)) result.alias = QStringLiteral("huhucapture");
    if (value.isUndefined()) return true;
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    const auto schema = object.value(QStringLiteral("schema"));
    if (!schema.isUndefined() && (!schema.isDouble() || schema.toDouble() != 1)) return false;
    for (const auto& key : {QStringLiteral("enabled"), QStringLiteral("discovery")})
        if (object.contains(key) && !object.value(key).isBool()) return false;
    result.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    result.discovery = object.value(QStringLiteral("discovery")).toBool(true);
    if (object.contains(QStringLiteral("alias"))) result.alias = object.value(QStringLiteral("alias")).toString();
    if (!boundedText(result.alias, 256)) return false;
    if (object.contains(QStringLiteral("port"))) {
        const auto port = object.value(QStringLiteral("port"));
        const auto number = port.toInt();
        if (number < 1 || number > 65535 || port.toDouble() != number) return false;
        result.port = static_cast<quint16>(number);
    }
    const auto selected = object.value(QStringLiteral("interfaces"));
    if (!selected.isUndefined() && !selected.isArray()) return false;
    if (selected.toArray().size() > 16) return false;
    QStringList names;
    for (const auto& entry : selected.toArray()) {
        if (!entry.isObject()) return false;
        const auto interface = entry.toObject();
        InterfaceChoice choice{interface.value(QStringLiteral("name")).toString(),
            interface.value(QStringLiteral("hardwareAddress")).toString(), {}};
        if (!boundedText(choice.name, 256) || choice.hardwareAddress.size() > 256 || names.contains(choice.name) ||
            !interface.value(QStringLiteral("addresses")).isArray()) return false;
        names.append(choice.name);
        const auto addresses = interface.value(QStringLiteral("addresses")).toArray();
        if (addresses.isEmpty() || addresses.size() > 16) return false;
        for (const auto& address : addresses) {
            const auto text = address.toString();
            if (!concreteIPv4(text) || choice.addresses.contains(text)) return false;
            choice.addresses.append(text);
        }
        result.interfaces.append(std::move(choice));
    }
    return true;
}

QJsonObject preferenceDocument(const Preferences& preferences, QJsonObject prior)
{
    prior.insert(QStringLiteral("schema"), 1);
    prior.insert(QStringLiteral("enabled"), preferences.enabled);
    prior.insert(QStringLiteral("discovery"), preferences.discovery);
    prior.insert(QStringLiteral("alias"), preferences.alias);
    prior.insert(QStringLiteral("port"), preferences.port);
    QJsonArray interfaces;
    const auto previousInterfaces = prior.value(QStringLiteral("interfaces")).toArray();
    for (const auto& choice : preferences.interfaces) {
        QJsonObject value;
        for (const auto& previous : previousInterfaces) {
            const auto old = previous.toObject();
            if (old.value(QStringLiteral("name")).toString() == choice.name &&
                old.value(QStringLiteral("hardwareAddress")).toString() == choice.hardwareAddress) { value = old; break; }
        }
        value.insert(QStringLiteral("name"), choice.name);
        value.insert(QStringLiteral("hardwareAddress"), choice.hardwareAddress);
        value.insert(QStringLiteral("addresses"), QJsonArray::fromStringList(choice.addresses));
        interfaces.append(value);
    }
    prior.insert(QStringLiteral("interfaces"), interfaces);
    return prior;
}

int mediaStatus(Media::Error error)
{
    switch (error) {
    case Media::Error::None: return HUHU_OK;
    case Media::Error::Cancelled: return HUHU_CANCELLED;
    case Media::Error::Busy: return HUHU_BUSY;
    case Media::Error::Budget: return HUHU_LIMIT_EXCEEDED;
    case Media::Error::InvalidMedia: case Media::Error::Unsupported: return HUHU_INVALID_MEDIA;
    case Media::Error::ChangedOnDisk: case Media::Error::InvalidManagedFile: return HUHU_INVALID_STATE;
    case Media::Error::DurabilityUnknown: return HUHU_DURABILITY_UNKNOWN;
    default: return HUHU_IO;
    }
}

std::uint32_t nativeKind(Media::Kind kind)
{
    switch (kind) { case Media::Kind::Png: return HUHU_PNG; case Media::Kind::Gif: return HUHU_GIF;
        case Media::Kind::Mp4: return HUHU_MP4; }
    return 0;
}

QStringView identityErrorKey(Transfer::IdentityError error)
{
    switch (error) {
    case Transfer::IdentityError::Cancelled: return u"lan.cancelled";
    case Transfer::IdentityError::Permissions: return u"lan.identityPermissions";
    case Transfer::IdentityError::InvalidIdentity: return u"lan.identityDamaged";
    case Transfer::IdentityError::InvalidTrust: return u"lan.trustDamaged";
    case Transfer::IdentityError::ChangedOnDisk: return u"lan.identityChangedOnDisk";
    case Transfer::IdentityError::DurabilityUnknown: return u"lan.identityDurability";
    default: return u"lan.identityStorageFailure";
    }
}

struct WorkResult {
    int status{HUHU_OK};
    Transfer::IdentityResult identity;
    std::vector<std::shared_ptr<const Transfer::PreparedMedia>> media;
};
} // namespace

struct LanController::State {
    LanController* owner;
    std::shared_ptr<Media::MediaArchiveService> archive;
    Config::SettingsStore& settings;
    std::shared_ptr<Transfer::IdentityStore> identityStore;
    Transfer::TransferService transfers;
    ui::DesktopTranslator translator;
    Preferences preferences;
    QVector<InterfaceAddress> interfaces;
    Transfer::IdentityResult identity;
    QThreadPool pool;
    std::stop_source cancellation;
    bool workPending{}, initialized{}, validConfig{}, stopping{}, finished{}, identityFailed{};
    bool needsStart{}, announcedAvailable{};
    int fallbackAttempts{};
    quint16 requestedPort{53317}, listeningPort{};
    QStringList boundAddresses;
    QStringView failureKey;
    int failureStatus{};
    QTimer interfacesTimer, discoveryTimer;
    Transfer::RequestId refreshRequest{};
    QPointer<ui::TransferWindow> transferWindow;
    QPointer<ui::TransferSettingsPanel> settingsWindow;
    QVector<QPointer<QWidget>> views;
    QMap<QString, QPointer<ui::ReceivePrompt>> prompts;
    struct SettingsCompletion { QPointer<ui::TransferSettingsPanel> view; quint64 token{}; } applying;

    State(LanController* q, std::shared_ptr<Media::MediaArchiveService> media,
        Config::SettingsStore& config, QString privateDirectory)
        : owner(q), archive(std::move(media)), settings(config),
          identityStore(std::make_shared<Transfer::IdentityStore>(std::move(privateDirectory)))
    {
        pool.setMaxThreadCount(1); pool.setExpiryTimeout(30000);
        interfacesTimer.setInterval(5000); discoveryTimer.setInterval(30000);
        QObject::connect(&interfacesTimer, &QTimer::timeout, owner, [this] { refreshInterfaces(); });
        QObject::connect(&discoveryTimer, &QTimer::timeout, owner, [this] { refreshDiscovery(); });
        QObject::connect(&transfers, &Transfer::TransferService::eventReady, owner,
            [this](const Transfer::Event& event) { consume(event); });
    }

    QString text(QStringView key) const { return translator ? translator(key) : key.toString(); }
    QString failure() const
    {
        if (!failureKey.isEmpty()) return text(failureKey);
        return failureStatus ? presentation::status(translator, failureStatus) : QString();
    }
    bool available() const { return !stopping && transfers.state() == Transfer::ServiceState::Enabled; }
    bool busy() const
    { return workPending || applying.token || transfers.state() == Transfer::ServiceState::Starting ||
        transfers.state() == Transfer::ServiceState::Stopping; }

    ui::LanSettingsSnapshot snapshot() const
    {
        ui::LanSettingsSnapshot result;
        result.enabled = preferences.enabled; result.discovery = preferences.discovery;
        result.canConfigure = initialized && validConfig && !stopping && !applying.token;
        result.alias = preferences.alias; result.port = preferences.port;
        result.fingerprint = identity.fingerprint;
        result.archiveDirectory = archive ? archive->directory() : QString();
        result.unavailableReason = failure();
        result.listeningPort = listeningPort; result.listeningAddresses = boundAddresses;
        const auto selected = selectedAddresses(preferences, interfaces);
        for (const auto& interface : interfaces)
            result.interfaces.append({interface.displayName.isEmpty() ? interface.name : interface.displayName,
                interface.address, interface.available, selected.contains(interface.address), interface.virtualNetwork});
        // Preserve an unavailable selected NIC visibly. Choosing another NIC
        // requires an explicit settings action; hotplug never means wildcard.
        for (const auto& choice : preferences.interfaces) {
            const bool present = std::any_of(interfaces.cbegin(), interfaces.cend(), [&](const auto& interface) {
                return interface.available && matches(choice, interface);
            });
            if (present) continue;
            for (const auto& address : choice.addresses) {
                auto found = std::find_if(result.interfaces.begin(), result.interfaces.end(), [&](const auto& interface) {
                    return interface.address == address;
                });
                if (found != result.interfaces.end()) found->selected = true;
                else result.interfaces.append({choice.name, address, false, true, false});
            }
        }
        return result;
    }

    void retainView(QWidget* view)
    {
        views.erase(std::remove_if(views.begin(), views.end(), [](const auto& value) { return value.isNull(); }), views.end());
        views.append(view);
    }

    void publish()
    {
        const QPointer<LanController> lifetime(owner);
        if (settingsWindow) settingsWindow->setSnapshot(snapshot());
        if (!lifetime) return;
        const bool active = available();
        if (announcedAvailable != active) {
            announcedAvailable = active; Q_EMIT owner->availabilityChanged(active);
            if (!lifetime) return;
        }
        Q_EMIT owner->busyChanged(busy());
    }

    void finishSettings(ui::DesktopResult result, QString message = {})
    {
        auto waiting = std::exchange(applying, {});
        if (waiting.view) waiting.view->completeChange(waiting.token, result, snapshot(), message);
        publish();
    }

    bool submit(std::function<WorkResult(std::stop_token)> work, std::function<void(WorkResult)> done)
    {
        if (stopping || workPending) return false;
        cancellation = std::stop_source{};
        const auto cancel = cancellation;
        auto* watcher = new QFutureWatcher<WorkResult>(owner);
        workPending = true;
        QObject::connect(watcher, &QFutureWatcher<WorkResult>::finished, owner,
            [this, watcher, done = std::move(done)]() mutable {
                auto result = watcher->result(); watcher->deleteLater(); workPending = false;
                if (stopping) { finishShutdown(); return; }
                const QPointer<LanController> lifetime(owner);
                done(std::move(result)); if (lifetime) publish();
            });
        try {
            watcher->setFuture(QtConcurrent::run(&pool, [work = std::move(work), cancel]() mutable {
                try { return work(cancel.get_token()); }
                catch (const std::bad_alloc&) { WorkResult result; result.status = HUHU_LIMIT_EXCEEDED; return result; }
                catch (...) { WorkResult result; result.status = HUHU_IO; return result; }
            }));
        } catch (...) {
            workPending = false; delete watcher; return false;
        }
        publish(); return true;
    }

    void reportFailure(QStringView key = {}, int status = 0)
    {
        const bool changed = failureKey != key || failureStatus != status;
        failureKey = key; failureStatus = status;
        const auto message = failure();
        const QPointer<LanController> lifetime(owner);
        if (applying.token) finishSettings(ui::DesktopResult::Failed, message);
        else publish();
        if (lifetime && changed && !message.isEmpty()) Q_EMIT owner->operationFailed(message);
    }

    void maybeStart()
    {
        if (stopping || !needsStart || !preferences.enabled || identityFailed || workPending ||
            transfers.state() != Transfer::ServiceState::Disabled) return;
        const auto selected = selectedAddresses(preferences, interfaces);
        if (selected.isEmpty() || selected.size() > 16) {
            failureKey = u"lan.networkUnavailable";
            if (applying.token) finishSettings(ui::DesktopResult::Failed, failure());
            else publish();
            return;
        }
        if (identity.nativeIdentity.isEmpty()) {
            if (!submit([store = identityStore](std::stop_token stop) {
                WorkResult result; result.identity = store->loadOrCreate(stop); return result;
            }, [this](WorkResult result) {
                if (result.status != HUHU_OK || !result.identity.ok()) {
                    identityFailed = true; needsStart = false;
                    reportFailure(result.identity.ok() ? QStringView{} : identityErrorKey(result.identity.error),
                        result.identity.ok() ? result.status : 0);
                    return;
                }
                identity = std::move(result.identity); maybeStart();
            })) reportFailure({}, HUHU_BUSY);
            return;
        }
        auto configuration = identity.nativeIdentity;
        configuration.insert(QStringLiteral("alias"), preferences.alias);
        configuration.insert(QStringLiteral("addresses"), QJsonArray::fromStringList(selected));
        configuration.insert(QStringLiteral("port"), requestedPort);
        configuration.insert(QStringLiteral("discovery"), preferences.discovery);
        const auto admitted = transfers.enable(QJsonDocument(configuration).toJson(QJsonDocument::Compact),
            std::make_unique<Transfer::ReceiveArchiveSink>(archive));
        if (!admitted) { needsStart = false; reportFailure({}, admitted.status); return; }
        needsStart = false; boundAddresses = selected; publish();
    }

    void reconfigure()
    {
        needsStart = preferences.enabled; requestedPort = preferences.port; fallbackAttempts = 0;
        refreshRequest = 0; discoveryTimer.stop();
        if (preferences.enabled) interfacesTimer.start();
        else if (!settingsWindow || !settingsWindow->isVisible()) interfacesTimer.stop();
        if (transfers.state() == Transfer::ServiceState::Enabled || transfers.state() == Transfer::ServiceState::Starting) {
            (void)transfers.disable(); publish(); return;
        }
        if (!preferences.enabled && transfers.state() == Transfer::ServiceState::Disabled) {
            finishSettings(ui::DesktopResult::Succeeded); return;
        }
        maybeStart();
    }

    void apply(ui::TransferSettingsPanel* view, quint64 token, const ui::LanSettingsChange& change)
    {
        if (stopping || !validConfig || applying.token || (workPending && change.enabled)) {
            view->completeChange(token, ui::DesktopResult::Failed, snapshot(), text(u"lan.busy")); return;
        }
        interfaces = interfaceAddresses();
        Preferences candidate = preferences;
        candidate.enabled = change.enabled; candidate.discovery = change.discovery;
        candidate.alias = change.alias; candidate.port = change.port;
        if (!boundedText(candidate.alias, 256) || !candidate.port || change.addresses.size() > 16) {
            view->completeChange(token, ui::DesktopResult::Failed, snapshot(), text(u"lan.settingsInvalid")); return;
        }
        if (change.enabled) {
            if (change.addresses.isEmpty()) {
                view->completeChange(token, ui::DesktopResult::Failed, snapshot(), text(u"lan.settingsInvalid")); return;
            }
            candidate.interfaces.clear();
            for (const auto& address : change.addresses) {
                const auto found = std::find_if(interfaces.cbegin(), interfaces.cend(), [&](const auto& value) {
                    return value.address == address && value.available;
                });
                if (found == interfaces.cend()) {
                    view->completeChange(token, ui::DesktopResult::Failed, snapshot(), text(u"lan.networkUnavailable")); return;
                }
                auto choice = std::find_if(candidate.interfaces.begin(), candidate.interfaces.end(), [&](const auto& value) {
                    return value.name == found->name;
                });
                if (choice == candidate.interfaces.end()) candidate.interfaces.append({found->name, found->hardwareAddress, {address}});
                else if (!choice->addresses.contains(address)) choice->addresses.append(address);
            }
        }
        const auto saved = settings.setValue({QStringLiteral("lan")}, preferenceDocument(candidate,
            settings.value({QStringLiteral("lan")}).toObject()));
        if (!saved.ok()) {
            // Turning reception off takes effect even if its preference cannot
            // be persisted. The localized error explicitly describes restart.
            if (!change.enabled) {
                preferences.enabled = false; needsStart = false; discoveryTimer.stop();
                cancellation.request_stop();
                (void)transfers.disable();
            }
            failureKey = change.enabled ? u"lan.settingsSaveFailure" : u"lan.disableSaveFailure";
            const auto message = failure();
            const QPointer<LanController> lifetime(owner);
            view->completeChange(token, ui::DesktopResult::Failed, snapshot(), message);
            if (!lifetime) return;
            publish();
            if (lifetime) Q_EMIT owner->operationFailed(message);
            return;
        }
        preferences = std::move(candidate); applying = {view, token};
        if (!preferences.enabled) cancellation.request_stop();
        failureKey = {}; failureStatus = 0; identityFailed = false;
        reconfigure();
    }

    void refreshInterfaces()
    {
        if (stopping) return;
        interfaces = interfaceAddresses();
        if (settingsWindow) settingsWindow->setSnapshot(snapshot());
        if (!preferences.enabled || !validConfig || identityFailed || applying.token || workPending) return;
        const auto selected = selectedAddresses(preferences, interfaces);
        if (available() && selected != boundAddresses) {
            failureKey = u"lan.networkChanged";
            reconfigure(); return;
        }
        if (transfers.state() == Transfer::ServiceState::Disabled && needsStart) maybeStart();
    }

    void refreshDiscovery()
    {
        if (!available() || !preferences.discovery || refreshRequest) return;
        const auto result = transfers.refreshDiscovery();
        if (result) refreshRequest = result.request;
        else if (result.status != HUHU_BUSY && result.status != HUHU_INVALID_STATE && result.status != HUHU_CLOSED)
            reportFailure({}, result.status);
    }

    QString expectedFingerprint(const QJsonObject& peer) const
    {
        const auto actual = peer.value(QStringLiteral("fingerprint")).toString().toUpper();
        for (const auto& value : identity.trustedPeers) {
            const auto saved = value.toObject();
            if (saved.value(QStringLiteral("fingerprint")).toString().compare(actual, ::Qt::CaseInsensitive) == 0)
                return actual; // A known certificate at a new IP is the same device.
        }
        for (qsizetype index = identity.trustedPeers.size(); index > 0; --index) {
            const auto saved = identity.trustedPeers[index - 1].toObject();
            for (const auto& endpoint : saved.value(QStringLiteral("endpoints")).toArray()) {
                const auto value = endpoint.toObject();
                if (value.value(QStringLiteral("address")) == peer.value(QStringLiteral("address")) &&
                    value.value(QStringLiteral("port")) == peer.value(QStringLiteral("port")))
                    return saved.value(QStringLiteral("fingerprint")).toString();
            }
        }
        return {};
    }

    void prepare(QPointer<ui::TransferWindow> view, quint64 token, QJsonObject peer,
        QString alias, QVector<Media::Entry> entries)
    {
        if (!available() || identity.nativeIdentity.isEmpty() || entries.isEmpty() || entries.size() > 64 ||
            !concreteIPv4(peer.value(QStringLiteral("address")).toString()) ||
            !presentation::fingerprint(peer.value(QStringLiteral("fingerprint")).toString())) {
            if (view) view->completePreparation(token, HUHU_INVALID_STATE, 0); return;
        }
        std::size_t metadataBytes{};
        for (const auto& entry : entries) {
            const auto amount = static_cast<std::size_t>(entry.path.size() + entry.name.size()) * sizeof(QChar) + sizeof(Media::Entry);
            if (amount > Transfer::TransferLimits::maximumViewSourceBytes - metadataBytes) {
                if (view) view->completePreparation(token, HUHU_LIMIT_EXCEEDED, 0); return;
            }
            metadataBytes += amount;
        }
        const auto snapshot = transfers.viewSnapshot();
        if (!snapshot || !snapshot->service) {
            if (view) view->completePreparation(token, HUHU_INVALID_STATE, 0); return;
        }
        const auto capturedGeneration = snapshot->service->generation;
        // User confirmation already happened in the view. Values, not its
        // lifetime, own this intent. No worker captures or dereferences QWidget.
        const auto submitted = submit([archive = archive, store = identityStore, peer, alias, entries](std::stop_token stop) {
            WorkResult result;
            result.identity = store->rememberPeer(peer.value(QStringLiteral("address")).toString(),
                static_cast<quint16>(peer.value(QStringLiteral("port")).toInt()),
                peer.value(QStringLiteral("fingerprint")).toString(), alias, stop);
            if (!result.identity.ok()) { result.status = HUHU_IO; return result; }
            result.media.reserve(static_cast<std::size_t>(entries.size()));
            for (const auto& entry : entries) {
                if (stop.stop_requested()) { result.status = HUHU_CANCELLED; result.media.clear(); return result; }
                const auto validated = archive->validatedLease(entry, stop);
                if (validated.failure) { result.status = mediaStatus(validated.failure.code); result.media.clear(); return result; }
                const auto opened = Transfer::PreparedMedia::open(entry.path, entry.snapshot,
                    nativeKind(entry.kind), validated.lease);
                if (opened.status != HUHU_OK) { result.status = opened.status; result.media.clear(); return result; }
                if (!validated.lease->unchanged()) { result.status = HUHU_INVALID_STATE; result.media.clear(); return result; }
                result.media.push_back(opened.media);
            }
            return result;
        }, [this, view, token, peer, alias, capturedGeneration](WorkResult result) mutable {
            const QPointer<LanController> lifetime(owner);
            if (result.identity.ok() && !result.identity.nativeIdentity.isEmpty()) identity = std::move(result.identity);
            else if (!result.identity.ok()) reportFailure(identityErrorKey(result.identity.error));
            if (!lifetime) return;
            auto status = result.status;
            const auto current = transfers.viewSnapshot();
            if (!available() || !current || !current->service || current->service->generation != capturedGeneration)
                status = HUHU_CANCELLED;
            const auto submission = status == HUHU_OK ? transfers.send(QJsonDocument(peer).toJson(QJsonDocument::Compact),
                std::move(result.media), alias) : Transfer::Submission{status, 0};
            peer = {}; // Includes an optional PIN; never retained in app history.
            result.media.clear(); // Admission transfers the only extra leases to the service.
            if (view) view->completePreparation(token, submission.status, submission.request);
            if (lifetime && !submission) Q_EMIT owner->operationFailed(presentation::status(translator, submission.status));
        });
        if (!submitted && view) view->completePreparation(token, HUHU_BUSY, 0);
    }

    void showPrompt(const Transfer::Event& offer)
    {
        if (stopping || offer.kind != Transfer::EventKind::ReceiveOffer) return;
        const auto session = offer.data.value(QStringLiteral("sessionId")).toString();
        if (session.isEmpty()) return;
        const auto key = QString::number(static_cast<qulonglong>(offer.generation)) + u'/' + session;
        if (auto existing = prompts.value(key)) {
            existing->show(); existing->raise(); existing->activateWindow(); return;
        }
        const auto current = transfers.viewSnapshot();
        if (!current || !current->service || offer.generation != current->service->generation || !available()) return;
        auto* prompt = new ui::ReceivePrompt(transfers, offer, archive->directory());
        prompt->setAttribute(::Qt::WA_DeleteOnClose); prompt->setTranslator(translator);
        prompts.insert(key, prompt);
        retainView(prompt);
        QObject::connect(prompt, &QObject::destroyed, owner, [this, key] { prompts.remove(key); });
        prompt->show(); prompt->raise(); prompt->activateWindow();
    }

    void consume(const Transfer::Event& event)
    {
        const QPointer<LanController> lifetime(owner);
        if (event.kind == Transfer::EventKind::ReceiveOffer) showPrompt(event);
        if (!lifetime) return;
        if (event.kind == Transfer::EventKind::ReceiveFinished) {
            const auto key = QString::number(static_cast<qulonglong>(event.generation)) + u'/' +
                event.data.value(QStringLiteral("sessionId")).toString();
            if (auto prompt = prompts.value(key)) QTimer::singleShot(0, prompt, [prompt] { if (prompt) prompt->close(); });
        }
        if (!event.receivedFiles.empty() && std::any_of(event.receivedFiles.cbegin(), event.receivedFiles.cend(),
            [](const auto& file) { return file.published && !file.path.isEmpty(); })) Q_EMIT owner->archiveChanged();
        if (!lifetime) return;
        if (stopping) {
            if (event.kind == Transfer::EventKind::Disabled) {
                const auto values = prompts.values(); for (auto prompt : values) if (prompt) prompt->close();
            }
            if (!lifetime) return;
            publish();
            // Completion is the final observation. A QApplication host may
            // synchronously destroy this controller once its join is complete.
            if (lifetime) finishShutdown();
            return;
        }
        if (event.kind == Transfer::EventKind::Enabled) {
            if (event.status == HUHU_OK) {
                listeningPort = static_cast<quint16>(event.data.value(QStringLiteral("port")).toInt());
                failureKey = {}; failureStatus = 0;
                if (preferences.discovery) { discoveryTimer.start(); refreshDiscovery(); }
                if (!lifetime) return;
                if (applying.token) finishSettings(ui::DesktopResult::Succeeded);
            } else if (event.status == HUHU_ADDRESS_IN_USE && fallbackAttempts < 4 && !stopping && preferences.enabled) {
                // Only a typed native AddrInUse permits an ephemeral fallback.
                ++fallbackAttempts; requestedPort = 0; needsStart = true;
            } else {
                needsStart = false; reportFailure({}, event.status);
            }
        } else if (event.kind == Transfer::EventKind::Disabled) {
            listeningPort = 0; refreshRequest = 0; discoveryTimer.stop();
            const auto values = prompts.values(); for (auto prompt : values) if (prompt) prompt->close();
            if (!preferences.enabled) {
                if (applying.token) finishSettings(event.status == HUHU_OK ? ui::DesktopResult::Succeeded : ui::DesktopResult::Failed,
                    event.status == HUHU_OK ? QString() : presentation::status(translator, event.status));
            } else maybeStart();
        } else if (event.kind == Transfer::EventKind::DiscoveryRefreshed && event.request == refreshRequest) {
            refreshRequest = 0;
            if (event.status == HUHU_IO) {
                discoveryTimer.stop(); reportFailure(u"lan.discoveryUnavailable");
            } else if (event.status != HUHU_OK && event.status != HUHU_CANCELLED && event.status != HUHU_BUSY)
                reportFailure({}, event.status);
        } else if (event.kind == Transfer::EventKind::ListenerFailed) {
            // A failed listener is not a request to spin up another runtime
            // immediately. Keep the actual failure visible until an explicit
            // Apply; selected-interface changes already have their own path.
            needsStart = false;
            reportFailure(u"lan.listenerFailed");
        }
        if (lifetime) publish();
    }

    void finishShutdown()
    {
        if (!stopping || finished || workPending ||
            (transfers.state() != Transfer::ServiceState::Disabled && transfers.state() != Transfer::ServiceState::Closed)) return;
        finished = true; Q_EMIT owner->shutdownFinished();
    }
};

LanController::LanController(std::shared_ptr<Media::MediaArchiveService> archive,
    Config::SettingsStore& settings, QString privateConfigDirectory, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), settings, std::move(privateConfigDirectory)))
{
}

LanController::~LanController()
{
    state_->stopping = true; state_->cancellation.request_stop();
    state_->interfacesTimer.stop(); state_->discoveryTimer.stop();
    // No worker waits for a GUI callback or owns a QWidget. Final destruction
    // is therefore a real barrier, including when the event loop has stopped.
    state_->pool.waitForDone(); state_->transfers.shutdownAndJoin();
    const auto views = state_->views;
    for (auto view : views) if (view) delete view;
}

void LanController::setTranslator(ui::DesktopTranslator translator)
{
    state_->translator = std::move(translator);
    if (state_->transferWindow) state_->transferWindow->setTranslator(state_->translator);
    if (state_->settingsWindow) state_->settingsWindow->setTranslator(state_->translator);
    for (auto prompt : state_->prompts) if (prompt) prompt->setTranslator(state_->translator);
    state_->publish();
}

bool LanController::initialize()
{
    if (QThread::currentThread() != thread() || state_->initialized || state_->stopping) return false;
    state_->initialized = true;
    state_->validConfig = state_->archive && parsePreferences(state_->settings.value({QStringLiteral("lan")},
        QJsonValue(QJsonValue::Undefined)), state_->preferences);
    if (!state_->validConfig) { state_->reportFailure(u"lan.settingsReadFailure"); return false; }
    state_->interfaces = interfaceAddresses();
    const QPointer<LanController> lifetime(this);
    if (state_->preferences.enabled) state_->reconfigure();
    if (lifetime) state_->publish();
    return true;
}

ui::TransferWindow* LanController::openTransfers(const QVector<Media::Entry>& exactEntries)
{
    if (state_->stopping || !state_->initialized) return nullptr;
    if (!state_->transferWindow) {
        auto* view = new ui::TransferWindow(state_->transfers);
        view->setAttribute(::Qt::WA_DeleteOnClose); view->setTranslator(state_->translator);
        view->setArchiveDirectory(state_->archive ? state_->archive->directory() : QString());
        state_->transferWindow = view;
        state_->retainView(view);
        connect(view, &ui::TransferWindow::viewClosed, this, [this, view] {
            if (state_->transferWindow == view) state_->transferWindow.clear();
        });
        connect(view, &ui::TransferWindow::settingsRequested, this, [this] { openSettings(); });
        connect(view, &ui::TransferWindow::receivedFileRequested, this, &LanController::revealFileRequested);
        connect(view, &ui::TransferWindow::receiveConfirmationRequested, this,
            [this](const Transfer::Event& offer) { state_->showPrompt(offer); });
        connect(view, &ui::TransferWindow::peerReviewRequested, this,
            [this, lifetime = QPointer<ui::TransferWindow>(view)](quint64 token, const QJsonObject& peer) {
                if (lifetime) lifetime->completePeerReview(token,
                    state_->identity.nativeIdentity.isEmpty() ? ui::DesktopResult::Failed : ui::DesktopResult::Succeeded,
                    state_->expectedFingerprint(peer), state_->identity.nativeIdentity.isEmpty() ? state_->text(u"lan.identityUnavailable") : QString());
            });
        connect(view, &ui::TransferWindow::prepareSendRequested, this,
            [this, lifetime = QPointer<ui::TransferWindow>(view)](quint64 token, const QJsonObject& peer,
                const QString& alias, const QVector<Media::Entry>& entries) { state_->prepare(lifetime, token, peer, alias, entries); });
    }
    if (!exactEntries.isEmpty()) state_->transferWindow->setMediaSelection(exactEntries);
    state_->transferWindow->show(); state_->transferWindow->raise(); state_->transferWindow->activateWindow();
    const QPointer<ui::TransferWindow> view(state_->transferWindow);
    state_->refreshDiscovery(); return view;
}

ui::TransferSettingsPanel* LanController::openSettings()
{
    if (state_->stopping || !state_->initialized) return nullptr;
    state_->interfaces = interfaceAddresses();
    if (!state_->settingsWindow) {
        auto* view = new ui::TransferSettingsPanel(state_->transfers);
        view->setAttribute(::Qt::WA_DeleteOnClose); view->setTranslator(state_->translator);
        state_->settingsWindow = view;
        state_->retainView(view);
        connect(view, &ui::TransferSettingsPanel::changeRequested, this,
            [this, view](quint64 token, const ui::LanSettingsChange& change) { state_->apply(view, token, change); });
        connect(view, &ui::TransferSettingsPanel::transfersRequested, this, [this] { openTransfers(); });
        connect(view, &ui::TransferSettingsPanel::viewClosed, this, [this] {
            state_->settingsWindow.clear();
            if (!state_->preferences.enabled) state_->interfacesTimer.stop();
        });
    }
    state_->settingsWindow->setSnapshot(state_->snapshot());
    state_->settingsWindow->show(); state_->settingsWindow->raise(); state_->settingsWindow->activateWindow();
    state_->interfacesTimer.start(); return state_->settingsWindow;
}

ui::LanSettingsSnapshot LanController::settingsSnapshot() const { return state_->snapshot(); }
Transfer::TransferService& LanController::service() noexcept { return state_->transfers; }
bool LanController::available() const { return state_->available(); }
bool LanController::busy() const { return state_->busy(); }

void LanController::shutdown()
{
    if (state_->stopping) return;
    const QPointer<LanController> lifetime(this);
    state_->stopping = true; state_->needsStart = false;
    if (state_->applying.token) state_->finishSettings(ui::DesktopResult::Cancelled);
    if (!lifetime) return;
    state_->cancellation.request_stop(); state_->interfacesTimer.stop(); state_->discoveryTimer.stop();
    const auto prompts = state_->prompts.values(); for (auto prompt : prompts) if (prompt) prompt->close();
    if (!lifetime) return;
    if (state_->transferWindow) state_->transferWindow->close();
    if (!lifetime) return;
    if (state_->settingsWindow) state_->settingsWindow->close();
    if (!lifetime) return;
    (void)state_->transfers.disable(); state_->publish();
    if (lifetime) state_->finishShutdown();
}

} // namespace HuhuCapture::Application
