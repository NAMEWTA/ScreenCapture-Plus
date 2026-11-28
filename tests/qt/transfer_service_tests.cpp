#include "services/transfer/ReceiveArchiveSink.h"
#include "services/transfer/TransferService.h"
#include "services/media/GifInspection.h"
#include "services/files/QtPaths.h"
#include "gifski.h"
#ifdef HUHUCAPTURE_TRANSFER_ALL_MEDIA
#include "services/media/VideoInspection.h"
#include <QCryptographicHash>
#endif

#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace transfer = HuhuCapture::Transfer;
namespace media = HuhuCapture::Media;
namespace files = HuhuCapture::Files;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void waitFor(Predicate predicate, const char* message, bool processGui = true)
{
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        if (processGui) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
}

template<class Work>
auto onWorker(Work work)
{
    auto future = std::async(std::launch::async, std::move(work));
    waitFor([&] { return future.wait_for(0ms) == std::future_status::ready; }, "Fixture worker timed out");
    return future.get();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Read actual completed media");
    const auto result = file.readAll();
    require(file.error() == QFileDevice::NoError, "Read complete source bytes");
    return result;
}

QByteArray identity(const char* alias)
{
    return onWorker([alias] {
        HuhuBuffer* raw{};
        require(huhu_identity_generate(&raw) == HUHU_OK, "Generate real official LocalSend TLS identity");
        std::unique_ptr<HuhuBuffer, decltype(&huhu_buffer_free)> owner(raw, &huhu_buffer_free);
        HuhuSlice view{};
        require(huhu_buffer_view(raw, &view) == HUHU_OK && view.length < 1024 * 1024, "Read generated identity");
        auto configuration = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(view.data),
            static_cast<qsizetype>(view.length))).object();
        require(configuration.contains(QStringLiteral("privateKeyPem")), "Identity contains the real private key");
        configuration.insert(QStringLiteral("alias"), QString::fromUtf8(alias));
        configuration.insert(QStringLiteral("addresses"), QJsonArray{QStringLiteral("127.0.0.1")});
        configuration.insert(QStringLiteral("port"), 0);
        configuration.insert(QStringLiteral("discovery"), false);
        configuration.insert(QStringLiteral("decisionTimeoutMs"), 15000);
        configuration.insert(QStringLiteral("idleTimeoutMs"), 15000);
        configuration.insert(QStringLiteral("commitTimeoutMs"), 15000);
        return QJsonDocument(configuration).toJson(QJsonDocument::Compact);
    });
}

media::Inspection inspectOther(const QString& path, media::Kind kind, std::stop_token stop)
{
#ifdef HUHUCAPTURE_TRANSFER_ALL_MEDIA
    if (kind == media::Kind::Mp4) return media::inspectVideo(path, {}, stop);
#endif
    require(kind == media::Kind::Gif, "Only a registered complete media decoder may inspect this transfer");
    return media::inspectGif(path, {}, stop);
}

std::shared_ptr<media::MediaArchiveService> archive(const QString& root,
    media::FullMediaInspector inspector = inspectOther)
{
    return std::make_shared<media::MediaArchiveService>(root, std::move(inspector));
}

media::Entry entryFor(const std::shared_ptr<media::MediaArchiveService>& archive, const QString& path)
{
    return onWorker([=] {
        const auto listed = archive->list();
        require(!listed.failure, "List actual managed history");
        const auto entry = std::find_if(listed.entries.begin(), listed.entries.end(), [&](const auto& item) {
            return item.path == path;
        });
        require(entry != listed.entries.end(), "Durable media must appear in shared history");
        return *entry;
    });
}

media::Entry createPng(const std::shared_ptr<media::MediaArchiveService>& archive, QRgb color)
{
    const auto result = onWorker([=] {
        QImage image(32, 24, QImage::Format_ARGB32);
        image.fill(color);
        return archive->archiveImage(image);
    });
    require(result.durable(), "Create and strictly validate a real archived PNG");
    return entryFor(archive, result.path);
}

