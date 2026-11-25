#include "app/LanController.h"
#include "platform/FileCommitter.h"
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
#include <QSaveFile>
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
namespace platform = HuhuCapture::Platform;
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

void printCreation(const char* operation, const platform::PrivateFileCreation& value,
    const std::error_code& error)
{
    // Report actual observations even on success. In particular, do not infer
    // a Windows default-owner mismatch just from a permission error elsewhere.
    std::cout << operation << ": stage=" << static_cast<int>(value.stage)
              << " error=" << error.value() << " category=" << error.category().name()
              << " before.ownerObserved=" << value.before.ownerObserved
              << " before.ownerMatchesUser=" << value.before.ownerMatchesUser
              << " before.tokenOwnerObserved=" << value.before.tokenOwnerObserved
              << " before.ownerMatchesTokenOwner=" << value.before.ownerMatchesTokenOwner
              << " before.tokenOwnerMatchesUser=" << value.before.tokenOwnerMatchesUser
              << " before.accessObserved=" << value.before.accessObserved
              << " before.accessPrivate=" << value.before.accessPrivate
              << " ownerChanged=" << value.ownerChanged
              << " unnamedTemporary=" << value.unnamedTemporary
              << " after.ownerObserved=" << value.after.ownerObserved
              << " after.ownerMatchesUser=" << value.after.ownerMatchesUser
              << " after.accessObserved=" << value.after.accessObserved
              << " after.accessPrivate=" << value.after.accessPrivate << '\n';
}

