#include "platform/FileCommitter.h"
#include "huhucapture_native.h"
#include "gifski.h"

#include <QCoreApplication>
#include <QColor>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checked(int status, const char* operation)
{
    if (status != HUHU_OK)
        throw std::runtime_error(std::string(operation) + ": native status " + std::to_string(status));
}

const std::uint8_t* bytes(const QByteArray& value)
{
    return reinterpret_cast<const std::uint8_t*>(value.constData());
}

std::filesystem::path nativePath(const QString& value)
{
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toUtf8().constData());
#endif
}

QString qtPath(const std::filesystem::path& value)
{
#ifdef _WIN32
    return QString::fromStdWString(value.wstring());
#else
    return QString::fromUtf8(value.string().c_str());
#endif
}

using Buffer = std::unique_ptr<HuhuBuffer, decltype(&huhu_buffer_free)>;
using Event = std::unique_ptr<HuhuEvent, decltype(&huhu_event_free)>;
using Receive = std::unique_ptr<HuhuReceive, decltype(&huhu_receive_free)>;
using Lease = std::unique_ptr<HuhuMediaLease, decltype(&huhu_media_lease_free)>;

QJsonObject object(HuhuSlice slice)
{
    require(slice.length <= 1024 * 1024, "Unexpected unbounded native JSON");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(reinterpret_cast<const char*>(slice.data), static_cast<qsizetype>(slice.length)), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid native JSON object");
    return document.object();
}

QJsonObject takeObject(HuhuBuffer* raw)
{
    Buffer owner(raw, &huhu_buffer_free);
    HuhuSlice view{};
    checked(huhu_buffer_view(owner.get(), &view), "Read owned native buffer");
    return object(view);
}

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Open completed media for hash validation");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Hash complete media");
    return hash.result();
}

void validateGif(const QString& path)
{
    QImageReader reader(path, "gif");
    require(reader.supportsAnimation() && reader.imageCount() == 3, "GIF must contain all three generated frames");
    const std::array<QColor, 3> expected{Qt::red, Qt::green, Qt::blue};
    for (const auto& color : expected) {
        const auto frame = reader.read();
        require(!frame.isNull() && frame.size() == QSize(32, 24), "Decode complete GIF frame");
        const auto actual = frame.pixelColor(16, 12);
        require(actual.red() == color.red() && actual.green() == color.green()
            && actual.blue() == color.blue(), "Decoded GIF pixels changed");
    }
    require(!reader.canRead(), "Unexpected extra GIF frames");
}

void encodeGif(const QString& path)
{
    const GifskiSettings settings{32, 24, 90, true, 0};
    auto finish = [](gifski* encoder) { if (encoder) (void)gifski_finish(encoder); };
    std::unique_ptr<gifski, decltype(finish)> encoder(gifski_new(&settings), finish);
    require(encoder != nullptr, "Create actual aggregate-library GIF encoder");
    const auto output = path.toUtf8();
    require(gifski_set_file_output(encoder.get(), output.constData()) == GIFSKI_OK, "Set real GIF output");
    const std::array<QColor, 3> colors{Qt::red, Qt::green, Qt::blue};
    for (std::uint32_t index = 0; index < colors.size(); ++index) {
        QImage frame(32, 24, QImage::Format_RGBA8888);
        frame.fill(colors[index]);
        require(gifski_add_frame_rgba_stride(encoder.get(), index, 32, 24,
            static_cast<std::uint32_t>(frame.bytesPerLine()), frame.constBits(), index / 16.0) == GIFSKI_OK,
            "Encode real Qt RGBA frame through the original gifski C ABI");
    }
    // finish consumes the handle even on failure. Never call it twice.
    require(gifski_finish(encoder.release()) == GIFSKI_OK, "Finalize actual GIF before validation");
    validateGif(path);
}

