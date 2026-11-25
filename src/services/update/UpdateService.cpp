#include "services/update/UpdateService.h"

#include "core/ReleaseVersion.h"
#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"
#include "services/files/QtPaths.h"
#include "services/update/ReleaseManifest.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace HuhuCapture::Updates {
namespace {
using Files::nativePath;
using Files::qtPath;
bool working(Stage stage) { return stage == Stage::Checking || stage == Stage::Downloading; }

// This is a type guard, not a second archive/deployment engine. The GitHub
// release/build manifests bind the exact package hash to the recorded build tree;
// package installation and full dependency inspection remain release gates.
bool packageHeader(QFile& file, PackageKind kind)
{
    if (!file.seek(0)) return false;
    const auto prefix = file.read(96);
    switch (kind) {
    case PackageKind::WindowsPortableZip:
        return prefix.startsWith(QByteArray("PK\x03\x04", 4));
    case PackageKind::LinuxDeb: {
        if (prefix.size() < 72 || !prefix.startsWith("!<arch>\n") ||
            prefix.mid(8, 16).trimmed() != "debian-binary" || prefix.mid(66, 2) != "`\n" ||
            prefix.mid(68, 4) != "2.0\n") return false;
        bool valid{}; const auto size = prefix.mid(56, 10).trimmed().toULongLong(&valid);
        return valid && size == 4;
    }
    case PackageKind::LinuxAppImage:
        return prefix.size() >= 64 && prefix.startsWith(QByteArray("\x7f" "ELF", 4)) &&
            prefix[4] == 2 && prefix[5] == 1 && prefix.mid(8, 3) == QByteArray("AI\x02", 3) &&
            qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(prefix.constData() + 18)) == 62;
    case PackageKind::WindowsInstaller: {
        if (prefix.size() < 64 || !prefix.startsWith("MZ")) return false;
        const auto offset = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(prefix.constData() + 60));
        if (offset < 64 || offset > 1024 * 1024 || !file.seek(offset)) return false;
        std::array<char, 26> header{};
        if (file.read(header.data(), static_cast<qint64>(header.size())) != static_cast<qint64>(header.size()) ||
            !QByteArrayView(header.data(), static_cast<qsizetype>(header.size())).startsWith(QByteArrayView("PE\0\0", 4))) return false;
        const auto machine = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.data() + 4));
        const auto flags = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.data() + 22));
        const auto magic = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.data() + 24));
        // The mature NSIS installer stub may be x86 while its full payload is
        // x64. Build-info binds that payload's architecture; do not reject NSIS.
        return ((machine == 0x014c && magic == 0x010b) || (machine == 0x8664 && magic == 0x020b)) &&
            (flags & 0x0002) && !(flags & 0x2000);
    }
    }
    return false;
}

class DownloadWorker final : public QObject {
public:
    DownloadWorker(Options options, std::shared_ptr<std::atomic_bool> cancel,
        std::function<void(Snapshot)> report)
        : options_(std::move(options)), cancelled_(std::move(cancel)), report_(std::move(report)),
          hash_(QCryptographicHash::Sha256)
    { status_.packageKind = options_.packageKind; }

    void begin(bool download)
    {
        if (stopping_ || active_) return;
        active_ = true; download_ = download; release_ = {}; selected_ = {};
        status_ = {}; status_.packageKind = options_.packageKind; status_.stage = Stage::Checking;
        if (!Manifest::canonicalVersion(options_.currentVersion) ||
            (options_.loopbackTestOrigin && !Manifest::validTestOrigin(*options_.loopbackTestOrigin))) {
            fail(Error::Metadata); return;
        }
        if (!network_) network_ = new QNetworkAccessManager(this);
        if (!deadline_) {
            deadline_ = new QTimer(this); deadline_->setSingleShot(true);
            connect(deadline_, &QTimer::timeout, this, [this] { fail(Error::Network); });
        }
        request(Phase::Release, Manifest::latestUrl(), {}, 0, Manifest::maximumMetadataBytes);
    }