media::Entry createGif(const std::shared_ptr<media::MediaArchiveService>& archive, const QString& path)
{
    const auto result = onWorker([=] {
        const GifskiSettings settings{32, 24, 90, true, 0};
        auto release = [](gifski* value) { if (value) (void)gifski_finish(value); };
        std::unique_ptr<gifski, decltype(release)> encoder(gifski_new(&settings), release);
        require(encoder != nullptr, "Create actual aggregate gifski encoder");
        const auto encoded = path.toUtf8();
        require(gifski_set_file_output(encoder.get(), encoded.constData()) == GIFSKI_OK, "Open real GIF output");
        const std::array<QRgb, 3> colors{qRgb(240, 20, 10), qRgb(10, 240, 20), qRgb(20, 10, 240)};
        for (std::uint32_t index = 0; index < colors.size(); ++index) {
            QImage image(32, 24, QImage::Format_RGBA8888);
            image.fill(QColor::fromRgb(colors[index]));
            require(gifski_add_frame_rgba_stride(encoder.get(), index, 32, 24,
                static_cast<std::uint32_t>(image.bytesPerLine()), image.constBits(), index / 16.0) == GIFSKI_OK,
                "Encode original Qt pixels through the official gifski ABI");
        }
        require(gifski_finish(encoder.release()) == GIFSKI_OK, "Finalize real GIF");
        return archive->importFile(path, media::Kind::Gif);
    });
    require(result.durable(), "Full GIF decoder must authorize source archival");
    return entryFor(archive, result.path);
}


#ifdef HUHUCAPTURE_TRANSFER_ALL_MEDIA
media::Entry importMp4(const std::shared_ptr<media::MediaArchiveService>& archive, const QString& fixtureRoot)
{
    const auto path = QDir(fixtureRoot).filePath(QStringLiteral("valid.mp4"));
    const auto result = onWorker([=] {
        const auto manifest = QJsonDocument::fromJson(readFile(QDir(fixtureRoot).filePath(QStringLiteral("manifest.json")))).object();
        require(manifest.value(QStringLiteral("schema")).toInt() == 1 &&
            manifest.value(QStringLiteral("ffmpegCommit")).toString() == QStringLiteral("946fcce07b6dcd0331c8cc609192aeff5e1924f8"),
            "Reuse the pinned real H.264/AAC encoder fixture and its provenance");
        const auto cases = manifest.value(QStringLiteral("cases")).toArray();
        const auto selected = std::find_if(cases.cbegin(), cases.cend(), [](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("file")).toString() == QStringLiteral("valid.mp4");
        });
        require(selected != cases.cend() && selected->toObject().value(QStringLiteral("accept")).toBool(),
            "The existing real video fixture must declare complete valid media");
        QFile input(path); QCryptographicHash hash(QCryptographicHash::Sha256);
        require(input.open(QIODevice::ReadOnly) && hash.addData(&input) &&
            QString::fromLatin1(hash.result().toHex()) == selected->toObject().value(QStringLiteral("sha256")).toString(),
            "Transfer only the exact real MP4 bytes declared by the shared encoder fixture");
        input.close();
        return archive->importFile(path, media::Kind::Mp4);
    });
    require(result.durable() && result.media.frames == 251, "The full MP4 decoder authorizes all original video frames before archival");
    return entryFor(archive, result.path);
}
#endif

struct Source {
    std::shared_ptr<const transfer::PreparedMedia> prepared;
    std::weak_ptr<const void> historyLifetime;
};