class Engine {
public:
    HuhuTransfer* handle{};
    QJsonObject info;
    Engine()
    {
        HuhuBuffer* identity{};
        checked(huhu_identity_generate(&identity), "Generate real LocalSend TLS identity");
        auto config = takeObject(identity);
        config.insert("alias", QStringLiteral("huhucapture ABI validation"));
        config.insert("addresses", QJsonArray{QStringLiteral("127.0.0.1")});
        config.insert("port", 0);
        config.insert("discovery", false);
        config.insert("decisionTimeoutMs", 10000);
        config.insert("idleTimeoutMs", 10000);
        config.insert("commitTimeoutMs", 10000);
        const auto encoded = QJsonDocument(config).toJson(QJsonDocument::Compact);
        checked(huhu_transfer_create(bytes(encoded), static_cast<size_t>(encoded.size()), &handle), "Start actual TLS listener");
        std::unique_ptr<HuhuTransfer, decltype(&huhu_transfer_free)> guard(handle, &huhu_transfer_free);
        HuhuBuffer* details{};
        const auto status = huhu_transfer_info(handle, &details);
        checked(status, "Read listener endpoint");
        info = takeObject(details);
        require(info.value("port").toInt() > 0 && info.value("fingerprint").toString().size() == 64,
            "Listener did not provide its actual endpoint and certificate fingerprint");
        guard.release();
    }
    ~Engine() { huhu_transfer_free(handle); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    QByteArray peer() const
    {
        return QJsonDocument(QJsonObject{{"address", "127.0.0.1"}, {"port", info.value("port")},
            {"fingerprint", info.value("fingerprint")}}).toJson(QJsonDocument::Compact);
    }
    Event poll()
    {
        HuhuEvent* raw{};
        const auto status = huhu_transfer_poll(handle, 10, &raw);
        require(status == HUHU_OK || status == HUHU_TIMEOUT || status == HUHU_CLOSED, "Poll native event queue");
        require((status == HUHU_OK) == (raw != nullptr), "Native event ownership/status mismatch");
        return Event(raw, &huhu_event_free);
    }
    void stopAndDrain()
    {
        checked(huhu_transfer_stop(handle), "Cancel and join native work");
        bool stopped = false;
        QDeadlineTimer deadline(10000);
        while (!deadline.hasExpired()) {
            HuhuEvent* raw{};
            const auto status = huhu_transfer_poll(handle, 0, &raw);
            Event event(raw, &huhu_event_free);
            if (status == HUHU_CLOSED) {
                require(stopped, "Native shutdown lost the reserved terminal event");
                checked(huhu_transfer_stop(handle), "Repeat completed shutdown");
                return;
            }
            checked(status, "Drain joined native engine");
            HuhuEventView view{};
            checked(huhu_event_view(event.get(), &view), "Read shutdown event");
            if (view.kind == HUHU_STOPPED) {
                require(!stopped, "Native shutdown produced duplicate terminal events");
                stopped = true;
            }
        }
        throw std::runtime_error("Native queue did not close after joined shutdown");
    }
};

HuhuEventView eventView(const Event& event)
{
    HuhuEventView view{};
    checked(huhu_event_view(event.get(), &view), "Read transfer event");
    return view;
}

void receiveAndCommit(Event& event, const QString& directory, const QByteArray& expectedHash)
{
    HuhuReceive* raw{};
    checked(huhu_event_take_receive(event.get(), &raw), "Take exclusive receive ownership");
    Receive receive(raw, &huhu_receive_free);
    require(huhu_receive_complete(receive.get(), HUHU_OK, HUHU_GIF) != HUHU_OK,
        "Native bridge acknowledged an unverified receive");
    std::error_code error;
    const auto staging = HuhuCapture::Platform::createExclusiveTemporaryFile(nativePath(directory), error);
    require(!error && !staging.empty(), "Claim private receive staging file");
    QFile output(qtPath(staging));
    require(output.open(QIODevice::WriteOnly), "Open owned receive file");
    std::array<std::uint8_t, 64 * 1024> buffer{};
    QDeadlineTimer deadline(20000);
    std::uint64_t total{};
    for (;;) {
        require(!deadline.hasExpired(), "Timed out reading actual TLS body");
        size_t count{};
        const auto status = huhu_receive_read(receive.get(), buffer.data(), buffer.size(), &count, 100);
        if (status == HUHU_TIMEOUT) continue;
        if (status == HUHU_END) break;
        checked(status, "Read verified-stream body");
        require(count > 0 && count <= buffer.size() && total + count <= 1024 * 1024,
            "Native body exceeded the known test media size budget");
        require(output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<qint64>(count))
            == static_cast<qint64>(count), "Write full receive chunk");
        total += count;
    }
    require(output.flush(), "Flush receive writer");
    output.close();
    HuhuVerification verification{};
    int status;
    do {
        require(!deadline.hasExpired(), "Timed out verifying actual TLS body");
        status = huhu_receive_verify(receive.get(), 100, &verification);
    } while (status == HUHU_TIMEOUT);
    checked(status, "Verify complete received media");
    require(verification.received_bytes == total && verification.sender_checksum_matched == 1
        && verification.media_kind == HUHU_GIF, "Verification result lost length, checksum or kind");
    require(QByteArray(reinterpret_cast<const char*>(verification.sha256), 32) == expectedHash
        && fileHash(qtPath(staging)) == expectedHash, "Complete TLS media differs from leased source");
    validateGif(qtPath(staging));
    require(huhu_receive_is_cancelled(receive.get()) == 0, "Receive was cancelled before publication");
    const auto destination = nativePath(directory + QStringLiteral("/received.gif"));
    const auto committed = HuhuCapture::Platform::commitFileNoReplace(staging, destination);
    if (!committed.durable()) {
        (void)huhu_receive_complete(receive.get(), committed.committed()
            ? HUHU_DURABILITY_UNKNOWN : HUHU_IO, HUHU_GIF);
        throw std::runtime_error("Receive could not be committed durably; published media retained");
    }
    checked(huhu_receive_complete(receive.get(), HUHU_OK, HUHU_GIF), "Acknowledge actual durable archive");
}

