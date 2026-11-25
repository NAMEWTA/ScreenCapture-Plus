#include "ui/qt/TransferWindow.h"
#include "ui/qt/ReceivePrompt.h"
#include "ui/qt/TransferSettingsPanel.h"
#include "services/transfer/ReceiveArchiveSink.h"
#include "services/localization/LanguageCatalog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace transfer = HuhuCapture::Transfer;
namespace media = HuhuCapture::Media;
namespace ui = HuhuCapture::UI::Qt;
namespace localization = HuhuCapture::Localization;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void waitFor(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message); settle(); std::this_thread::sleep_for(1ms);
    }
}
template<class Work> auto worker(Work work)
{
    auto task = std::async(std::launch::async, std::move(work));
    waitFor([&] { return task.wait_for(0ms) == std::future_status::ready; }, "Real fixture worker timed out");
    return task.get();
}
template<class T> T* child(QWidget& widget, const char* name)
{
    auto* result = widget.findChild<T*>(QString::fromLatin1(name)); require(result != nullptr, name); return result;
}
void click(QAbstractButton& button)
{
    require(button.isEnabled(), "The actual control must be actionable before a click");
    const QPointF point = qobject_cast<QCheckBox*>(&button) ? QPointF(10, button.height() / 2) : QPointF(button.rect().center());
    const QPointF global = button.mapToGlobal(point.toPoint());
    QMouseEvent down(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&button, &down); QApplication::sendEvent(&button, &up); settle();
}
void show(QWidget& widget) { widget.show(); widget.raise(); widget.activateWindow(); settle(); }
void save(QWidget& widget, const QString& directory, const QString& file)
{ if (!directory.isEmpty()) require(widget.grab().save(QDir(directory).filePath(file)), "Save real LAN widget pixels"); }

QByteArray identity(const char* alias, quint16 port = 0)
{
    return worker([=] {
        HuhuBuffer* raw{}; require(huhu_identity_generate(&raw) == HUHU_OK, "Generate an actual LocalSend TLS identity");
        std::unique_ptr<HuhuBuffer, decltype(&huhu_buffer_free)> owner(raw, &huhu_buffer_free);
        HuhuSlice view{}; require(huhu_buffer_view(raw, &view) == HUHU_OK, "Read actual generated identity");
        auto config = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(view.data),
            static_cast<qsizetype>(view.length))).object();
        config.insert(QStringLiteral("alias"), QString::fromUtf8(alias));
        config.insert(QStringLiteral("addresses"), QJsonArray{QStringLiteral("127.0.0.1")});
        config.insert(QStringLiteral("port"), port); config.insert(QStringLiteral("discovery"), false);
        config.insert(QStringLiteral("decisionTimeoutMs"), 15000);
        config.insert(QStringLiteral("idleTimeoutMs"), 15000); config.insert(QStringLiteral("commitTimeoutMs"), 15000);
        return QJsonDocument(config).toJson(QJsonDocument::Compact);
    });
}

class Events final {
public:
    explicit Events(transfer::TransferService& service)
    {
        connection = QObject::connect(&service, &transfer::TransferService::eventReady, &service,
            [this](const transfer::Event& event) { values.push_back(event); });
    }
    ~Events() { QObject::disconnect(connection); }
    std::optional<transfer::Event> find(transfer::EventKind kind, transfer::RequestId request = 0, std::size_t begin = 0) const
    {
        for (auto index = begin; index < values.size(); ++index)
            if (values[index].kind == kind && (!request || values[index].request == request)) return values[index];
        return {};
    }
    transfer::Event wait(transfer::EventKind kind, transfer::RequestId request = 0, std::size_t begin = 0)
    {
        waitFor([&] { return find(kind, request, begin).has_value(); }, "Actual transfer event did not arrive");
        return *find(kind, request, begin);
    }
    std::vector<transfer::Event> values;
private:
    QMetaObject::Connection connection;
};