    void stop()
    {
        if (stopping_) return;
        stopping_ = true; cancelled_->store(true, std::memory_order_release);
        const bool incomplete = active_;
        active_ = false; clearReply(); cleanupStaging();
        delete network_; network_ = nullptr;
        if (incomplete) { status_.stage = Stage::Cancelled; status_.error = Error::None; report_(status_); }
        QThread::currentThread()->quit();
    }

private:
    enum class Phase { Release, Manifest, BuildInfo, Package };
    Options options_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::function<void(Snapshot)> report_;
    Snapshot status_;
    Manifest::Release release_;
    Manifest::Selection selected_;
    QNetworkAccessManager* network_{};
    QNetworkReply* reply_{};
    QTimer* deadline_{};
    QFile* output_{};
    std::optional<Platform::FileHandle> writer_;
    std::optional<Platform::FileSnapshot> claimed_;
    std::filesystem::path staging_;
    QString directory_;
    QCryptographicHash hash_;
    QElapsedTimer progress_;
    QElapsedTimer requestTime_;
    QByteArray body_, expectedHash_;
    QUrl canonical_;
    Phase phase_{Phase::Release};
    quint64 expected_{}, limit_{}, received_{}, ignored_{};
    qint64 requestBudget_{};
    unsigned redirects_{};
    bool active_{}, download_{}, stopping_{}, headerChecked_{};

