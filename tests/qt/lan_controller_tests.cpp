#include "app/LanController.h"
#include "platform/PrivateStorage.h"
#include "services/files/QtPaths.h"
#include "services/localization/LanguageCatalog.h"
#include "services/transfer/IdentityStore.h"
#include "services/transfer/ReceiveArchiveSink.h"
#include "ui/qt/ReceivePrompt.h"
#include "ui/qt/TransferWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace application = HuhuCapture::Application;
namespace config = HuhuCapture::Config;
namespace files = HuhuCapture::Files;
namespace media = HuhuCapture::Media;
namespace transfer = HuhuCapture::Transfer;
namespace ui = HuhuCapture::UI::Qt;
namespace localization = HuhuCapture::Localization;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Condition> void waitFor(Condition condition, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 35s;
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        settle(); std::this_thread::sleep_for(1ms);
    }
}
template<class Work> auto worker(Work work)
{
    auto future = std::async(std::launch::async, std::move(work));
    waitFor([&] { return future.wait_for(0ms) == std::future_status::ready; }, "Real file worker did not finish");
    return future.get();
}
template<class Widget> Widget* child(QWidget& parent, const char* name)
{
    auto* result = parent.findChild<Widget*>(QString::fromLatin1(name)); require(result != nullptr, name); return result;
}
void click(QAbstractButton& button, bool pump = true)
{
    require(button.isEnabled(), "Actual control must be enabled");
    const QPointF point = qobject_cast<QCheckBox*>(&button) ? QPointF(10, button.height() / 2) : QPointF(button.rect().center());
    const QPointF global = button.mapToGlobal(point.toPoint());
    QMouseEvent down(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&button, &down); QApplication::sendEvent(&button, &up); if (pump) settle();
}

QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read actual stored file");
    auto result = file.readAll(); require(file.error() == QFileDevice::NoError, "Complete file read"); return result;
}

void replace(const QString& path, const QByteArray& bytes)
{
    QFile file(path); require(file.open(QIODevice::WriteOnly), "Open existing fixture for explicit mutation");
    require(file.write(bytes) == bytes.size() && file.flush(), "Write exact fixture bytes");
    file.close(); require(file.error() == QFileDevice::NoError, "Close fixture mutation");
}

struct Events {
    explicit Events(transfer::TransferService& service)
    { connection = QObject::connect(&service, &transfer::TransferService::eventReady, &service,
        [this](const transfer::Event& event) { values.push_back(event); }); }
    ~Events() { QObject::disconnect(connection); }
    std::optional<transfer::Event> find(transfer::EventKind kind, std::size_t begin = 0) const
    { for (auto index = begin; index < values.size(); ++index) if (values[index].kind == kind) return values[index]; return {}; }
    transfer::Event wait(transfer::EventKind kind, std::size_t begin = 0)
    { waitFor([&] { return find(kind, begin).has_value(); }, "Production service event did not arrive"); return *find(kind, begin); }
    std::vector<transfer::Event> values;
    QMetaObject::Connection connection;
};

void chooseLoopback(ui::TransferSettingsPanel& panel, const QString& alias, quint16 port)
{
    auto* enabled = child<QCheckBox>(panel, "lanEnable"); if (!enabled->isChecked()) click(*enabled);
    child<QLineEdit>(panel, "lanAlias")->setText(alias);
    child<QSpinBox>(panel, "lanPort")->setValue(port);
    auto* discovery = child<QCheckBox>(panel, "lanDiscovery"); if (discovery->isChecked()) click(*discovery);
    auto* interfaces = child<QListWidget>(panel, "lanInterfaces"); bool found{};
    for (int index = 0; index < interfaces->count(); ++index) {
        auto* item = interfaces->item(index);
        const bool selected = item->data(Qt::UserRole).toString() == QStringLiteral("127.0.0.1");
        item->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
        if (selected) { require(item->data(Qt::UserRole + 3).toBool(), "Loopback comes from an available real OS interface"); found = true; }
    }
    require(found, "Real host has IPv4 loopback");
}

void enable(application::LanController& controller, const QString& alias, quint16 port)
{
    auto* panel = controller.openSettings(); require(panel != nullptr, "Open actual LAN settings"); settle();
    chooseLoopback(*panel, alias, port);
    require(panel->requestApply(), "Apply actual interface selection");
    waitFor([&] { return !panel->changePending(); }, "Enable settings did not finish");
    require(controller.available() && controller.settingsSnapshot().listeningPort > 0,
        "Production application controller enabled a real TLS listener");
}

