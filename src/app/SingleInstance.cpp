#include "app/SingleInstance.h"
#include "services/files/QtPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <filesystem>
#include <utility>

namespace HuhuCapture::Application {
namespace {
constexpr qsizetype requestBytes = 24;
constexpr qsizetype replyBytes = 21;
constexpr int maximumClients = 8;
constexpr int deadlineMs = 3000;

bool validEntry(Core::CaptureEntry entry)
{
    switch (entry) {
    case Core::CaptureEntry::Capture:
    case Core::CaptureEntry::Pin:
    case Core::CaptureEntry::Scroll:
    case Core::CaptureEntry::Recording:
    case Core::CaptureEntry::Qr: return true;
    }
    return false;
}

QByteArray encodeRequest(const Core::StartupRequest& request, const QByteArray& id)
{
    QByteArray bytes("HHCP", 4);
    bytes.append(char(1));
    bytes.append(static_cast<char>(request.entry));
    bytes.append(request.background ? char(1) : char(0));
    bytes.append(char(0));
    bytes.append(id);
    return bytes;
}
} // namespace

struct SingleInstance::State {
    struct Peer { QByteArray bytes; QTimer* timer{}; bool processed{}; };
    SingleInstance* q;
    QString runtimeDirectory;
    QString scope;
    AdmitActivation admit;
    Core::StartupRequest initial;
    InstanceRole role{InstanceRole::Pending};
    bool started{};
    bool accepting{};
    bool shuttingDown{};
    bool submitted{};
    QString name;
    QByteArray requestId;
    QByteArray reply;
    std::unique_ptr<QLockFile> lock;
    QLocalServer server;
    QLocalSocket client;
    QTimer retry;
    QTimer deadline;
    QHash<QLocalSocket*, Peer> peers;

    State(SingleInstance* owner, QString directory, QString key, AdmitActivation handler)
        : q(owner), runtimeDirectory(std::move(directory)), scope(std::move(key)), admit(std::move(handler))
    {
        retry.setInterval(50);
        deadline.setSingleShot(true);
        deadline.setInterval(deadlineMs);
        client.setReadBufferSize(replyBytes + 1);
#ifdef Q_OS_WIN
        server.setSocketOptions(QLocalServer::UserAccessOption);
#else
        // The 0700 parent supplies the access boundary. Qt's explicit Unix
        // permission mode binds a temporary and renames it over the requested
        // path; use ordinary bind so an existing non-socket is never replaced.
        server.setSocketOptions(QLocalServer::NoOptions);
#endif
        server.setMaxPendingConnections(maximumClients);
        server.setListenBacklogSize(maximumClients);
        QObject::connect(&retry, &QTimer::timeout, q, [this] { attempt(); });
        QObject::connect(&deadline, &QTimer::timeout, q, [this] {
            fail(submitted ? InstanceFailure::DeliveryUnknown : InstanceFailure::PrimaryUnavailable,
                submitted ? QStringLiteral("The existing instance did not acknowledge the action")
                          : QStringLiteral("The existing instance is unavailable or shutting down"));
        });
        QObject::connect(&server, &QLocalServer::newConnection, q, [this] { acceptConnections(); });
        QObject::connect(&client, &QLocalSocket::connected, q, [this] {
            if (role != InstanceRole::Pending || shuttingDown) return;
            retry.stop();
            // Once write is attempted there is no automatic resend or takeover:
            // a lost acknowledgment must not cause duplicate capture actions.
            submitted = true;
            const auto bytes = encodeRequest(initial, requestId);
            if (client.write(bytes) != bytes.size())
                fail(InstanceFailure::DeliveryUnknown, QStringLiteral("The activation request could not be fully queued"));
        });
        QObject::connect(&client, &QLocalSocket::readyRead, q, [this] { readReply(); });
        QObject::connect(&client, &QLocalSocket::errorOccurred, q, [this](QLocalSocket::LocalSocketError error) {
            if (role != InstanceRole::Pending || shuttingDown) return;
            QPointer<SingleInstance> alive(q);
            readReply();
            if (!alive || role != InstanceRole::Pending) return;
            if (submitted) {
                fail(InstanceFailure::DeliveryUnknown, QStringLiteral("The existing instance closed before acknowledgment"));
            } else if (error != QLocalSocket::ServerNotFoundError && error != QLocalSocket::ConnectionRefusedError) {
                fail(InstanceFailure::PrimaryUnavailable, client.errorString());
            }
            // A just-created primary may own its lock before listen(). The
            // timer retries only while no action bytes have been submitted.
        });
        QObject::connect(&client, &QLocalSocket::disconnected, q, [this] {
            if (role != InstanceRole::Pending || shuttingDown || !submitted) return;
            QPointer<SingleInstance> alive(q);
            readReply();
            if (alive && role == InstanceRole::Pending)
                fail(InstanceFailure::DeliveryUnknown, QStringLiteral("The activation acknowledgment was incomplete"));
        });
    }