void privateCreationContracts(const QString& root)
{
    const auto directory = QDir(root).filePath(QStringLiteral("new private files"));
    constexpr auto ownerDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    constexpr auto ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    require(QDir().mkpath(directory, ownerDirectory), "Create actual private initialization directory");
    std::error_code error;
    require(platform::isPrivateStorage(files::nativePath(directory), true, error), "Fixture directory is actually private");

    const auto staging = platform::createExclusiveTemporaryFile(files::nativePath(directory), error);
    require(!staging.empty() && !error, "Create the production exclusive empty file");
    const auto claimed = platform::readFileSnapshot(staging, error);
    require(claimed.has_value(), "Read actual newly created file identity");
    auto handle = platform::openOwnedEmptyFileForWrite(staging, claimed->identity, error);
    require(bool(handle), "Open the same claimed new object without truncation");
    auto wrongIdentity = claimed->identity;
    wrongIdentity.file ^= 1;
    platform::PrivateFileCreation creation;
    require(!platform::initializeCreatedPrivateFile(handle.get(), wrongIdentity, error, creation) &&
        error && !creation.ownerChanged, "A mismatched file identity cannot initialize permissions");
    const bool initialized = platform::initializeCreatedPrivateFile(handle.get(), claimed->identity, error, creation);
    printCreation("Exclusive file initialization", creation, error);
    require(initialized && !error && creation.stage == platform::PrivateFileCreationStage::Ready,
        "The actual created empty private object is ready before its first byte");
    require(platform::isPrivateStorage(staging, false, error), "Strict read predicate accepts initialized object");
    const QByteArray content("private content retained\n");
    QFile borrowed;
    require(borrowed.open(handle.get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle) &&
        borrowed.write(content) == content.size() && borrowed.flush(), "Write only after real private initialization");
    require(!platform::initializeCreatedPrivateFile(handle.get(), claimed->identity, error, creation) &&
        error && !creation.ownerChanged, "Creation initialization never mutates a nonempty file");
    require(borrowed.seek(0) && borrowed.readAll() == content, "Refused reinitialization retains all existing bytes");
    borrowed.close(); require(handle.close(error), "Close the exact original file descriptor");

    const auto linked = platform::createExclusiveTemporaryFile(files::nativePath(directory), error);
    require(!linked.empty() && !error, "Create separate actual empty hard-link fixture");
    const auto linkedSnapshot = platform::readFileSnapshot(linked, error);
    require(linkedSnapshot.has_value(), "Read hard-link fixture identity");
    const auto secondName = files::nativePath(QDir(directory).filePath(QStringLiteral("second-name")));
    std::filesystem::create_hard_link(linked, secondName, error);
    require(!error, "Create a real second name for the same empty object");
    auto linkedHandle = platform::openOwnedEmptyFileForWrite(linked, linkedSnapshot->identity, error);
    require(bool(linkedHandle), "Open actual multiply linked fixture without changing it");
    require(!platform::initializeCreatedPrivateFile(linkedHandle.get(), linkedSnapshot->identity, error, creation) &&
        error && !creation.ownerChanged, "Multiply linked empty files cannot be initialized as exclusive creations");
    const auto linkedAfter = platform::readFileDescriptorSnapshot(linkedHandle.get(), error);
    require(linkedAfter && linkedAfter->identity == linkedSnapshot->identity && linkedAfter->bytes == 0,
        "Hard-link rejection retains object identity and its empty content");
#ifndef Q_OS_WIN
    require(std::filesystem::remove(linked, error) && !error &&
        std::filesystem::remove(secondName, error) && !error, "Unlink both owned fixture names while retaining their actual fd");
    require(!platform::initializeCreatedPrivateFile(linkedHandle.get(), linkedSnapshot->identity, error, creation) &&
        error && !creation.ownerChanged && !creation.unnamedTemporary,
        "An ordinary deleted file is not treated as a newly created Linux O_TMPFILE");
#endif
    require(linkedHandle.close(error), "Close the multiply linked object");

    QFile publicFile(QDir(directory).filePath(QStringLiteral("nonprivate-empty")));
    require(publicFile.open(QIODevice::ReadWrite | QIODevice::NewOnly, ownerFile | QFileDevice::ReadOther),
        "Explicitly create a real nonprivate empty fixture");
#ifndef Q_OS_WIN
    // A restrictive process umask must not silently turn this negative fixture
    // into a private file. This is an owned test file, never application data.
    require(publicFile.setPermissions(ownerFile | QFileDevice::ReadOther), "Apply the actual negative fixture mode");
#endif
    const auto publicSnapshot = platform::readFileDescriptorSnapshot(publicFile.handle(), error);
    require(publicSnapshot.has_value(), "Read nonprivate fixture identity");
    require(!platform::initializeCreatedPrivateFile(publicFile.handle(), publicSnapshot->identity, error, creation) &&
        error && creation.before.accessObserved && !creation.before.accessPrivate && !creation.ownerChanged,
        "An existing broad DACL/mode is rejected without repair or ownership changes");
    publicFile.close();
    require(!platform::isPrivateStorage(files::nativePath(publicFile.fileName()), false, error) &&
        read(publicFile.fileName()).isEmpty(), "Rejected nonprivate file keeps its permissions and empty content");

    const auto preservedPath = QDir(directory).filePath(QStringLiteral("existing-private"));
    QFile existing(preservedPath);
    require(existing.open(QIODevice::WriteOnly | QIODevice::NewOnly, ownerFile) &&
        existing.write(content) == content.size(), "Create real private file for atomic cancellation");
    existing.close();
    const auto before = platform::readFileSnapshot(files::nativePath(preservedPath), error);
    require(before.has_value(), "Claim the original file for preservation verification");
    {
        QSaveFile replacement(preservedPath);
        replacement.setDirectWriteFallback(false);
        require(replacement.open(QIODevice::WriteOnly), "Open the real Qt atomic replacement");
        const auto temporary = platform::readFileDescriptorSnapshot(replacement.handle(), error);
        require(temporary.has_value(), "Qt exposes the same new temporary object through its borrowed descriptor");
        const bool ready = platform::initializeCreatedPrivateFile(replacement.handle(), temporary->identity, error, creation);
        printCreation("QSaveFile initialization", creation, error);
        require(ready && !error, "Qt replacement is private before receiving content");
        require(replacement.write("cancelled replacement") == 21, "Write only to the real atomic temporary file");
        replacement.cancelWriting();
        require(!replacement.commit(), "Cancellation does not publish the replacement");
    }
    const auto after = platform::readFileSnapshot(files::nativePath(preservedPath), error);
    require(after && *before == *after && read(preservedPath) == content &&
        platform::isPrivateStorage(files::nativePath(preservedPath), false, error),
        "Atomic cancellation preserves the existing file identity, bytes and strict privacy");
}