void shutdown(application::LanController& controller)
{
    bool finished{};
    auto connection = QObject::connect(&controller, &application::LanController::shutdownFinished, &controller, [&] { finished = true; });
    controller.shutdown(); waitFor([&] { return finished; }, "Real controller shutdown did not join workers and native engine");
    QObject::disconnect(connection);
    require(!controller.service().statistics().nativeRunning && !controller.available(), "Shutdown leaves no native runtime");
}

media::Entry makePng(const std::shared_ptr<media::MediaArchiveService>& archive)
{
    return worker([archive] {
        QImage image(480, 320, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, qRgb((x * 37 + y) % 256, (y * 17 + x) % 256, (x ^ y) % 256));
        const auto saved = archive->archiveImage(image); require(saved.durable(), "Archive actual full PNG");
        const auto listing = archive->list(); require(!listing.failure, "List actual archive");
        auto found = std::find_if(listing.entries.cbegin(), listing.entries.cend(), [&](const auto& entry) { return entry.path == saved.path; });
        require(found != listing.entries.cend(), "Created PNG has exact managed entry"); return *found;
    });
}

ui::ReceivePrompt* promptFor(const QString& session)
{
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* prompt = qobject_cast<ui::ReceivePrompt*>(widget); prompt && prompt->sessionId() == session) return prompt;
    return nullptr;
}

ui::TransferWindow* choosePeer(application::LanController& sender, const media::Entry& source, quint16 port)
{
    auto* view = sender.openTransfers({source}); require(view != nullptr, "Open real application transfer view");
    require(bool(view->discover(QStringLiteral("127.0.0.1"), port)), "Admit actual TLS peer discovery");
    waitFor([&] { return view->deviceCount() == 1 && child<QPushButton>(*view, "transferSend")->isEnabled(); },
        "Actual peer discovery and application trust review did not finish");
    return view;
}

void confirmAndDestroy(ui::TransferWindow* view)
{
    click(*child<QPushButton>(*view, "transferSend"));
    auto* box = child<QMessageBox>(*view, "transferSendConfirmation");
    require(box->defaultButton() == box->button(QMessageBox::No), "Real send dialog defaults to No");
    click(*box->button(QMessageBox::Yes), false);
    require(view->preparationPending(), "Confirmed intent is owned by the production worker before its GUI completion");
    // No event-loop turn between confirmation and deletion: the real worker's
    // completion must return to the application after this QWidget is gone.
    view->close(); delete view; settle();
}

void privateIdentityContracts(const QString& root)
{
    const auto location = QDir(root).filePath(QStringLiteral("private identity"));
    transfer::IdentityStore store(location);
    auto first = worker([&] { return store.loadOrCreate(); });
    require(first.ok() && first.created && first.fingerprint.size() == 64, "Persist official native RSA identity");
    std::error_code error;
    require(HuhuCapture::Platform::isPrivateStorage(files::nativePath(store.identityPath()), false, error),
        "Saved identity has actual private owner permissions/ACL");
    const auto original = read(store.identityPath());
    transfer::IdentityStore reopened(location);
    auto same = worker([&] { return reopened.loadOrCreate(); });
    require(same.ok() && !same.created && same.fingerprint == first.fingerprint && read(store.identityPath()) == original,
        "Reopening preserves exact key bytes and DER identity");
    auto peer = worker([&] { return reopened.rememberPeer(QStringLiteral("127.0.0.1"), 53317,
        first.fingerprint, QStringLiteral("Explicit confirmation")); });
    require(peer.ok() && peer.trustedPeers.size() == 1, "Persist only a user-confirmed public fingerprint");
    const auto trust = read(reopened.trustPath());
    require(!trust.contains("PRIVATE KEY") && !trust.contains("privateKeyPem"), "Trust metadata excludes private keys");
    replace(reopened.identityPath(), QByteArray("damaged identity\n"));
    auto damaged = worker([&] { transfer::IdentityStore candidate(location); return candidate.loadOrCreate(); });
    require(damaged.error == transfer::IdentityError::InvalidIdentity && read(reopened.identityPath()) == "damaged identity\n",
        "Damaged saved identity fails without silently replacing bytes");
    replace(reopened.identityPath(), original);
    const auto changedTrust = QByteArray("{\"schema\":99,\"peers\":[]}");
    replace(reopened.trustPath(), changedTrust);
    auto future = worker([&] { transfer::IdentityStore candidate(location); return candidate.loadOrCreate(); });
    require(future.error == transfer::IdentityError::InvalidTrust && read(reopened.trustPath()) == changedTrust,
        "Unknown trust schema is retained and not interpreted as empty trust");
}