enum class Scenario { Commit, Decline, Cancel, WrongFingerprint };

void transfer(Engine& sender, Engine& receiver, const Lease& lease, Scenario scenario,
              const QString& destination, const QByteArray& expectedHash)
{
    auto peer = receiver.peer();
    if (scenario == Scenario::WrongFingerprint) {
        auto altered = QJsonDocument::fromJson(peer).object();
        auto fingerprint = altered.value("fingerprint").toString();
        fingerprint[0] = fingerprint[0] == u'0' ? u'1' : u'0';
        altered.insert("fingerprint", fingerprint);
        peer = QJsonDocument(altered).toJson(QJsonDocument::Compact);
    }
    const HuhuMediaLease* leases[]{lease.get()};
    std::uint64_t operation{};
    checked(huhu_transfer_send(sender.handle, bytes(peer), static_cast<size_t>(peer.size()), leases, 1, &operation),
        "Admit real TLS transfer");
    require(operation != 0, "Transfer operation identity is missing");
    QDeadlineTimer deadline(30000);
    bool offered = false, committed = false, senderEnded = false, receiverEnded = false;
    while (!deadline.hasExpired()) {
        if (auto event = receiver.poll()) {
            const auto view = eventView(event);
            const auto json = object(view.json);
            if (view.kind == HUHU_RECEIVE_OFFER) {
                require(!offered && scenario != Scenario::WrongFingerprint, "Unexpected or repeated receive offer");
                offered = true;
                require(json.value("fingerprint") == sender.info.value("fingerprint"), "Sender certificate identity changed");
                const auto session = json.value("sessionId").toString().toUtf8();
                require(!session.isEmpty(), "Missing receive session identity");
                if (scenario == Scenario::Cancel) {
                    checked(huhu_transfer_cancel(sender.handle, operation), "Cancel pending real TLS transfer");
                } else {
                    QJsonArray accepted;
                    const auto files = json.value("files").toArray();
                    require(files.size() == 1, "Offer did not contain the selected leased file");
                    if (scenario == Scenario::Commit) accepted.append(files.first().toObject().value("id"));
                    const auto ids = QJsonDocument(accepted).toJson(QJsonDocument::Compact);
                    checked(huhu_transfer_accept(receiver.handle, bytes(session), static_cast<size_t>(session.size()),
                        bytes(ids), static_cast<size_t>(ids.size())), "Apply explicit receive decision");
                }
            } else if (view.kind == HUHU_RECEIVE_FILE) {
                require(scenario == Scenario::Commit && offered && !committed, "Unexpected media body admission");
                receiveAndCommit(event, destination, expectedHash);
                committed = true;
            } else if (view.kind == HUHU_RECEIVE_END) {
                require(!receiverEnded, "Receive produced duplicate terminal events");
                receiverEnded = true;
                if (scenario == Scenario::Commit) require(view.status == HUHU_OK && committed,
                    "Receive succeeded before actual durable commit");
            } else if (view.kind == HUHU_LISTENER_ERROR) {
                throw std::runtime_error("Actual TLS listener failed");
            }
        }
        if (auto event = sender.poll()) {
            const auto view = eventView(event);
            if (view.kind == HUHU_SEND_END) {
                require(view.operation == operation && !senderEnded, "Send terminal identity/count mismatch");
                senderEnded = true;
                if (scenario == Scenario::Commit)
                    require(view.status == HUHU_OK && committed, "Sender success preceded durable receiver success");
                else if (scenario == Scenario::Cancel)
                    require(view.status == HUHU_CANCELLED, "Cancelled sender did not report cancellation");
                else require(view.status != HUHU_OK, "Rejected transfer reported success");
            }
        }
        if (senderEnded && (scenario == Scenario::WrongFingerprint || receiverEnded)) {
            require((scenario == Scenario::WrongFingerprint) != offered, "Unexpected offer state at completion");
            std::cout << "PASS TLS scenario " << static_cast<int>(scenario) << '\n';
            return;
        }
        QCoreApplication::processEvents();
    }
    throw std::runtime_error("Actual TLS transfer did not deliver its terminal events");
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        require(argc == 2, "Provide a private ABI evidence directory");
        require(huhu_native_abi_version() == 1, "Unrecognized aggregate native ABI");
        const auto base = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
        require(QDir().mkpath(base), "Create ABI evidence root");
        QTemporaryDir evidence(base + QStringLiteral("/run-XXXXXX"));
        require(evidence.isValid(), "Create exclusive ABI evidence directory");
        evidence.setAutoRemove(false);
        const auto gif = evidence.path() + QStringLiteral("/真实编码.gif");
        encodeGif(gif);
        const auto hash = fileHash(gif);
        Engine receiver, sender;
        HuhuMediaLease* raw{};
        const auto encodedPath = gif.toUtf8();
        checked(huhu_media_lease_open(bytes(encodedPath), static_cast<size_t>(encodedPath.size()), &raw), "Lease finalized GIF");
        Lease lease(raw, &huhu_media_lease_free);
        transfer(sender, receiver, lease, Scenario::Commit, evidence.path(), hash);
        transfer(sender, receiver, lease, Scenario::WrongFingerprint, evidence.path(), hash);
        transfer(sender, receiver, lease, Scenario::Decline, evidence.path(), hash);
        transfer(sender, receiver, lease, Scenario::Cancel, evidence.path(), hash);
        sender.stopAndDrain();
        receiver.stopAndDrain();
        require(fileHash(evidence.path() + QStringLiteral("/received.gif")) == hash,
            "Shutdown changed the durable received archive");
        std::cout << "PASS same Qt process: one Rust static library, original gifski, real TLS, "
                     "full GIF decode, SHA-256, durable archive, refusal, cancellation and joined shutdown\n"
                     "Loopback ABI validation does not prove multicast discovery or two-machine interoperability.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL Qt/native aggregate validation: " << error.what() << '\n';
        return 1;
    }
}