Source prepare(const std::shared_ptr<media::MediaArchiveService>& archive, const media::Entry& entry)
{
    return onWorker([=] {
        auto lease = archive->validatedLease(entry);
        require(!lease.failure && lease.lease, "Sender first validates the complete selected archive");
        std::shared_ptr<const void> token = lease.lease;
        std::weak_ptr<const void> lifetime = token;
        const auto kind = entry.kind == media::Kind::Png ? HUHU_PNG : entry.kind == media::Kind::Gif ? HUHU_GIF : HUHU_MP4;
        auto prepared = transfer::PreparedMedia::open(entry.path, entry.snapshot, kind, std::move(token));
        require(prepared.status == HUHU_OK && prepared.media, "Prepare exact opened identity and native lease");
        return Source{std::move(prepared.media), std::move(lifetime)};
    });
}

class Events {
public:
    explicit Events(transfer::TransferService& service)
    {
        connection_ = QObject::connect(&service, &transfer::TransferService::eventReady, &service,
            [this](const transfer::Event& event) {
                wrongThread = wrongThread || QThread::currentThread() != QCoreApplication::instance()->thread();
                values.push_back(event);
            });
    }
    ~Events() { QObject::disconnect(connection_); }
    std::vector<transfer::Event> values;
    bool wrongThread{};
    std::optional<transfer::Event> find(transfer::EventKind kind, transfer::RequestId request = 0,
        std::size_t begin = 0, const QString& session = {}) const
    {
        for (auto index = begin; index < values.size(); ++index) {
            const auto& event = values[index];
            if (event.kind == kind && (!request || event.request == request) &&
                (session.isEmpty() || event.data.value(QStringLiteral("sessionId")).toString() == session)) return event;
        }
        return {};
    }
    transfer::Event wait(transfer::EventKind kind, transfer::RequestId request = 0,
        std::size_t begin = 0, const QString& session = {})
    {
        waitFor([&] { return find(kind, request, begin, session).has_value(); }, "Expected transfer service event did not arrive");
        return *find(kind, request, begin, session);
    }
private:
    QMetaObject::Connection connection_;
};

struct Running {
    QJsonObject info;
    std::uint64_t generation{};
    transfer::ReceiveArchiveSink* sink{}; // Valid only until disable/shutdown.
};

Running enable(transfer::TransferService& service, Events& events, const QByteArray& configuration,
    const std::shared_ptr<media::MediaArchiveService>& archive, transfer::ReceiveArchiveLimits limits = {})
{
    auto sink = std::make_unique<transfer::ReceiveArchiveSink>(archive, limits);
    const auto pointer = sink.get();
    const auto submitted = service.enable(configuration, std::move(sink));
    require(bool(submitted), "Admit explicit LAN enable");
    const auto enabled = events.wait(transfer::EventKind::Enabled, submitted.request);
    require(enabled.status == HUHU_OK && enabled.data.value(QStringLiteral("port")).toInt() > 0,
        "Actually start the TLS listener on an ephemeral loopback port");
    return {enabled.data, enabled.generation, pointer};
}

QByteArray peer(const Running& receiver)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("address"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("port"), receiver.info.value(QStringLiteral("port"))},
        {QStringLiteral("fingerprint"), receiver.info.value(QStringLiteral("fingerprint"))}}).toJson(QJsonDocument::Compact);
}

QString sessionOf(const transfer::Event& offer)
{
    const auto session = offer.data.value(QStringLiteral("sessionId")).toString();
    require(!session.isEmpty(), "Receive offer contains a real session identity");
    return session;
}

QString fileOf(const transfer::Event& offer)
{
    const auto files = offer.data.value(QStringLiteral("files")).toArray();
    require(files.size() == 1, "Single-history-item UI flow offers one actual file");
    const auto id = files.first().toObject().value(QStringLiteral("id")).toString();
    require(!id.isEmpty(), "Offered file has a real protocol identity");
    return id;
}