void controllerContracts(const QString& pictures)
{
    QTemporaryDir temporary; require(temporary.isValid(), "Create real controller fixture directory");
    privateIdentityContracts(temporary.path());
    const auto senderRoot = QDir(temporary.path()).filePath(QStringLiteral("sender"));
    const auto receiverRoot = QDir(temporary.path()).filePath(QStringLiteral("receiver"));
    config::SettingsStore senderSettings(QDir(senderRoot).filePath(QStringLiteral("config.json")));
    config::SettingsStore receiverSettings(QDir(receiverRoot).filePath(QStringLiteral("config.json")));
    require(senderSettings.load().ok() && receiverSettings.load().ok(), "Load shared real application configuration");
    require(senderSettings.setValue({QStringLiteral("unrelatedFutureField")}, QStringLiteral("keep")).ok(),
        "Create unrelated configuration metadata for preservation check");
    auto senderArchive = std::make_shared<media::MediaArchiveService>(QDir(senderRoot).filePath(QStringLiteral("媒体")));
    auto receiverArchive = std::make_shared<media::MediaArchiveService>(QDir(receiverRoot).filePath(QStringLiteral("媒体")));
    localization::LanguageCatalog languages({localization::ResourcePolicy::LinuxUserResources, temporary.path(), temporary.path()});
    require(languages.load(QStringLiteral("en-US")).usable, "Load actual language catalog");
    const ui::DesktopTranslator translate = [&languages](QStringView key) { return languages.text(key); };
    auto sender = std::make_unique<application::LanController>(senderArchive, senderSettings, senderRoot);
    application::LanController receiver(receiverArchive, receiverSettings, receiverRoot);
    sender->setTranslator(translate); receiver.setTranslator(translate);
    require(sender->initialize() && receiver.initialize(), "Absent LAN preferences initialize as disabled");
    const auto identityPath = QDir(senderRoot).filePath(QStringLiteral("lan/identity.json"));
    require(!QFileInfo::exists(identityPath) && !sender->busy() && !sender->service().statistics().nativeRunning,
        "Cold default-disabled app creates no identity, worker or transfer runtime");
    auto* coldSettings = sender->openSettings(); auto* coldTransfers = sender->openTransfers(); settle();
    require(coldSettings && coldTransfers && !QFileInfo::exists(identityPath) && !sender->service().statistics().nativeRunning,
        "Opening real views while disabled does not silently enable networking");
    coldSettings->close(); coldTransfers->close(); settle();

    QTcpServer collision;
    require(collision.listen(QHostAddress::LocalHost, 0), "Reserve an actual TCP port to force bind conflict");
    const auto occupiedPort = collision.serverPort();
    Events sent(sender->service()), received(receiver.service());
    enable(*sender, QStringLiteral("Sender application"), occupiedPort);
    require(std::any_of(sent.values.cbegin(), sent.values.cend(), [](const auto& event) {
        return event.kind == transfer::EventKind::Enabled && event.status == HUHU_ADDRESS_IN_USE;
    }), "Native bind collision is a typed AddrInUse result");
    require(sender->settingsSnapshot().listeningPort != occupiedPort && sender->settingsSnapshot().listeningPort != 0,
        "Controller starts an actual alternative port only after typed collision");
    const auto senderFingerprint = sender->settingsSnapshot().fingerprint;
    require(senderSettings.value({QStringLiteral("unrelatedFutureField")}).toString() == QStringLiteral("keep"),
        "LAN settings preserve unrelated configuration fields");
    require(senderSettings.value({QStringLiteral("lan"), QStringLiteral("enabled")}).toBool(), "Explicit enable is persisted");
    enable(receiver, QStringLiteral("Receiver application"), occupiedPort);
    auto* receiverSettingsView = receiver.openSettings();
    const auto receiverPort = receiver.settingsSnapshot().listeningPort;
    require(child<QLabel>(*receiverSettingsView, "lanServiceState")->text().contains(QString::number(receiverPort)),
        "Settings displays the actual fallback listener port");
    if (!pictures.isEmpty()) require(receiverSettingsView->grab().save(QDir(pictures).filePath(QStringLiteral("lan-controller-settings-en.png"))),
        "Save real controller settings pixels");
    receiverSettingsView->close(); settle();

    const auto source = makePng(senderArchive);
    unsigned archiveChanges{};
    QObject::connect(&receiver, &application::LanController::archiveChanged, &receiver, [&] { ++archiveChanges; });
    auto* firstView = choosePeer(*sender, source, receiverPort);
    confirmAndDestroy(firstView);
    const auto offer = received.wait(transfer::EventKind::ReceiveOffer);
    const auto session = offer.data.value(QStringLiteral("sessionId")).toString();
    waitFor([&] { return promptFor(session) != nullptr; }, "Application owns a real receive prompt without an open transfer window");
    require(senderArchive->reserveRemoval(source).failure.code == media::Error::Busy,
        "Production worker/native service retain exact archive lease after view destruction");
    auto* prompt = promptFor(session);
    if (!pictures.isEmpty()) require(prompt->grab().save(QDir(pictures).filePath(QStringLiteral("lan-controller-receive-en.png"))),
        "Save real globally routed receive confirmation pixels");
    require(bool(prompt->acceptSelected()), "Actual user receive decision is admitted");
    const auto sentEnd = sent.wait(transfer::EventKind::SendFinished);
    const auto receivedEnd = received.wait(transfer::EventKind::ReceiveFinished);
    require(sentEnd.status == HUHU_OK && receivedEnd.status == HUHU_OK && receivedEnd.receivedFiles.size() == 1,
        "Two production controllers finish a real TLS transfer and archive commit");
    require(receivedEnd.receivedFiles.front().published && read(receivedEnd.receivedFiles.front().path) == read(source.path),
        "Received archive contains exactly the source bytes");
    require(archiveChanges > 0 && receiverArchive->list().entries.size() == 1,
        "Receiver routes real archive change to the shared application archive");
    require(!senderArchive->reserveRemoval(source).failure, "Native END releases the actual send lease");

    const auto trustedPath = QDir(senderRoot).filePath(QStringLiteral("lan/trusted-peers.json"));
    require(read(trustedPath).contains(receiver.settingsSnapshot().fingerprint.toUtf8()), "Explicit peer confirmation persists actual TLS identity");
    auto* knownView = choosePeer(*sender, source, receiverPort);
    require(child<QLabel>(*knownView, "transferTrust")->text() == languages.text(u"lan.knownIdentity"),
        "Next real TLS probe resolves the stored fingerprint");
    const auto sentBegin = sent.values.size(), receiveBegin = received.values.size();
    confirmAndDestroy(knownView);
    const auto rejectedOffer = received.wait(transfer::EventKind::ReceiveOffer, receiveBegin);
    const auto rejectedSession = rejectedOffer.data.value(QStringLiteral("sessionId")).toString();
    waitFor([&] { return promptFor(rejectedSession) != nullptr; }, "Second actual offer opens one application prompt");
    require(promptFor(rejectedSession)->close(), "Closing global receive confirmation explicitly declines");
    const auto rejected = sent.wait(transfer::EventKind::SendFinished, sentBegin);
    require(rejected.status == HUHU_CANCELLED && rejected.data.value(QStringLiteral("httpStatus")).toInt() == 403,
        "Close is a real HTTP refusal, never implicit acceptance");
    require(receiverArchive->list().entries.size() == 1, "Declined receive publishes no second file");

    {
        // A second real loopback endpoint carries the receiver's SAME saved
        // certificate. This verifies identity/IP handling without inventing
        // discovery events; it does not stand in for physical NIC testing.
        auto identity = worker([&] {
            transfer::IdentityStore store(receiverRoot); return store.loadOrCreate();
        });
        require(identity.ok(), "Load actual receiver identity for an alternate-address fixture");
        auto configuration = identity.nativeIdentity;
        configuration.insert(QStringLiteral("alias"), QStringLiteral("Receiver alternate address"));
        configuration.insert(QStringLiteral("addresses"), QJsonArray{QStringLiteral("127.0.0.2")});
        configuration.insert(QStringLiteral("port"), 0); configuration.insert(QStringLiteral("discovery"), true);
        transfer::TransferService alternate; Events alternateEvents(alternate);
        require(bool(alternate.enable(QJsonDocument(configuration).toJson(QJsonDocument::Compact),
            std::make_unique<transfer::ReceiveArchiveSink>(receiverArchive))), "Admit actual alternate TLS listener");
        const auto started = alternateEvents.wait(transfer::EventKind::Enabled);
        require(started.status == HUHU_OK, "Bind real alternate loopback address");
        const auto alternatePort = static_cast<quint16>(started.data.value(QStringLiteral("port")).toInt());
        const auto announcement = alternate.refreshDiscovery(); require(bool(announcement), "Admit real official multicast announcement");
        const auto announcementEnd = alternateEvents.wait(transfer::EventKind::DiscoveryRefreshed);
        require(announcementEnd.request == announcement.request &&
            (announcementEnd.status == HUHU_OK || announcementEnd.status == HUHU_IO),
            "Official announcement reports actual success or actual unavailable multicast with one typed terminal");
        std::cout << "Actual loopback multicast announcement status: " << announcementEnd.status << '\n';
        auto* aliases = sender->openTransfers({source});
        require(bool(aliases->discover(QStringLiteral("127.0.0.2"), alternatePort)), "Probe real alternate TLS endpoint");
        waitFor([&] { return aliases->selectedEndpoint() == QStringLiteral("127.0.0.2:") + QString::number(alternatePort) &&
            child<QPushButton>(*aliases, "transferSend")->isEnabled(); }, "Alternate actual identity probe did not finish");
        require(aliases->deviceCount() == 1, "Multiple real endpoints of one certificate share one device row");
        require(child<QLabel>(*aliases, "transferTrust")->text() == languages.text(u"lan.knownIdentity"),
            "A known certificate at a different IP is not reported as a changed identity");
        require(aliases->selectEndpoint(QStringLiteral("127.0.0.1"), receiverPort) && aliases->deviceCount() == 1,
            "Selecting another endpoint preserves one certificate-based device row");
        aliases->close(); settle();
        const auto beforeStop = alternateEvents.values.size(); require(alternate.disable() == HUHU_OK, "Stop alternate native fixture");
        const auto stopped = alternateEvents.wait(transfer::EventKind::Disabled, beforeStop);
        require(stopped.status == HUHU_OK, "Alternate listener/discovery and sink actually join");
    }

    const auto beforeRestart = read(identityPath);
    shutdown(*sender);
    sender.reset();
    auto restarted = std::make_unique<application::LanController>(senderArchive, senderSettings, senderRoot);
    restarted->setTranslator(translate);
    require(restarted->initialize(), "Persisted explicit enable restores application intent");
    waitFor([&] { return restarted->available() || (!restarted->busy() && !restarted->settingsSnapshot().unavailableReason.isEmpty()); },
        "Restart listener did not settle");
    require(restarted->available() && restarted->settingsSnapshot().fingerprint == senderFingerprint && read(identityPath) == beforeRestart,
        "Application restart preserves exact TLS identity and uses persisted explicit settings");
    auto* restartedView = choosePeer(*restarted, source, receiverPort);
    require(child<QLabel>(*restartedView, "transferTrust")->text() == languages.text(u"lan.knownIdentity"),
        "Confirmed fingerprint survives application restart");
    restartedView->close(); settle();
    auto* disablePanel = restarted->openSettings(); settle();
    auto* enabled = child<QCheckBox>(*disablePanel, "lanEnable"); require(enabled->isChecked(), "Saved explicit enabled preference is visible");
    click(*enabled); require(disablePanel->requestApply(), "Explicit settings disable is admitted");
    waitFor([&] { return !disablePanel->changePending(); }, "Disable did not finish");
    require(!restarted->available() && !restarted->service().statistics().nativeRunning &&
        !senderSettings.value({QStringLiteral("lan"), QStringLiteral("enabled")}).toBool(),
        "Disable stops the actual runtime and persists disabled preference");

    enable(*restarted, QStringLiteral("Disable conflict fixture"), occupiedPort);
    disablePanel = restarted->openSettings(); settle();
    const auto externallyChanged = QByteArray("{\"external\":\"must be retained\"}\n");
    replace(senderSettings.path(), externallyChanged);
    click(*child<QCheckBox>(*disablePanel, "lanEnable"));
    require(disablePanel->requestApply(), "Explicit disable reaches the actual persistence conflict");
    waitFor([&] { return !disablePanel->changePending() && !restarted->service().statistics().nativeRunning; },
        "Failed persistence must still stop the real listener");
    require(!restarted->available() && !restarted->settingsSnapshot().enabled &&
        read(senderSettings.path()) == externallyChanged &&
        child<QLabel>(*disablePanel, "lanSettingsStatus")->text() == languages.text(u"lan.disableSaveFailure"),
        "Disable remains effective and reports persistence failure without overwriting external configuration");
    shutdown(*restarted); shutdown(receiver);
}