void diagnoseReceivePrompt(const ui::ReceivePrompt& prompt, const transfer::Event& offer,
    transfer::TransferService& receiver, transfer::TransferService& sender,
    transfer::RequestId sendRequest, const QJsonArray& stages)
{
    // This fixture creates its own PNG and TLS peers. Keep failure evidence
    // bounded and omit paths, aliases, fingerprints, session IDs and tokens.
    const auto selected = prompt.selectedFiles();
    QJsonArray files;
    const auto offered = offer.data.value(QStringLiteral("files")).toArray();
    for (qsizetype index = 0; index < offered.size() && index < 64; ++index) {
        const auto file = offered[index].toObject();
        const auto id = file.value(QStringLiteral("id")).toString();
        files.append(QJsonObject{{QStringLiteral("index"), index},
            {QStringLiteral("fileType"), file.value(QStringLiteral("fileType")).toString().left(64)},
            {QStringLiteral("size"), file.value(QStringLiteral("size")).toInteger(-1)},
            {QStringLiteral("nameLength"), file.value(QStringLiteral("fileName")).toString().size()},
            {QStringLiteral("hasFileId"), !id.isEmpty()},
            {QStringLiteral("selected"), selected.contains(id)}});
    }
    QJsonArray rows;
    const auto* tree = prompt.findChild<QTreeWidget*>(QStringLiteral("receiveFiles"));
    if (tree) for (int index = 0; index < tree->topLevelItemCount() && index < 64; ++index) {
        const auto* item = tree->topLevelItem(index);
        rows.append(QJsonObject{{QStringLiteral("index"), index},
            {QStringLiteral("enabled"), item->flags().testFlag(Qt::ItemIsEnabled)},
            {QStringLiteral("checkable"), item->flags().testFlag(Qt::ItemIsUserCheckable)},
            {QStringLiteral("checkState"), static_cast<int>(item->checkState(0))}});
    }
    auto snapshot = [&](transfer::TransferService& service, bool incoming) {
        QJsonArray events;
        const auto view = service.viewSnapshot();
        if (view) for (const auto& event : view->events) {
            if (incoming ? event.generation != offer.generation ||
                event.data.value(QStringLiteral("sessionId")) != offer.data.value(QStringLiteral("sessionId")) :
                event.request != sendRequest) continue;
            events.append(QJsonObject{{QStringLiteral("kind"), static_cast<int>(event.kind)},
                {QStringLiteral("status"), event.status},
                {QStringLiteral("localStatus"), event.data.value(QStringLiteral("localStatus")).toInt(event.status)},
                {QStringLiteral("httpStatus"), event.data.value(QStringLiteral("httpStatus")).toInt()},
                {QStringLiteral("sequence"), QString::number(event.sequence)}});
        }
        return QJsonObject{{QStringLiteral("state"), static_cast<int>(service.state())},
            {QStringLiteral("nativeRunning"), service.statistics().nativeRunning},
            {QStringLiteral("events"), events}};
    };
    const QJsonObject evidence{{QStringLiteral("decisionTimeoutMs"), 15000},
        {QStringLiteral("offerKind"), static_cast<int>(offer.kind)},
        {QStringLiteral("offerFileCount"), offered.size()},
        {QStringLiteral("selectedCount"), selected.size()},
        {QStringLiteral("answered"), prompt.answered()},
        {QStringLiteral("decisionPending"), prompt.decisionPending()},
        {QStringLiteral("filesEnabled"), tree && tree->isEnabled()},
        {QStringLiteral("files"), files}, {QStringLiteral("rows"), rows},
        {QStringLiteral("receiver"), snapshot(receiver, true)},
        {QStringLiteral("sender"), snapshot(sender, false)},
        {QStringLiteral("stages"), stages}};
    std::cerr << "Receive prompt state diagnostic: " <<
        QJsonDocument(evidence).toJson(QJsonDocument::Compact).constData() << '\n';
}

QJsonObject enable(transfer::TransferService& service, Events& events, const QByteArray& configuration,
    const std::shared_ptr<media::MediaArchiveService>& archive)
{
    const auto admitted = service.enable(configuration, std::make_unique<transfer::ReceiveArchiveSink>(archive));
    require(bool(admitted), "Explicit enable must be admitted");
    const auto enabled = events.wait(transfer::EventKind::Enabled, admitted.request);
    require(enabled.status == HUHU_OK && enabled.data.value(QStringLiteral("port")).toInt() > 0, "Start a real loopback TLS listener");
    return enabled.data;
}

void stop(transfer::TransferService& service, Events& events)
{
    if (service.state() == transfer::ServiceState::Disabled) return;
    const auto begin = events.values.size(); require(service.disable() == HUHU_OK, "Admit asynchronous disable");
    const auto done = events.wait(transfer::EventKind::Disabled, 0, begin);
    require(done.status == HUHU_OK && !service.statistics().nativeRunning, "Actual runtime and receive workers join");
}