void stop(transfer::TransferService& service, Events& events)
{
    const auto begin = events.values.size();
    require(service.disable() == HUHU_OK, "Admit priority asynchronous disable");
    const auto ended = events.wait(transfer::EventKind::Disabled, 0, begin);
    require(ended.status == HUHU_OK, "All native/sink workers must join successfully");
    require(service.state() == transfer::ServiceState::Disabled, "Service is actually disabled");
    const auto statistics = service.statistics();
    require(!statistics.nativeRunning && statistics.reservedNotifications == 0 && statistics.reservedBytes == 0,
        "Stopped service retains no runtime or terminal reservations");
}

void checkBound(const transfer::TransferService& service)
{
    const auto statistics = service.statistics();
    require(statistics.queuedCommands <= transfer::TransferLimits::maximumQueuedCommands,
        "Command admission must remain bounded");
    require(statistics.queuedNotifications + statistics.reservedNotifications <= transfer::TransferLimits::maximumNotifications,
        "Every admitted terminal fits its reserved count");
    require(statistics.notificationBytes + statistics.reservedBytes <= transfer::TransferLimits::maximumNotificationBytes,
        "Pending values and future terminals fit the byte budget");
}

void successfulTransfer(transfer::TransferService& sender, Events& senderEvents,
    transfer::TransferService& receiver, Events& receiverEvents, const Running& receiving,
    const std::shared_ptr<media::MediaArchiveService>& sourceArchive, const media::Entry& entry,
    const std::shared_ptr<media::MediaArchiveService>& destination)
{
    auto source = prepare(sourceArchive, entry);
    const auto receiveBegin = receiverEvents.values.size();
    auto view = std::make_unique<QObject>();
    unsigned viewEvents{};
    QObject::connect(&sender, &transfer::TransferService::eventReady, view.get(),
        [&](const transfer::Event&) { ++viewEvents; });
    const auto sent = sender.send(peer(receiving), {std::move(source.prepared)});
    require(bool(sent), "Admit actual send with stable source leases");
    const auto offer = receiverEvents.wait(transfer::EventKind::ReceiveOffer, 0, receiveBegin);
    require(!source.historyLifetime.expired(), "Source history lease survives while receiver confirmation is pending");
    require(sourceArchive->reserveRemoval(entry).failure.code == media::Error::Busy,
        "Application history cleanup must not claim a sending source");
    view.reset();
    const auto subscribedCount = viewEvents;
    require(bool(receiver.accept(sessionOf(offer), {fileOf(offer)})), "Accept the offered history item explicitly");
    const auto received = receiverEvents.wait(transfer::EventKind::ReceiveFinished, 0, receiveBegin, sessionOf(offer));
    const auto finished = senderEvents.wait(transfer::EventKind::SendFinished, sent.request);
    require(finished.status == HUHU_OK && received.status == HUHU_OK, "Both ends require actual durable success");
    require(viewEvents == subscribedCount, "Closing the view unsubscribes without cancelling the transfer");
    require(received.receivedFiles.size() == 1 && received.receivedFiles.front().published &&
        received.receivedFiles.front().status == HUHU_OK, "Receive terminal contains the exact successful local publication");
    const auto& saved = received.receivedFiles.front();
    require(saved.attempt != 0 && !saved.path.isEmpty(), "The worker's real attempt and path survive the actor handoff");
    require(readFile(saved.path) == readFile(entry.path), "Transfer preserves original media bytes without re-encoding");
    require(entryFor(destination, saved.path).kind == entry.kind, "Only the strictly decoded committed media enters history");
    require(source.historyLifetime.expired(), "Release the history lease only after the real native SEND_END");
    const auto storage = receiving.sink->statistics();
    require(storage.activeFiles == 0 && storage.queuedFiles == 0 && storage.pendingResults == 0 && storage.reservedBytes == 0,
        "Finished receipt releases body workers, result slots and future disk reservation");
    checkBound(sender); checkBound(receiver);
}