void hostLifetimeContracts()
{
    QTemporaryDir temporary; require(temporary.isValid(), "Create host lifetime fixture");
    config::SettingsStore settings(QDir(temporary.path()).filePath(QStringLiteral("config.json")));
    require(settings.load().ok(), "Load actual host lifetime settings");
    auto archive = std::make_shared<media::MediaArchiveService>(QDir(temporary.path()).filePath(QStringLiteral("media")));
    QObject host;
    {
        auto controller = std::make_unique<application::LanController>(archive, settings, temporary.path());
        require(controller->initialize(), "Initialize actual disabled controller");
        QPointer<application::LanController> lifetime(controller.get());
        QObject::connect(controller.get(), &application::LanController::busyChanged, &host,
            [&](bool) { controller.reset(); });
        controller->shutdown();
        require(lifetime.isNull(), "Host may synchronously release a disabled controller during shutdown state notification");
    }
    {
        auto controller = std::make_unique<application::LanController>(archive, settings, temporary.path());
        require(controller->initialize(), "Initialize actual asynchronous lifetime fixture");
        QTcpServer collision;
        require(collision.listen(QHostAddress::LocalHost, 0), "Reserve a real test port");
        enable(*controller, QStringLiteral("Host lifetime fixture"), collision.serverPort());
        QPointer<application::LanController> lifetime(controller.get());
        bool joined{};
        QObject::connect(controller.get(), &application::LanController::shutdownFinished, &host, [&] {
            joined = !controller->service().statistics().nativeRunning;
            controller.reset();
        });
        controller->shutdown();
        waitFor([&] { return lifetime.isNull(); }, "Host release on real asynchronous join completion did not finish");
        require(joined, "shutdownFinished follows real native resource release and permits immediate destruction");
    }
    require(settings.setValue({QStringLiteral("lan")}, QStringLiteral("invalid fixture")).ok(),
        "Write an actual invalid LAN preference without changing the shared configuration format");
    auto invalid = std::make_unique<application::LanController>(archive, settings, temporary.path());
    QPointer<application::LanController> invalidLifetime(invalid.get());
    QObject::connect(invalid.get(), &application::LanController::operationFailed, &host,
        [&](const QString&) { invalid.reset(); });
    require(!invalid->initialize() && invalidLifetime.isNull(),
        "Host may synchronously destroy a controller in its actual invalid-configuration callback");
}