    bool cancelled() const { return cancelled_->load(std::memory_order_acquire); }
    void clearReply()
    {
        if (deadline_) deadline_->stop();
        if (!reply_) return;
        auto* reply = std::exchange(reply_, nullptr);
        disconnect(reply, nullptr, this, nullptr);
        reply->abort(); reply->deleteLater();
    }
    void cleanupStaging()
    {
        if (output_) { output_->close(); delete output_; output_ = nullptr; }
        writer_.reset();
        if (!staging_.empty() && claimed_) {
            std::error_code error;
            const auto current = Platform::readFileSnapshot(staging_, error);
            if (current && current->identity == claimed_->identity) std::filesystem::remove(staging_, error);
        }
        staging_.clear(); claimed_.reset();
    }
    void fail(Error error)
    {
        if (!active_) return;
        active_ = false; clearReply(); cleanupStaging();
        status_.stage = cancelled() ? Stage::Cancelled : Stage::Failed;
        status_.error = cancelled() ? Error::None : error;
        report_(status_);
    }
    void request(Phase phase, const QUrl& canonical, QByteArray expectedHash, quint64 expected, quint64 limit)
    {
        clearReply();
        phase_ = phase; canonical_ = canonical; expectedHash_ = std::move(expectedHash);
        expected_ = expected; limit_ = limit; redirects_ = 0;
        requestBudget_ = phase == Phase::Package ? 60 * 60 * 1000 : 30000;
        requestTime_.start();
        issue(Manifest::requestUrl(canonical, options_.loopbackTestOrigin));
    }
    void issue(const QUrl& url)
    {
        if (cancelled()) { fail(Error::Network); return; }
        if (!url.isValid() || url.isEmpty()) { fail(Error::Metadata); return; }
        // Redirects share this response's wall-clock budget. An otherwise
        // allowed chain must not restart the timeout at every hop.
        const auto remaining = requestBudget_ - requestTime_.elapsed();
        if (remaining <= 0) { fail(Error::Network); return; }
        received_ = 0; ignored_ = 0; headerChecked_ = false; body_.clear(); hash_.reset();
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "huhucapture");
        request.setRawHeader("Accept", phase_ == Phase::Release ? "application/vnd.github+json" : "application/octet-stream");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        request.setTransferTimeout(30000);
        reply_ = network_->get(request);
        reply_->setReadBufferSize(256 * 1024);
        connect(reply_, &QNetworkReply::metaDataChanged, this, [this] { inspectHeaders(); });
        connect(reply_, &QNetworkReply::readyRead, this, [this] { readBody(); });
        connect(reply_, &QNetworkReply::finished, this, [this] { completeResponse(); });
        deadline_->start(static_cast<int>(remaining));
    }
    void inspectHeaders()
    {
        if (!active_ || !reply_ || headerChecked_) return;
        const auto http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!http) return;
        headerChecked_ = true;
        if (http != 200) return;
        const auto encoding = reply_->rawHeader("Content-Encoding").trimmed().toLower();
        if (!encoding.isEmpty() && encoding != "identity") { fail(Error::Integrity); return; }
        if (reply_->hasRawHeader("Content-Length")) {
            const auto text = reply_->rawHeader("Content-Length");
            bool valid{};
            const auto length = text.toULongLong(&valid);
            if (!valid || text.isEmpty() || !std::all_of(text.cbegin(), text.cend(), [](char c) { return c >= '0' && c <= '9'; }) ||
                length > limit_ || (expected_ && length != expected_)) fail(Error::Integrity);
        }
    }
    void readBody()
    {
        if (!active_ || !reply_) return;
        inspectHeaders();
        if (!active_ || !reply_) return;
        while (reply_->bytesAvailable() > 0) {
            if (cancelled()) { fail(Error::Network); return; }
            const auto chunk = reply_->read(64 * 1024);
            if (chunk.isEmpty()) { fail(Error::Network); return; }
            const auto amount = static_cast<quint64>(chunk.size());
            if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
                ignored_ += amount;
                if (ignored_ > 64 * 1024) { fail(Error::Network); return; }
                continue;
            }
            if (amount > limit_ - received_ || (expected_ && amount > expected_ - received_)) {
                fail(Error::Integrity); return;
            }
            if (phase_ == Phase::Package) {
                if (!output_ || output_->write(chunk) != chunk.size()) { fail(Error::Save); return; }
            } else body_.append(chunk);
            hash_.addData(chunk); received_ += amount;
            if (phase_ == Phase::Package) {
                status_.received = received_;
                if (!progress_.isValid() || progress_.elapsed() >= 100) { progress_.restart(); report_(status_); }
            }
        }
    }
    void completeResponse()
    {
        if (!active_ || !reply_) return;
        readBody();
        if (!active_ || !reply_) return;
        const auto http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308) {
            const auto redirect = reply_->url().resolved(reply_->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (++redirects_ > 5 || !Manifest::allowedRedirect(redirect, canonical_, options_.loopbackTestOrigin)) {
                fail(Error::Metadata); return;
            }
            clearReply(); issue(redirect); return;
        }
        if (http != 200 || reply_->error() != QNetworkReply::NoError) { fail(Error::Network); return; }
        if ((expected_ && received_ != expected_) || (!expectedHash_.isEmpty() && hash_.result().toHex() != expectedHash_)) {
            fail(Error::Integrity); return;
        }
        clearReply();
        if (cancelled()) { fail(Error::Network); return; }
        switch (phase_) {
        case Phase::Release: {
            if (!Manifest::parseRelease(body_, release_)) { fail(Error::Metadata); return; }
            status_.version = release_.version; status_.tag = release_.tag;
            const auto asset = release_.assets.value(QStringLiteral("release-manifest.json"));
            request(Phase::Manifest, asset.url, asset.sha256, asset.size, Manifest::maximumMetadataBytes);
            break;
        }
        case Phase::Manifest:
            if (!Manifest::selectPackage(body_, release_, options_.packageKind, selected_)) { fail(Error::Metadata); return; }
            status_.assetName = selected_.package.name; status_.total = selected_.package.size;
            request(Phase::BuildInfo, selected_.buildInfo.url, selected_.buildInfo.sha256,
                selected_.buildInfo.size, Manifest::maximumBuildInfoBytes);
            break;
        case Phase::BuildInfo:
            if (!Manifest::validateBuildInfo(body_, selected_)) { fail(Error::Metadata); return; }
            if (!download_) {
                const auto current = Core::parseReleaseVersion(options_.currentVersion.toStdWString());
                const auto offered = Core::parseReleaseVersion(status_.version.toStdWString());
                status_.stage = current && offered && *offered > *current ? Stage::Available : Stage::Idle;
                active_ = false; report_(status_); return;
            }
            if (!prepareStaging()) return;
            status_.stage = Stage::Downloading; status_.received = 0; report_(status_);
            request(Phase::Package, selected_.package.url, selected_.package.sha256,
                selected_.package.size, Manifest::packageBudget(options_.packageKind));
            break;
        case Phase::Package: commit(); break;
        }
    }
    bool prepareStaging()
    {
        const auto root = options_.downloadsDirectory;
        if (root.isEmpty() || root.contains(QChar::Null) || !QDir::isAbsolutePath(root) ||
            root.startsWith(u':') || !QDir().mkpath(root)) { fail(Error::Folder); return false; }
        directory_ = QDir(root).canonicalPath();
        if (directory_.isEmpty()) { fail(Error::Folder); return false; }
        std::error_code error;
        staging_ = Platform::createExclusiveTemporaryFile(nativePath(directory_), error);
        if (staging_.empty()) { fail(Error::Save); return false; }
        claimed_ = Platform::readFileSnapshot(staging_, error);
        if (!claimed_) { fail(Error::Save); return false; }
        writer_.emplace(Platform::openOwnedEmptyFileForWrite(staging_, claimed_->identity, error));
        if (!*writer_) { fail(Error::Save); return false; }
        output_ = new QFile(this);
        if (!output_->open(writer_->get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle)) {
            fail(Error::Save); return false;
        }
        return true;
    }
    void commit()
    {
        if (!output_ || !writer_ || !claimed_ || !output_->flush()) { fail(Error::Save); return; }
        if (!packageHeader(*output_, options_.packageKind)) { fail(Error::Integrity); return; }
        std::error_code error;
        const auto actual = Platform::readFileDescriptorSnapshot(writer_->get(), error);
        const auto named = Platform::readFileSnapshot(staging_, error);
        if (!actual || !named || actual->identity != claimed_->identity || *actual != *named || actual->bytes != expected_) {
            fail(Error::Integrity); return;
        }
        output_->close();
        if (output_->error() != QFileDevice::NoError || !writer_->close(error)) { fail(Error::Save); return; }
        delete output_; output_ = nullptr; writer_.reset();
        const auto dot = selected_.package.name.lastIndexOf(u'.');
        const auto stem = selected_.package.name.left(dot), suffix = selected_.package.name.mid(dot);
        for (unsigned collision = 0; collision < 10000; ++collision) {
            if (cancelled()) { fail(Error::Network); return; }
            const auto name = collision ? stem + QStringLiteral("-%1").arg(collision, 3, 10, QLatin1Char('0')) + suffix : selected_.package.name;
            const auto destination = nativePath(QDir(directory_).filePath(name));
            const auto committed = Platform::commitFileNoReplace(staging_, destination);
            if (committed.committed()) {
                status_.file = qtPath(committed.destination);
                // Final names never belong to cleanup, even if directory sync
                // or a late cancellation prevents reporting a durable result.
                cleanupStaging(); active_ = false;
                status_.received = received_;
                status_.stage = committed.durable() ? Stage::Downloaded : Stage::Failed;
                status_.error = committed.durable() ? Error::None : Error::DurabilityUnknown;
                report_(status_); return;
            }
            if (committed.error != std::errc::file_exists) { fail(Error::Save); return; }
        }
        fail(Error::Save);
    }
};
} // namespace