void privateIdentityContracts(const QString& root)
{
    const auto location = QDir(root).filePath(QStringLiteral("private identity"));
    transfer::IdentityStore store(location);
    auto first = worker([&] { return store.loadOrCreate(); });
    printCreation("Identity creation", first.fileDiagnostic.creation, first.fileDiagnostic.error);
    if (!first.ok() || !first.created || first.fingerprint.size() != 64) {
        std::error_code directoryError, fileError;
        const bool directoryPrivate = HuhuCapture::Platform::isPrivateStorage(
            files::nativePath(QFileInfo(store.identityPath()).absolutePath()), true, directoryError);
        const bool filePrivate = HuhuCapture::Platform::isPrivateStorage(
            files::nativePath(store.identityPath()), false, fileError);
        // Numeric state and native error codes are sufficient to locate this
        // boundary. Never print the identity JSON, key, SID or user directory.
        std::cerr << "Identity persistence: error=" << static_cast<int>(first.error)
                  << " native=" << first.nativeStatus << " created=" << first.created
                  << " phase=" << static_cast<int>(first.fileDiagnostic.phase)
                  << " fileNativeError=" << first.fileDiagnostic.error.value()
                  << " fileErrorCategory=" << first.fileDiagnostic.error.category().name()
                  << " qtFileError=" << first.fileDiagnostic.qtFileError
                  << " fingerprintLength=" << first.fingerprint.size()
                  << " directoryPrivate=" << directoryPrivate
                  << " directoryError=" << directoryError.value()
                  << " filePrivate=" << filePrivate << " fileError=" << fileError.value()
                  << " fileExists=" << QFileInfo::exists(store.identityPath()) << '\n';
    }
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
    const auto originalTrust = platform::readFileSnapshot(files::nativePath(reopened.trustPath()), error);
    auto updated = worker([&] { return reopened.rememberPeer(QStringLiteral("127.0.0.2"), 53318,
        first.fingerprint, QStringLiteral("Second explicit confirmation")); });
    printCreation("Trust replacement", updated.fileDiagnostic.creation, updated.fileDiagnostic.error);
    require(updated.ok() && updated.trustedPeers.size() == 1 &&
        updated.trustedPeers.first().toObject().value(QStringLiteral("endpoints")).toArray().size() == 2,
        "A second real confirmation updates existing trust through QSaveFile");
    const auto updatedTrust = platform::readFileSnapshot(files::nativePath(reopened.trustPath()), error);
    require(originalTrust && updatedTrust && originalTrust->identity != updatedTrust->identity &&
        platform::isPrivateStorage(files::nativePath(reopened.trustPath()), false, error) && read(reopened.identityPath()) == original,
        "Atomic trust replacement has a new private object and preserves the original TLS key");
    transfer::IdentityStore updatedStore(location);
    auto updatedRead = worker([&] { return updatedStore.loadOrCreate(); });
    require(updatedRead.ok() && !updatedRead.created && updatedRead.fingerprint == first.fingerprint &&
        updatedRead.trustedPeers == updated.trustedPeers, "Strict reopen accepts the real replacement without changing trust");
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
    privateCreationContracts(temporary.path());
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
    shutdown(*restarted); shutdown(receiver);
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
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--identity-only")) {
            QTemporaryDir temporary; require(temporary.isValid(), "Create real isolated identity fixture");
            privateIdentityContracts(temporary.path()); privateCreationContracts(temporary.path());
            std::cout << "Actual private creation, RSA persistence, atomic trust replacement and data preservation passed. "
                "Explicit identity-only run: network/controller contracts NOT RUN.\n";
            return 0;
        }
        const auto pictures = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        if (!pictures.isEmpty()) require(QDir().mkpath(pictures), "Create actual controller PNG output directory");
        controllerContracts(pictures); configurationConflict();
        std::cout << "Production LAN controller: actual private identity persistence, shared settings, interface selection, "
            "typed port fallback, TLS transfer after view destruction, global receive confirmation, shared archive and shutdown passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LAN controller validation failed: " << error.what() << '\n'; return 1;
    }
}