void configurationConflict()
{
    QTemporaryDir temporary; require(temporary.isValid(), "Create config-conflict fixture");
    config::SettingsStore settings(QDir(temporary.path()).filePath(QStringLiteral("config.json")));
    require(settings.load().ok() && settings.save().ok(), "Create actual shared configuration");
    auto archive = std::make_shared<media::MediaArchiveService>(QDir(temporary.path()).filePath(QStringLiteral("media")));
    application::LanController controller(archive, settings, temporary.path()); require(controller.initialize(), "Initialize disabled conflict fixture");
    auto* panel = controller.openSettings(); settle(); chooseLoopback(*panel, QStringLiteral("Conflict fixture"), 53317);
    const auto changed = QByteArray("{\"external\":\"retained\"}\n"); replace(settings.path(), changed);
    require(panel->requestApply(), "Actual settings request reaches shared persistence conflict check");
    waitFor([&] { return !panel->changePending(); }, "Conflict result did not arrive");
    require(!controller.available() && !controller.service().statistics().nativeRunning &&
        read(settings.path()) == changed && !QFileInfo::exists(QDir(temporary.path()).filePath(QStringLiteral("lan/identity.json"))),
        "Changed-on-disk config is retained; failed enable creates no identity or listener");
    shutdown(controller);
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        const auto pictures = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        if (!pictures.isEmpty()) require(QDir().mkpath(pictures), "Create actual controller PNG output directory");
        controllerContracts(pictures); configurationConflict(); hostLifetimeContracts();
        std::cout << "Production LAN controller: actual private identity persistence, shared settings, interface selection, "
            "typed port fallback, TLS transfer after view destruction, global receive confirmation, shared archive and shutdown passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LAN controller validation failed: " << error.what() << '\n'; return 1;
    }
}