struct UpdateService::State {
    UpdateService* owner;
    Options options;
    Snapshot current;
    QThread thread;
    DownloadWorker* worker{};
    std::shared_ptr<std::atomic_bool> cancelled{std::make_shared<std::atomic_bool>(false)};
    std::mutex mutex;
    std::optional<Snapshot> latest;
    bool noticeQueued{}, stopping{}, finished{};

    State(UpdateService* q, Options value) : owner(q), options(std::move(value))
    { current.packageKind = options.packageKind; thread.setObjectName(QStringLiteral("huhu-update")); }

    void post(Snapshot value)
    {
        bool schedule{};
        {
            std::lock_guard lock(mutex); latest = std::move(value);
            if (!noticeQueued) { noticeQueued = true; schedule = true; }
        }
        if (schedule) QMetaObject::invokeMethod(owner, [this] { drain(); }, ::Qt::QueuedConnection);
    }
    void drain()
    {
        std::optional<Snapshot> value;
        { std::lock_guard lock(mutex); value.swap(latest); noticeQueued = false; }
        if (!value) return;
        current = std::move(*value); current.stopped = stopping;
        // A subscriber may start the next task synchronously. Deliver a stable
        // value so later subscribers still observe this completion snapshot.
        const auto delivered = current;
        Q_EMIT owner->changed(delivered);
    }
    void start(bool download)
    {
        if (!worker) {
            worker = new DownloadWorker(options, cancelled, [this](Snapshot value) { post(std::move(value)); });
            worker->moveToThread(&thread);
            QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
            QObject::connect(&thread, &QThread::finished, owner, [this] {
                pollJoin();
            });
            thread.start();
        }
        cancelled->store(false, std::memory_order_release);
        current = {}; current.packageKind = options.packageKind; current.stage = Stage::Checking;
        QMetaObject::invokeMethod(worker, [worker = worker, download] { worker->begin(download); }, ::Qt::QueuedConnection);
        const auto delivered = current;
        Q_EMIT owner->changed(delivered);
    }
    void pollJoin()
    {
        // QThread::finished precedes deferred deletes and thread-local cleanup.
        // A zero-timeout wait is the public actual join probe on our supported
        // Linux/Windows Qt builds; keep servicing the GUI until it succeeds.
        if (!thread.wait(0)) {
            QTimer::singleShot(10, owner, [this] { pollJoin(); });
            return;
        }
        worker = nullptr;
        completeShutdown();
    }
    void completeShutdown()
    {
        if (finished) return;
        finished = true;
        const QPointer<UpdateService> lifetime(owner);
        drain();
        if (!lifetime) return;
        current.stopped = true;
        if (working(current.stage)) { current.stage = Stage::Cancelled; current.error = Error::None; }
        const auto delivered = current;
        Q_EMIT owner->changed(delivered);
        if (lifetime) Q_EMIT owner->shutdownFinished();
    }
    void stop()
    {
        if (stopping) return;
        stopping = true; cancelled->store(true, std::memory_order_release);
        if (!worker) { completeShutdown(); return; }
        QMetaObject::invokeMethod(worker, [worker = worker] { worker->stop(); }, ::Qt::QueuedConnection);
    }
    void join()
    {
        stopping = true; cancelled->store(true, std::memory_order_release);
        if (worker && thread.isRunning()) {
            QMetaObject::invokeMethod(worker, [worker = worker] { worker->stop(); }, ::Qt::QueuedConnection);
        }
        thread.wait();
        worker = nullptr;
    }
};

UpdateService::UpdateService(Options options, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(options)))
{ qRegisterMetaType<Snapshot>(); }
UpdateService::~UpdateService() { state_->join(); }
Snapshot UpdateService::snapshot() const { return state_->current; }
bool UpdateService::busy() const { return working(state_->current.stage); }
bool UpdateService::begin(bool download)
{
    if (QThread::currentThread() != thread() || state_->stopping || busy()) return false;
    state_->start(download); return true;
}
bool UpdateService::check() { return begin(false); }
bool UpdateService::download() { return begin(true); }
void UpdateService::shutdown()
{
    if (QThread::currentThread() != thread()) return;
    state_->stop();
}
} // namespace HuhuCapture::Updates