// A deterministic scheduling gate around the REAL strict GIF inspector. It
// produces no fabricated media result: cancellation wakes the worker via its
// production stop_token, then the existing inspector returns the real status.
// This tests cancellation after actual TLS EOF/hash verification, before codec
// dispatch; it does not claim to benchmark or interrupt a particular LZW step.
struct DecodeGate {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::atomic<bool> entered{};
    media::Inspection inspect(const QString& path, media::Kind kind, std::stop_token stop)
    {
        {
            std::unique_lock lock(mutex);
            entered.store(true, std::memory_order_release);
            condition.wait(lock, stop, [] { return false; });
        }
        return inspectOther(path, kind, stop);
    }
};

void run([[maybe_unused]] const QString& videoFixtureRoot)
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "Create owned isolated filesystem for service integration");
    const auto sourceArchive = archive(QDir(temporary.path()).filePath(QString::fromUtf8("发送历史")));
    const auto destination = archive(QDir(temporary.path()).filePath(QString::fromUtf8("接收历史")));
    const auto png = createPng(sourceArchive, qRgb(20, 100, 230));
    const auto gif = createGif(sourceArchive, QDir(temporary.path()).filePath(QString::fromUtf8("完整动画.gif")));
#ifdef HUHUCAPTURE_TRANSFER_ALL_MEDIA
    const auto mp4 = importMp4(sourceArchive, videoFixtureRoot);
#endif
    const auto existing = createPng(destination, qRgb(230, 100, 20));
    const auto originalExisting = readFile(existing.path);
    const auto senderConfig = identity("huhucapture service sender");
    const auto receiverConfig = identity("huhucapture service receiver");

    // Exact integer identity mismatch is rejected even with the selected
    // archive's valid logical lease. No pathname/JSON rounding can authorize it.
    onWorker([&] {
        const auto lease = sourceArchive->validatedLease(png);
        require(!lease.failure, "Acquire actual validated history lease");
        auto different = png.snapshot;
        different.identity.file ^= std::uint64_t{1} << 63;
        require(transfer::PreparedMedia::open(png.path, different, HUHU_PNG, lease.lease).status == HUHU_INVALID_STATE,
            "Compare all 64 bits of the already-open selected file identity");
    });

    transfer::TransferService sender, receiver;
    Events senderEvents(sender), receiverEvents(receiver);
    require(sender.state() == transfer::ServiceState::Disabled && !sender.statistics().nativeRunning,
        "LAN is cold and disabled before explicit enable");
    auto sending = enable(sender, senderEvents, senderConfig, sourceArchive);
    const auto receiving = enable(receiver, receiverEvents, receiverConfig, destination);
    const auto discovery = sender.discover(QStringLiteral("127.0.0.1"),
        static_cast<std::uint16_t>(receiving.info.value(QStringLiteral("port")).toInt()));
    require(bool(discovery), "Admit manual endpoint discovery");
    const auto discovered = senderEvents.wait(transfer::EventKind::DiscoveryFinished, discovery.request);
    require(discovered.status == HUHU_OK && discovered.data.value(QStringLiteral("fingerprint")) ==
        receiving.info.value(QStringLiteral("fingerprint")), "Manual discovery verifies the actual peer TLS identity");
    successfulTransfer(sender, senderEvents, receiver, receiverEvents, receiving, sourceArchive, png, destination);
    successfulTransfer(sender, senderEvents, receiver, receiverEvents, receiving, sourceArchive, gif, destination);
#ifdef HUHUCAPTURE_TRANSFER_ALL_MEDIA
    successfulTransfer(sender, senderEvents, receiver, receiverEvents, receiving, sourceArchive, mp4, destination);