media::Entry createPng(const std::shared_ptr<media::MediaArchiveService>& archive)
{
    return worker([=] {
        QImage image(160, 100, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, qRgb((x * 37 + y) % 256, (y * 17 + x) % 256, (x ^ y) % 256));
        const auto saved = archive->archiveImage(image); require(saved.durable(), "Create a fully validated real PNG archive");
        const auto listed = archive->list(); require(!listed.failure, "Read actual source history");
        const auto found = std::find_if(listed.entries.begin(), listed.entries.end(), [&](const auto& entry) { return entry.path == saved.path; });
        require(found != listed.entries.end(), "Saved PNG is in actual history"); return *found;
    });
}

struct Prepared {
    int status{HUHU_INVALID_STATE};
    std::vector<std::shared_ptr<const transfer::PreparedMedia>> files;
    std::vector<std::weak_ptr<const void>> history;
};

Prepared prepare(const std::shared_ptr<media::MediaArchiveService>& archive, const QVector<media::Entry>& entries)
{
    Prepared result;
    for (const auto& entry : entries) {
        auto lease = archive->validatedLease(entry);
        if (lease.failure || !lease.lease) return result;
        std::shared_ptr<const void> token = lease.lease; result.history.emplace_back(token);
        auto file = transfer::PreparedMedia::open(entry.path, entry.snapshot, HUHU_PNG, std::move(token));
        if (!file.media || file.status != HUHU_OK) { result.status = file.status; return result; }
        result.files.push_back(std::move(file.media));
    }
    result.status = HUHU_OK; return result;
}

// A concrete application-lifetime caller, not a fake transport. Its small
// admission queue can be held before dispatch so the test destroys the view
// before a real validation worker and real TLS sender take over the intent.
class SendHost final : public QObject {
public:
    SendHost(transfer::TransferService& service, std::shared_ptr<media::MediaArchiveService> archive)
        : service_(service), archive_(std::move(archive))
    {
        timer_.setInterval(1);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] {
            if (!future_.valid() || future_.wait_for(0ms) != std::future_status::ready) return;
            timer_.stop(); auto prepared = future_.get(); histories = std::move(prepared.history);
            last = prepared.status == HUHU_OK ? service_.send(QJsonDocument(peer_).toJson(QJsonDocument::Compact),
                std::move(prepared.files), alias_) : transfer::Submission{prepared.status, 0};
            peer_ = {}; alias_.clear();
            if (view_) view_->completePreparation(token_, last.status, last.request);
            finished = true;
        });
    }
    void bind(ui::TransferWindow& view)
    {
        QObject::connect(&view, &ui::TransferWindow::prepareSendRequested, this,
            [this, window = QPointer<ui::TransferWindow>(&view)](quint64 token, const QJsonObject& peer,
                const QString& alias, const QVector<media::Entry>& entries) {
                require(!queued && !future_.valid(), "One bounded fixture preparation at a time");
                view_ = window; token_ = token; peer_ = peer; alias_ = alias; entries_ = entries;
                queued = true; finished = false; ++admissions;
            });
    }
    void dispatch()
    {
        require(queued, "Only an explicit confirmed send can reach the application queue"); queued = false;
        future_ = std::async(std::launch::async, [archive = archive_, entries = std::exchange(entries_, {})] { return prepare(archive, entries); });
        timer_.start();
    }
    bool queued{}, finished{};
    unsigned admissions{};
    transfer::Submission last;
    std::vector<std::weak_ptr<const void>> histories;
    QVector<media::Entry> queuedEntries() const { return entries_; }
private:
    transfer::TransferService& service_;
    std::shared_ptr<media::MediaArchiveService> archive_;
    QTimer timer_;
    QPointer<ui::TransferWindow> view_;
    quint64 token_{};
    QJsonObject peer_;
    QString alias_;
    QVector<media::Entry> entries_;
    std::future<Prepared> future_;
};

void confirm(ui::TransferWindow& window, bool yes)
{
    click(*child<QPushButton>(window, "transferSend"));
    auto* box = child<QMessageBox>(window, "transferSendConfirmation");
    require(box->defaultButton() == box->button(QMessageBox::No), "Send confirmation defaults to No");
    click(*box->button(yes ? QMessageBox::Yes : QMessageBox::No));
}

QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read actual media bytes");
    const auto bytes = file.readAll(); require(file.error() == QFileDevice::NoError, "Read full media"); return bytes;
}

void contracts(const QString& pictures)
{
    QTemporaryDir temporary; require(temporary.isValid(), "Create real fixture directory");
    const auto senderArchive = std::make_shared<media::MediaArchiveService>(QDir(temporary.path()).filePath(QStringLiteral("sender/媒体")));
    const auto receiverArchive = std::make_shared<media::MediaArchiveService>(QDir(temporary.path()).filePath(QStringLiteral("receiver/媒体")));
    transfer::TransferService sender, receiver; Events sent(sender), received(receiver);
    localization::LanguageCatalog languages({localization::ResourcePolicy::LinuxUserResources, temporary.path(), temporary.path()});
    require(languages.load(QStringLiteral("en-US")).usable, "Load the real embedded English catalog");
    const ui::DesktopTranslator translate = [&languages](QStringView key) { return languages.text(key); };
    auto window = std::make_unique<ui::TransferWindow>(sender); window->setTranslator(translate); show(*window);
    ui::TransferSettingsPanel settings(sender); settings.setTranslator(translate); show(settings);
    require(!sender.statistics().nativeRunning && sender.state() == transfer::ServiceState::Disabled,
        "Opening both real views creates no runtime, listener or discovery");
    require(!child<QPushButton>(*window, "transferSend")->isEnabled() && !settings.requestApply(),
        "An unconnected configuration cannot invent an enabled service");
    const auto source = createPng(senderArchive);
    const auto senderConfiguration = identity("Sender GUI");
    const auto receiverConfiguration = identity("Receiver GUI");
    (void)enable(sender, sent, senderConfiguration, senderArchive);
    auto receiverInfo = enable(receiver, received, receiverConfiguration, receiverArchive);
    const auto receiverPort = static_cast<quint16>(receiverInfo.value(QStringLiteral("port")).toInt());
    const auto originalFingerprint = receiverInfo.value(QStringLiteral("fingerprint")).toString();
    {
        transfer::TransferService ordering; Events ordered(ordering);
        const auto configuration = identity("Publication order GUI");
        require(bool(ordering.enable(configuration, std::make_unique<transfer::ReceiveArchiveSink>(senderArchive))), "Admit ordering fixture listener");
        const auto deadline = std::chrono::steady_clock::now() + 25s;
        // Deliberately do not process GUI events: the real actor first publishes
        // its empty discovery snapshot, then a successful manual TLS terminal.
        while (ordering.state() != transfer::ServiceState::Enabled) {
            require(std::chrono::steady_clock::now() < deadline, "Ordering fixture did not start"); std::this_thread::sleep_for(1ms);
        }
        const auto probe = ordering.discover(QStringLiteral("127.0.0.1"), receiverPort);
        require(bool(probe), "Actual probe is admitted before any facade notification drains");
        while (ordering.statistics().queuedNotifications < 2) {
            require(std::chrono::steady_clock::now() < deadline, "Ordering probe did not finish"); std::this_thread::sleep_for(1ms);
        }
        const auto found = ordered.wait(transfer::EventKind::DiscoveryFinished, probe.request);
        require(found.status == HUHU_OK, "Ordering fixture uses a successful real TLS result");
        const auto state = ordering.viewSnapshot();
        require(state && state->devices && state->devices->sequence < found.sequence,
            "Publication order survives drain's terminal-before-coalesced-snapshot layout");
        ui::TransferWindow late(ordering); late.setTranslator(translate);
        require(late.deviceCount() == 1, "An older empty device snapshot cannot hide the later real manual probe");
        stop(ordering, ordered);
    }
    require(window->setMediaSelection({source}), "Select one actual archived screenshot");
    window->setArchiveDirectory(receiverArchive->directory());
    unsigned reviews{};
    auto bindReview = [&](ui::TransferWindow& view) {
        QObject::connect(&view, &ui::TransferWindow::peerReviewRequested, &view,
            [&view, &reviews, &originalFingerprint](quint64 token, const QJsonObject& peer) {
                ++reviews; require(!peer.value(QStringLiteral("fingerprint")).toString().isEmpty(), "Review the real discovered certificate");
                view.completePeerReview(token, ui::DesktopResult::Succeeded, originalFingerprint);
            });
    };
    bindReview(*window);
    SendHost host(sender, senderArchive); host.bind(*window);
    require(bool(window->discover(QStringLiteral("127.0.0.1"), receiverPort)), "Actual manual TLS discovery is admitted");
    waitFor([&] { return window->deviceCount() == 1 && child<QPushButton>(*window, "transferSend")->isEnabled(); }, "TLS discovery and host trust review complete");
    const auto selected = window->selectedEndpoint();
    require(reviews == 1, "One stable selection requests one trust review");
    window->refreshLanguage(); require(window->selectedEndpoint() == selected && reviews == 1, "Refresh preserves the selected device and its verified review");
    save(*window, pictures, QStringLiteral("lan-devices-en.png"));
    const auto confirmationStart = std::chrono::steady_clock::now();
    QJsonArray receiveStages;
    auto receiveStage = [&](const char* name) {
        receiveStages.append(QJsonObject{{QStringLiteral("stage"), QString::fromLatin1(name)},
            {QStringLiteral("elapsedMs"), static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - confirmationStart).count())}});
    };
    confirm(*window, true); require(host.queued && window->preparationPending(), "A real confirmation queues the application-owned intent");
    receiveStage("sendConfirmed");
    window->close(); window.reset();
    receiveStage("senderViewDestroyed");
    require(sender.statistics().nativeRunning && host.queued, "Closing and destroying the view keeps the confirmed intent and service alive");
    host.dispatch(); waitFor([&] { return host.finished; }, "Actual media preparation and service admission complete");
    receiveStage("sendAdmitted");
    require(bool(host.last), "The host submits actual prepared media after view destruction");
    const auto offer = received.wait(transfer::EventKind::ReceiveOffer);
    receiveStage("offerObserved");
    require(!host.histories.front().expired(), "The service retains the real source lease while confirmation is pending");
    require(senderArchive->reserveRemoval(source).failure.code == media::Error::Busy, "Active send protects original history data");
    receiveStage("sourceLeaseChecked");
    {
        ui::TransferWindow receiving(receiver); receiving.setTranslator(translate);
        receiveStage("taskViewConstructed"); show(receiving); receiveStage("taskViewShown");
        require(receiving.taskCount() == 1, "A late-created view restores the actual pending incoming task");
        receiving.close(); require(receiver.statistics().nativeRunning, "Closing the task view does not decline a receive");
    }
    receiveStage("taskViewDestroyed");
    auto prompt = std::make_unique<ui::ReceivePrompt>(receiver, offer, receiverArchive->directory());
    receiveStage("promptConstructed");
    prompt->setTranslator(translate); show(*prompt); receiveStage("promptShown");
    if (prompt->selectedFiles().size() != 1 || prompt->answered())
        diagnoseReceivePrompt(*prompt, offer, receiver, sender, host.last.request, receiveStages);
    require(prompt->selectedFiles().size() == 1 && !prompt->answered(), "Only real offered supported media is selectable");
    save(*prompt, pictures, QStringLiteral("lan-receive-en.png"));
    click(*child<QPushButton>(*prompt, "receiveAccept"));
    require(prompt->answered(), "Only the explicit accept action answers the session"); prompt.reset();
    const auto incoming = received.wait(transfer::EventKind::ReceiveFinished);
    const auto outgoing = sent.wait(transfer::EventKind::SendFinished, host.last.request);
    require(incoming.status == HUHU_OK && outgoing.status == HUHU_OK && incoming.receivedFiles.size() == 1,
        "Both ends require the real successful TLS/archive result");
    const auto actualPath = incoming.receivedFiles.front().path;
    require(incoming.receivedFiles.front().published && read(actualPath) == read(source.path), "Actual archived PNG bytes are preserved");
    require(host.histories.front().expired(), "Neither a closed view nor the read-only snapshot retains a media lease");
    const auto snapshot = sender.viewSnapshot(); require(snapshot.has_value(), "GUI snapshot is available");
    const auto queued = std::find_if(snapshot->events.begin(), snapshot->events.end(), [](const auto& event) { return event.kind == transfer::EventKind::SendQueued; });
    require(queued != snapshot->events.end() && queued->sendFiles.size() == 1 && queued->sendFiles.front().snapshot == source.snapshot,
        "Snapshot retains exact source identity for revalidation, not only a filename");
    require(!queued->data.value(QStringLiteral("peer")).toObject().contains(QStringLiteral("pin")), "Activity snapshots never retain protocol PINs");
    window = std::make_unique<ui::TransferWindow>(sender); window->setTranslator(translate); bindReview(*window); host.bind(*window);
    require(window->setMediaSelection({source}), "Select real media after reopening"); show(*window);
    require(window->taskCount() == 1, "Reopening restores the completed task even though no view consumed its terminal");
    {
        ui::TransferWindow results(receiver); results.setTranslator(translate); show(results);
        auto* activity = child<QTreeWidget>(results, "transferActivity"); activity->setCurrentItem(activity->topLevelItem(0)); settle();
        const auto* details = child<QListWidget>(results, "transferDetails");
        require(details->count() == 1 && details->item(0)->data(Qt::UserRole).toString() == actualPath,
            "The completed UI exposes the exact durable received path");
        QString revealed;
        QObject::connect(&results, &ui::TransferWindow::receivedFileRequested, &results, [&](const QString& path) { revealed = path; });
        click(*child<QPushButton>(results, "transferReveal")); require(revealed == actualPath, "Locate dispatches the actual received file");
        save(results, pictures, QStringLiteral("lan-completed-en.png"));
    }

    // Closing the dedicated decision prompt is an explicit refusal, unlike
    // closing a task window. The sender observes the real protocol 403.
    auto refused = worker([&] { return prepare(senderArchive, {source}); });
    const auto beforeRefusal = received.values.size();
    QJsonObject peer{{QStringLiteral("address"), QStringLiteral("127.0.0.1")}, {QStringLiteral("port"), receiverPort},
        {QStringLiteral("fingerprint"), originalFingerprint}};
    const auto refusedSend = sender.send(QJsonDocument(peer).toJson(QJsonDocument::Compact), std::move(refused.files), QStringLiteral("Receiver GUI"));
    require(bool(refusedSend), "Admit actual refusal-path send");
    const auto refusedOffer = received.wait(transfer::EventKind::ReceiveOffer, 0, beforeRefusal);
    {
        ui::ReceivePrompt refusing(receiver, refusedOffer, receiverArchive->directory()); refusing.setTranslator(translate); show(refusing);
        require(refusing.close() && refusing.answered(), "Closing the prompt submits an explicit refusal");
    }
    const auto refusal = sent.wait(transfer::EventKind::SendFinished, refusedSend.request);
    require(refusal.status == HUHU_CANCELLED && refusal.data.value(QStringLiteral("httpStatus")).toInt() == 403,
        "Real receiver refusal remains distinct from storage and protocol failures");
    received.wait(transfer::EventKind::ReceiveFinished, 0, beforeRefusal);
    require(worker([&] { return receiverArchive->list().entries.size(); }) == 1, "Declining creates no extra archive");
    waitFor([&] { return window->taskCount() == 2; }, "Live task view displays the real refusal");

    // A real multi-file offer proves checkbox selection and per-file partial
    // outcomes. Retry must prepare only the previously unaccepted item.
    const auto secondSource = createPng(senderArchive);
    auto multiple = worker([&] { return prepare(senderArchive, {source, secondSource}); });
    const auto beforePartial = received.values.size();
    const auto partialSend = sender.send(QJsonDocument(peer).toJson(QJsonDocument::Compact), std::move(multiple.files), QStringLiteral("Receiver GUI"));
    require(bool(partialSend), "Admit a real two-file offer");
    const auto multiOffer = received.wait(transfer::EventKind::ReceiveOffer, 0, beforePartial);
    {
        ui::ReceivePrompt selecting(receiver, multiOffer, receiverArchive->directory()); selecting.setTranslator(translate); show(selecting);
        auto* files = child<QTreeWidget>(selecting, "receiveFiles"); require(files->topLevelItemCount() == 2, "Render every actual offered file");
        for (int index = 0; index < files->topLevelItemCount(); ++index) {
            auto* item = files->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == QStringLiteral("1")) item->setCheckState(0, Qt::Unchecked);
        }
        require(selecting.selectedFiles() == QStringList{QStringLiteral("0")}, "The explicit partial decision retains protocol file IDs");
        require(bool(selecting.acceptSelected()), "Accept only the selected real file");
    }
    const auto partial = sent.wait(transfer::EventKind::SendFinished, partialSend.request);
    const auto partialReceipt = received.wait(transfer::EventKind::ReceiveFinished, 0, beforePartial);
    require(partial.status == HUHU_PARTIAL && partialReceipt.receivedFiles.size() == 1 && partialReceipt.receivedFiles.front().published,
        "Real partial acceptance is not displayed as all-files success");
    waitFor([&] { return window->taskCount() >= 3; }, "Retain the actual partial result");
    auto* activity = child<QTreeWidget>(*window, "transferActivity");
    for (int index = 0; index < activity->topLevelItemCount(); ++index) {
        auto* row = activity->topLevelItem(index);
        if (row->data(0, Qt::UserRole).toString().endsWith(QStringLiteral("/send/") + QString::number(static_cast<qulonglong>(partialSend.request))))
            activity->setCurrentItem(row);
    }
    settle(); click(*child<QPushButton>(*window, "transferRetry"));
    waitFor([&] { return window->findChild<QMessageBox*>(QStringLiteral("transferSendConfirmation")) != nullptr; },
        "Retry performs a new actual TLS discovery before confirmation");
    auto* retryConfirmation = child<QMessageBox>(*window, "transferSendConfirmation");
    click(*retryConfirmation->button(QMessageBox::Yes));
    require(host.queued && host.queuedEntries().size() == 1 && host.queuedEntries().front().snapshot == secondSource.snapshot,
        "Retry asks the host to revalidate only the unaccepted file identity");
    const auto beforeRetry = received.values.size(); host.dispatch();
    waitFor([&] { return host.finished; }, "Retry uses a new actual media validation worker");
    require(bool(host.last) && host.last.request != partialSend.request, "Retry is a newly admitted operation");
    const auto retryOffer = received.wait(transfer::EventKind::ReceiveOffer, 0, beforeRetry);
    {
        ui::ReceivePrompt retryReceipt(receiver, retryOffer, receiverArchive->directory()); retryReceipt.setTranslator(translate); show(retryReceipt);
        const auto offeredFiles = retryOffer.data.value(QStringLiteral("files")).toArray();
        require(offeredFiles.size() == 1 && offeredFiles.first().toObject().value(QStringLiteral("fileName")).toString() == secondSource.name,
            "Actual retry offers the unaccepted original bytes, not already saved files");
        require(bool(retryReceipt.reject()), "Explicitly finish the retry fixture by declining");
    }
    require(sent.wait(transfer::EventKind::SendFinished, host.last.request).status == HUHU_CANCELLED, "Retry has its own actual terminal");
    received.wait(transfer::EventKind::ReceiveFinished, 0, beforeRetry);
    require(host.histories.front().expired(), "Retry source lease is released at its real terminal");

    const auto retainedBefore = sender.viewSnapshot()->events.size();
    for (unsigned index = 0; index < 20; ++index) {
        const auto missing = sender.accept(QStringLiteral("missing-session-%1").arg(index), {});
        require(bool(missing), "Admit a real stale decision command");
        require(sent.wait(transfer::EventKind::CommandFinished, missing.request).status == HUHU_CLOSED,
            "A nonexistent session is rejected by the actual actor");
    }
    require(sender.viewSnapshot()->events.size() == retainedBefore, "Stale decisions cannot create immortal snapshot tasks");
    for (unsigned index = 0; index < 18; ++index) {
        const auto probe = sender.discover(QStringLiteral("127.0.0.1"), receiverPort);
        require(bool(probe), "Admit bounded actual probe history");
        require(sent.wait(transfer::EventKind::DiscoveryFinished, probe.request).status == HUHU_OK, "Repeated probes still use real TLS");
    }
    const auto retained = sender.viewSnapshot();
    const auto discoveryCount = std::count_if(retained->events.begin(), retained->events.end(), [](const auto& event) {
        return event.kind == transfer::EventKind::DiscoveryFinished;
    });
    require(discoveryCount <= static_cast<std::ptrdiff_t>(transfer::TransferLimits::maximumRecentDiscoveries), "Completed discovery retention is bounded");
    require(std::any_of(retained->events.begin(), retained->events.end(), [&](const auto& event) {
        return event.kind == transfer::EventKind::SendFinished && event.request == outgoing.request;
    }), "Repeated discovery cannot evict a transfer result from its separate retention budget");

    // The host is restarted with a genuinely different certificate on the
    // same endpoint. This is not a synthetic device/fingerprint notification.
    stop(receiver, received);
    const auto replacementIdentity = identity("Receiver GUI", receiverPort);
    receiverInfo = enable(receiver, received, replacementIdentity, receiverArchive);
    require(receiverInfo.value(QStringLiteral("fingerprint")).toString() != originalFingerprint, "Real replacement TLS identity differs");
    require(bool(window->discover(QStringLiteral("127.0.0.1"), receiverPort)), "Re-probe the actual changed certificate");
    waitFor([&] { return child<QLabel>(*window, "transferTrust")->text() == languages.text(u"lan.changedIdentity"); }, "UI explicitly warns about a changed certificate");
    const auto previousAdmissions = host.admissions;
    click(*child<QPushButton>(*window, "transferSend"));
    auto* changed = child<QMessageBox>(*window, "transferSendConfirmation");
    require(changed->text().contains(originalFingerprint) && changed->text().contains(receiverInfo.value(QStringLiteral("fingerprint")).toString()),
        "Identity-change confirmation shows both actual fingerprints");
    save(*changed, pictures, QStringLiteral("lan-changed-identity-en.png"));
    click(*changed->button(QMessageBox::No));
    require(host.admissions == previousAdmissions && !host.queued, "Rejecting a changed identity starts no send");

    // Configuration remains explicit. The real loopback adapter is supplied
    // by the host fixture; the UI does not invent an interface or wildcard.
    ui::LanSettingsSnapshot effective;
    effective.enabled = true; effective.canConfigure = true; effective.alias = QStringLiteral("Sender GUI");
    effective.port = static_cast<quint16>(sent.wait(transfer::EventKind::Enabled).data.value(QStringLiteral("port")).toInt());
    effective.fingerprint = sent.wait(transfer::EventKind::Enabled).data.value(QStringLiteral("fingerprint")).toString();
    effective.archiveDirectory = senderArchive->directory();
    for (const auto& adapter : QNetworkInterface::allInterfaces())
        for (const auto& address : adapter.addressEntries()) if (address.ip() == QHostAddress::LocalHost)
            effective.interfaces.append({adapter.humanReadableName(), address.ip().toString(), true, true, false});
    require(!effective.interfaces.isEmpty(), "Use a genuinely enumerated local network interface");
    settings.setSnapshot(effective); show(settings);
    for (const auto* code : {"en-US", "zh-CN", "zh-TW", "ja-JP", "ru-RU", "id-ID"}) {
        require(languages.load(QString::fromLatin1(code)).usable, "Load each real embedded language catalog");
        settings.refreshLanguage(); window->refreshLanguage(); settle();
        require(settings.windowTitle() == languages.text(u"lan.settings") && !settings.windowTitle().startsWith(QStringLiteral("lan.")),
            "All six maintained languages resolve actual LAN strings");
        save(settings, pictures, QStringLiteral("lan-settings-%1.png").arg(QString::fromLatin1(code)));
    }
    quint64 changing{};
    QObject::connect(&settings, &ui::TransferSettingsPanel::changeRequested, &settings,
        [&](quint64 token, const ui::LanSettingsChange& change) {
            require(!change.enabled && change.addresses.contains(QStringLiteral("127.0.0.1")), "User disable preserves the selected explicit address");
            changing = token; require(sender.disable() == HUHU_OK, "Host requests real asynchronous shutdown");
        });
    click(*child<QCheckBox>(settings, "lanEnable")); click(*child<QPushButton>(settings, "lanApply"));
    require(changing && settings.changePending(), "UI waits for the actual settings/service result");
    settings.close();
    waitFor([&] { return sender.state() == transfer::ServiceState::Disabled && !sender.statistics().nativeRunning; }, "Closing settings does not interrupt the requested service shutdown");
    effective.enabled = false; settings.completeChange(changing, ui::DesktopResult::Succeeded, effective);
    require(!settings.changePending(), "Only the concrete host result completes the settings operation");
    window.reset(); stop(receiver, received);
    require(!sender.statistics().nativeRunning && !receiver.statistics().nativeRunning, "All actual network runtimes are released");
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    try {
        const auto directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        if (!directory.isEmpty()) require(QDir().mkpath(directory), "Create visual evidence directory");
        contracts(directory);
        std::cout << "Real Qt LAN UI, TLS identity change, archive, refusal and view lifetime contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Transfer UI validation failed: " << error.what() << '\n'; return 1;
    }
}