    void finish(InstanceResult result)
    {
        if (role != InstanceRole::Pending || shuttingDown) return;
        role = result.role;
        retry.stop(); deadline.stop();
        client.abort();
        Q_EMIT q->startupFinished(result);
    }

    void fail(InstanceFailure failure, const QString& detail)
    {
        finish({InstanceRole::Failed, failure, detail});
    }

    void begin()
    {
        if (shuttingDown || role != InstanceRole::Pending) return;
        if (!validEntry(initial.entry) || (initial.oneShot && initial.background)) {
            fail(InstanceFailure::InvalidScope, QStringLiteral("Invalid startup request")); return;
        }
        if (initial.oneShot) { finish({InstanceRole::Independent, {}, {}}); return; }
        if (runtimeDirectory.isEmpty() || runtimeDirectory.contains(QChar::Null) ||
            scope.isEmpty() || scope.size() > 4096 || scope.contains(QChar::Null) || !admit) {
            fail(InstanceFailure::InvalidScope, QStringLiteral("A runtime directory, stable scope and action handler are required"));
            return;
        }
        const auto root = QFileInfo(runtimeDirectory).canonicalFilePath();
        if (root.isEmpty() || !QFileInfo(root).isDir()) {
            fail(InstanceFailure::RuntimeDirectory, QStringLiteral("The user runtime directory is unavailable")); return;
        }
        const auto privateDirectory = QDir(root).filePath(QStringLiteral("huhucapture"));
        if (QFileInfo(privateDirectory).isSymLink() || !QDir().mkpath(privateDirectory)) {
            fail(InstanceFailure::RuntimeDirectory, QStringLiteral("Cannot create the private startup directory")); return;
        }
#ifndef Q_OS_WIN
        if (!QFile::setPermissions(privateDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
            fail(InstanceFailure::RuntimeDirectory, QStringLiteral("Cannot protect the startup directory")); return;
        }
#endif
        const auto digest = QCryptographicHash::hash(root.toUtf8() + '\0' + scope.toUtf8(),
            QCryptographicHash::Sha256).toHex().first(32);
        const auto base = QStringLiteral("huhucapture-") + QString::fromLatin1(digest);
#ifdef Q_OS_WIN
        name = base;
#else
        name = QDir(privateDirectory).filePath(base);
        if (QFile::encodeName(name).size() > 103) {
            fail(InstanceFailure::RuntimeDirectory, QStringLiteral("The local socket path exceeds the platform limit")); return;
        }
#endif
        lock = std::make_unique<QLockFile>(QDir(privateDirectory).filePath(base + QStringLiteral(".lock")));
        // A slow but living process never becomes stale just because time has
        // elapsed. Qt may still recover a lock after its process has exited.
        lock->setStaleLockTime(0);
        requestId = QUuid::createUuid().toRfc4122();
        deadline.start(); retry.start();
        attempt();
    }

    void attempt()
    {
        if (role != InstanceRole::Pending || shuttingDown || submitted || !lock) return;
        if (lock->tryLock(0)) {
#ifndef Q_OS_WIN
            std::error_code pathError;
            auto status = std::filesystem::symlink_status(Files::nativePath(name), pathError);
            if ((pathError && pathError != std::errc::no_such_file_or_directory) ||
                (status.type() != std::filesystem::file_type::not_found && !std::filesystem::is_socket(status))) {
                lock->unlock();
                fail(InstanceFailure::Listen, QStringLiteral("Startup endpoint is occupied by a non-socket or unreadable object"));
                return;
            }
#endif
            // Only the lock owner may remove a leftover endpoint. First try the
            // ordinary listen path, so unrelated failures are never unlinked.
            if (!server.listen(name)) {
                bool removable = server.serverError() == QAbstractSocket::AddressInUseError;
#ifndef Q_OS_WIN
                status = std::filesystem::symlink_status(Files::nativePath(name), pathError);
                removable = removable && !pathError && std::filesystem::is_socket(status);
#endif
                if (!removable || !QLocalServer::removeServer(name) || !server.listen(name)) {
                    const auto detail = server.errorString();
                    lock->unlock(); fail(InstanceFailure::Listen, detail); return;
                }
            }
            accepting = true;
            finish({InstanceRole::Primary, {}, {}});
            return;
        }
        if (lock->error() != QLockFile::LockFailedError) {
            fail(InstanceFailure::Lock, QStringLiteral("Cannot acquire the startup lock")); return;
        }
        if (client.state() == QLocalSocket::UnconnectedState)
            client.connectToServer(name, QIODevice::ReadWrite);
    }

    void readReply()
    {
        if (role != InstanceRole::Pending || shuttingDown || !submitted) return;
        reply += client.read(replyBytes + 1 - reply.size());
        if (reply.size() < replyBytes) return;
        if (reply.size() != replyBytes || reply.first(4) != QByteArrayLiteral("HCOK") ||
            reply.sliced(4, 16) != requestId || (reply[20] != char(0) && reply[20] != char(1))) {
            fail(InstanceFailure::InvalidReply, QStringLiteral("Invalid activation acknowledgment; delivery is uncertain"));
            return;
        }
        if (reply[20] == char(1)) finish({InstanceRole::Forwarded, {}, {}});
        else fail(InstanceFailure::Rejected, QStringLiteral("The existing instance could not accept this action"));
    }

    void retire(QLocalSocket* socket)
    {
        const auto found = peers.find(socket);
        if (found == peers.end()) return;
        found->timer->stop();
        QObject::disconnect(found->timer, nullptr, q, nullptr);
        peers.erase(found);
        QObject::disconnect(socket, nullptr, q, nullptr);
        socket->abort();
        socket->deleteLater();
    }

    void acceptConnections()
    {
        while (auto* socket = server.nextPendingConnection()) {
            if (!accepting || shuttingDown || peers.size() >= maximumClients) {
                socket->abort(); socket->deleteLater(); continue;
            }
            socket->setReadBufferSize(requestBytes + 1);
            auto* timer = new QTimer(socket);
            timer->setSingleShot(true);
            timer->setInterval(deadlineMs);
            peers.insert(socket, {{}, timer, false});
            QObject::connect(timer, &QTimer::timeout, q, [this, socket] { retire(socket); });
            QObject::connect(socket, &QLocalSocket::disconnected, q, [this, socket] { retire(socket); });
            QObject::connect(socket, &QLocalSocket::readyRead, q, [this, socket] { readRequest(socket); });
            timer->start();
            QPointer<SingleInstance> alive(q);
            readRequest(socket);
            if (!alive) return;
        }
    }

    void readRequest(QLocalSocket* socket)
    {
        auto found = peers.find(socket);
        if (found == peers.end() || found->processed || !accepting || shuttingDown) return;
        auto& bytes = found->bytes;
        bytes += socket->read(requestBytes + 1 - bytes.size());
        if (bytes.size() < requestBytes) return;
        const auto entry = static_cast<Core::CaptureEntry>(static_cast<unsigned char>(bytes[5]));
        if (bytes.size() != requestBytes || bytes.first(4) != QByteArrayLiteral("HHCP") ||
            bytes[4] != char(1) || !validEntry(entry) || (bytes[6] != char(0) && bytes[6] != char(1)) ||
            bytes[7] != char(0) || QUuid::fromRfc4122(bytes.sliced(8, 16)).isNull()) {
            retire(socket); return;
        }
        const auto id = bytes.sliced(8, 16);
        const Core::StartupRequest request{entry, false, bytes[6] == char(1)};
        found->processed = true;
        QPointer<SingleInstance> alive(q);
        bool accepted = false;
        bool uncertain = false;
        try {
            // Copy before calling user code: the callback may close windows,
            // stop admission, or destroy the complete application coordinator.
            const auto callback = admit;
            accepted = callback && callback(request);
        } catch (...) { uncertain = true; }
        if (!alive || !peers.contains(socket) || shuttingDown) return;
        // A handler may have handed off the action before throwing. Only an
        // explicit false return proves rejection; losing the acknowledgment
        // preserves the caller's no-automatic-resend rule for uncertain work.
        if (uncertain) { retire(socket); return; }
        const auto acknowledgment = QByteArrayLiteral("HCOK") + id + (accepted ? char(1) : char(0));
        if (socket->write(acknowledgment) != acknowledgment.size()) { retire(socket); return; }
        socket->disconnectFromServer();
    }

    void stopAccepting()
    {
        accepting = false;
        server.close();
        const auto clients = peers.keys();
        for (auto* socket : clients) retire(socket);
    }

    void shutdown()
    {
        shuttingDown = true;
        retry.stop(); deadline.stop();
        QObject::disconnect(&retry, nullptr, q, nullptr);
        QObject::disconnect(&deadline, nullptr, q, nullptr);
        QObject::disconnect(&client, nullptr, q, nullptr);
        QObject::disconnect(&server, nullptr, q, nullptr);
        stopAccepting(); client.abort();
        // The server and all accepted sockets are closed before unlocking.
        if (lock) lock->unlock();
    }
};

SingleInstance::SingleInstance(QString runtimeDirectory, QString scope, AdmitActivation admit, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(runtimeDirectory),
        std::move(scope), std::move(admit))) {}

SingleInstance::~SingleInstance() { state_->shutdown(); }

bool SingleInstance::start(Core::StartupRequest request)
{
    if (thread() != QThread::currentThread() || state_->started || state_->shuttingDown ||
        state_->role != InstanceRole::Pending) return false;
    state_->started = true;
    state_->initial = request;
    QTimer::singleShot(0, this, [this] { state_->begin(); });
    return true;
}

void SingleInstance::stopAccepting()
{
    if (thread() != QThread::currentThread()) return;
    state_->stopAccepting();
    if (state_->role == InstanceRole::Pending)
        state_->fail(InstanceFailure::Rejected, QStringLiteral("Application startup is stopping"));
}

InstanceRole SingleInstance::role() const noexcept { return state_->role; }
QString SingleInstance::endpoint() const { return state_->name; }

} // namespace HuhuCapture::Application