#endif
    require(readFile(existing.path) == originalExisting, "Import never replaces an existing archive entry");

    const auto receiveBeforeMismatch = receiverEvents.values.size();
    auto wrongPeer = QJsonDocument::fromJson(peer(receiving)).object();
    auto fingerprint = wrongPeer.value(QStringLiteral("fingerprint")).toString();
    fingerprint[0] = fingerprint.front() == QChar(u'0') ? QChar(u'1') : QChar(u'0');
    wrongPeer.insert(QStringLiteral("fingerprint"), fingerprint);
    auto mismatched = prepare(sourceArchive, png);
    const auto bad = sender.send(QJsonDocument(wrongPeer).toJson(QJsonDocument::Compact), {std::move(mismatched.prepared)});
    require(bool(bad), "Admit real TLS pin mismatch attempt");
    require(senderEvents.wait(transfer::EventKind::SendFinished, bad.request).status == HUHU_PROTOCOL,
        "A changed fingerprint fails before transmitting an offer/body");
    require(!receiverEvents.find(transfer::EventKind::ReceiveOffer, 0, receiveBeforeMismatch),
        "Unpinned sender must not expose media metadata to the receiver");
    require(mismatched.historyLifetime.expired(), "TLS failure releases the source lease at its terminal");

    auto declined = prepare(sourceArchive, png);
    auto begin = receiverEvents.values.size();
    const auto rejected = sender.send(peer(receiving), {std::move(declined.prepared)});
    require(bool(rejected), "Admit transfer for explicit user decline");
    const auto declinedOffer = receiverEvents.wait(transfer::EventKind::ReceiveOffer, 0, begin);
    require(bool(receiver.accept(sessionOf(declinedOffer), {})), "User explicitly declines without body admission");
    const auto declinedEnd = receiverEvents.wait(transfer::EventKind::ReceiveFinished, 0, begin, sessionOf(declinedOffer));
    const auto denied = senderEvents.wait(transfer::EventKind::SendFinished, rejected.request);
    require(declinedEnd.status == HUHU_CANCELLED && declinedEnd.receivedFiles.empty() &&
        denied.status == HUHU_CANCELLED && denied.data.value(QStringLiteral("httpStatus")).toInt() == 403,
        "A user decline is a distinct actual 403 terminal");

    // Fresh sink policy, same real TLS configuration: no artificial transport
    // or filesystem is installed for these pre-body admission failures.
    for (const bool storageFailure : {false, true}) {
        transfer::TransferService limited;
        Events limitedEvents(limited);
        transfer::ReceiveArchiveLimits policy;
        if (storageFailure) policy.minimumFreeBytes = std::numeric_limits<std::uint64_t>::max();
        else policy.maximumFileBytes = png.snapshot.bytes - 1;
        const auto refusedArchive = archive(QDir(temporary.path()).filePath(storageFailure ? "space-policy" : "size-policy"));
        const auto limitedRunning = enable(limited, limitedEvents, receiverConfig, refusedArchive, policy);
        auto source = prepare(sourceArchive, png);
        const auto sent = sender.send(peer(limitedRunning), {std::move(source.prepared)});
        require(bool(sent), "Admit transfer to real bounded receiver");
        const auto offer = limitedEvents.wait(transfer::EventKind::ReceiveOffer);
        const auto accepted = limited.accept(sessionOf(offer), {fileOf(offer)});
        require(bool(accepted), "Run actual receiver policy/disk reservation before accepting");
        const auto expected = storageFailure ? HUHU_IO : HUHU_LIMIT_EXCEEDED;
        require(limitedEvents.wait(transfer::EventKind::CommandFinished, accepted.request).status == expected,
            "Reserve must expose its actual policy or storage failure");
        const auto receiveEnd = limitedEvents.wait(transfer::EventKind::ReceiveFinished, 0, 0, sessionOf(offer));
        const auto sendEnd = senderEvents.wait(transfer::EventKind::SendFinished, sent.request);
        require(receiveEnd.status == expected && receiveEnd.receivedFiles.empty() && sendEnd.status == expected &&
            sendEnd.data.value(QStringLiteral("httpStatus")).toInt() == (storageFailure ? 507 : 413),
            "Storage and byte budget refusal must not masquerade as user decline");
        require(refusedArchive->list().entries.empty() && limitedRunning.sink->statistics().reservedBytes == 0,
            "Pre-body refusal creates no archive and leaves no disk reservation");
        stop(limited, limitedEvents);
    }

    // Actual port conflict fails enable and joins the freshly created sink.
    QTcpServer occupied;
    require(occupied.listen(QHostAddress::LocalHost, 0), "Hold a real loopback listening port");
    auto conflictConfig = QJsonDocument::fromJson(receiverConfig).object();
    conflictConfig.insert(QStringLiteral("port"), occupied.serverPort());
    transfer::TransferService conflicted;
    Events conflictEvents(conflicted);
    const auto conflict = conflicted.enable(QJsonDocument(conflictConfig).toJson(QJsonDocument::Compact),
        std::make_unique<transfer::ReceiveArchiveSink>(destination));
    require(bool(conflict), "Queue port-conflict startup outside the GUI");
    require(conflictEvents.wait(transfer::EventKind::Enabled, conflict.request).status == HUHU_ADDRESS_IN_USE &&
        conflictEvents.wait(transfer::EventKind::Disabled, conflict.request).status == HUHU_ADDRESS_IN_USE,
        "A real bind failure is neither enabled nor a clean successful startup");
    require(!conflicted.statistics().nativeRunning && occupied.isListening(), "Failed enable leaves the existing port owner intact");
    occupied.close();

    const auto previousGeneration = sending.generation;
    stop(sender, senderEvents);
    sending = enable(sender, senderEvents, senderConfig, sourceArchive);
    require(sending.generation > previousGeneration, "Fresh enable cannot reuse a stale GUI generation");
    successfulTransfer(sender, senderEvents, receiver, receiverEvents, receiving, sourceArchive, png, destination);

    // The real body is read and hash-verified by production ReceiveArchiveSink.
    // Hold only the pre-decoder scheduling boundary, then cancel without pumping
    // a single GUI event. The actor must wake/join the stop-token-aware worker.
    auto gate = std::make_shared<DecodeGate>();
    const auto gatedArchive = archive(QDir(temporary.path()).filePath("cancelled-receive"),
        [gate](const QString& path, media::Kind kind, std::stop_token token) { return gate->inspect(path, kind, token); });
    transfer::TransferService cancelling;
    Events cancellingEvents(cancelling);
    const auto cancellingRunning = enable(cancelling, cancellingEvents, receiverConfig, gatedArchive);
    auto cancelledSource = prepare(sourceArchive, gif);
    const auto cancelledSend = sender.send(peer(cancellingRunning), {std::move(cancelledSource.prepared)});
    require(bool(cancelledSend), "Admit actual GIF for cancellation after verification");
    const auto cancelOffer = cancellingEvents.wait(transfer::EventKind::ReceiveOffer);
    require(bool(cancelling.accept(sessionOf(cancelOffer), {fileOf(cancelOffer)})), "Accept real GIF body before cancellation");
    waitFor([&] { return gate->entered.load(std::memory_order_acquire); }, "Receiver did not reach its complete codec boundary");
    waitFor([&] {
        return std::any_of(cancellingEvents.values.begin(), cancellingEvents.values.end(), [&](const auto& event) {
            return event.kind == transfer::EventKind::ReceiveProgress && event.receiveProgress &&
                event.receiveProgress->sessionId == sessionOf(cancelOffer) && event.receiveProgress->fileId == fileOf(cancelOffer) &&
                event.receiveProgress->attempt != 0 && event.receiveProgress->bytes == gif.snapshot.bytes &&
                event.receiveProgress->total == gif.snapshot.bytes;
        });
    }, "Receiver must report actual complete written bytes through its own typed progress snapshot");
    require(!senderEvents.find(transfer::EventKind::SendFinished, cancelledSend.request),
        "Uploaded/verified bytes cannot ACK before full decode and durable archive");
    require(bool(cancelling.cancelReceive(sessionOf(cancelOffer))), "Queue explicit receive cancellation");
    waitFor([&] { return cancelledSource.historyLifetime.expired(); },
        "Receive cancellation must complete without GUI callbacks", false);
    cancelling.shutdownAndJoin(); // Still without GUI event dispatch.
    require(!cancelling.statistics().nativeRunning, "Join barrier releases the actual runtime and receiver workers");
    const auto cancelledReceive = cancellingEvents.wait(transfer::EventKind::ReceiveFinished, 0, 0, sessionOf(cancelOffer));
    require(cancelledReceive.status == HUHU_CANCELLED && cancelledReceive.receivedFiles.size() == 1 &&
        !cancelledReceive.receivedFiles.front().published && cancelledReceive.receivedFiles.front().path.isEmpty(),
        "Cancellation terminal includes the actual unfinished local outcome without an archive");
    require(senderEvents.wait(transfer::EventKind::SendFinished, cancelledSend.request).status == HUHU_CANCELLED,
        "Explicit receiver cancellation reaches the sender through actual TLS");
    require(cancellingEvents.wait(transfer::EventKind::Disabled).status == HUHU_OK,
        "Cancelled operations can still close the service cleanly after their terminals");
    require(gatedArchive->list().entries.empty() && QDir(gatedArchive->directory()).entryList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(), "Cancelled receive removes only its own staging file");

    // Stall the GUI while saturating real command/terminal budgets. Priority
    // stop must not depend on making space through a Qt callback.
    std::set<transfer::RequestId> admitted;
    unsigned busy{};
    for (unsigned index = 0; index < 256; ++index) {
        const auto request = sender.cancelSend(std::numeric_limits<transfer::RequestId>::max() - index);
        if (request) admitted.insert(request.request);
        else { require(request.status == HUHU_BUSY, "Full notification budget must reject new work explicitly"); ++busy; }
        checkBound(sender);
    }
    require(busy && !admitted.empty(), "Actual bounded actor queue applies admission backpressure");
    begin = senderEvents.values.size();
    require(sender.disable() == HUHU_OK, "Stop bypasses a saturated command/notification budget");
    sender.shutdownAndJoin();
    const auto stopped = senderEvents.wait(transfer::EventKind::Disabled, 0, begin);
    require(stopped.status == HUHU_OK && sender.state() == transfer::ServiceState::Closed,
        "Saturated service joins without a GUI drain and remains permanently closed");
    for (const auto id : admitted) {
        const auto count = std::count_if(senderEvents.values.begin(), senderEvents.values.end(), [id](const auto& event) {
            return event.kind == transfer::EventKind::CommandFinished && event.request == id;
        });
        require(count == 1, "Every admitted command has exactly one reliable terminal, even at shutdown");
    }
    checkBound(sender);
    stop(receiver, receiverEvents);
    require(!senderEvents.wrongThread && !receiverEvents.wrongThread && !cancellingEvents.wrongThread,
        "Only immutable values are delivered on the Qt owner thread");
    require(readFile(existing.path) == originalExisting && !readFile(png.path).isEmpty() && !readFile(gif.path).isEmpty(),
        "All failures, retries and shutdowns retain existing source and destination media");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
#ifdef HUHUCAPTURE_TRANSFER_ALL_MEDIA
        require(argc == 2, "Supply the existing real MP4 fixture directory for the all-media integration");
        run(QString::fromLocal8Bit(argv[1]));
        std::cout << "production TransferService/ReceiveArchiveSink: real TLS, PNG/GIF/MP4 full-decoder round trips, pinned identity, refusal, leases, bounded events and joined cancellation passed\n";
#else
        run({});
        std::cout << "production TransferService/ReceiveArchiveSink: real TLS, PNG/GIF, pinned identity, refusal, leases, bounded events and joined cancellation passed\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transfer service integration failed: " << error.what() << '\n';
        return 1;
    }
}
