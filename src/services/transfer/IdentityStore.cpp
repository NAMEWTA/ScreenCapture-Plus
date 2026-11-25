#include "services/transfer/IdentityStore.h"

#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"
#include "platform/PrivateStorage.h"
#include "platform/rust/include/huhucapture_native.h"
#include "services/files/QtPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <algorithm>
#include <filesystem>
#include <memory>

namespace HuhuCapture::Transfer {
namespace {
constexpr auto ownerDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
using Files::nativePath;
using Files::qtPath;
using Buffer = std::unique_ptr<HuhuBuffer, decltype(&huhu_buffer_free)>;

bool bounded(const QString& value, qsizetype bytes)
{
    return !value.isEmpty() && !value.contains(QChar::Null) && value.toUtf8().size() <= bytes;
}

bool fingerprintValid(const QString& value)
{
    return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
            (c >= QLatin1Char('A') && c <= QLatin1Char('F')) ||
            (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    });
}

bool addressValid(const QString& value)
{
    const QHostAddress address(value);
    return address.protocol() == QAbstractSocket::IPv4Protocol && !address.isNull() &&
        !address.isMulticast() && address.toIPv4Address() != 0 && address.toIPv4Address() != 0xffffffffu &&
        address.toString() == value;
}

IdentityError readPrivate(const QString& path, qsizetype limit, QByteArray& bytes, bool& exists,
    std::error_code& error)
{
    bytes.clear();
    exists = false;
    error.clear();
    if (!Platform::isPrivateStorage(nativePath(QFileInfo(path).absolutePath()), true, error))
        return IdentityError::Permissions;
    if (!Platform::isPrivateStorage(nativePath(path), false, error)) {
        if (error == std::errc::no_such_file_or_directory) { error.clear(); return IdentityError::None; }
        return IdentityError::Permissions;
    }
    exists = true;
    auto handle = Platform::openRegularFileReadOnly(nativePath(path), error);
    if (!handle) return IdentityError::Read;
    const auto before = Platform::readFileDescriptorSnapshot(handle.get(), error);
    if (!before || before->bytes > static_cast<std::uint64_t>(limit)) return IdentityError::Read;
    QFile input;
    if (!input.open(handle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return IdentityError::Read;
    bytes = input.read(limit + 1);
    if (input.error() != QFileDevice::NoError || bytes.size() > limit || !input.atEnd()) return IdentityError::Read;
    const auto after = Platform::readFileDescriptorSnapshot(handle.get(), error);
    if (!after) return IdentityError::ChangedOnDisk;
    const auto named = Platform::readFileSnapshot(nativePath(path), error);
    if (!named || *before != *after || *after != *named ||
        after->bytes != static_cast<std::uint64_t>(bytes.size())) return IdentityError::ChangedOnDisk;
    return IdentityError::None;
}

bool parseObject(const QByteArray& bytes, QJsonObject& result)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    result = document.object();
    return true;
}

bool trustValid(const QJsonObject& document)
{
    if (document.value(QStringLiteral("schema")).toInt(-1) != 1 ||
        !document.value(QStringLiteral("peers")).isArray()) return false;
    const auto peers = document.value(QStringLiteral("peers")).toArray();
    if (peers.size() > IdentityStore::maximumTrustedPeers) return false;
    QStringList seen;
    for (const auto& value : peers) {
        if (!value.isObject()) return false;
        const auto peer = value.toObject();
        const auto fingerprint = peer.value(QStringLiteral("fingerprint")).toString().toUpper();
        if (!fingerprintValid(fingerprint) || seen.contains(fingerprint) ||
            !bounded(peer.value(QStringLiteral("alias")).toString(), 256) ||
            !peer.value(QStringLiteral("endpoints")).isArray()) return false;
        seen.append(fingerprint);
        bool numeric{};
        peer.value(QStringLiteral("confirmedAt")).toString().toLongLong(&numeric);
        if (!numeric) return false;
        const auto endpoints = peer.value(QStringLiteral("endpoints")).toArray();
        if (endpoints.isEmpty() || endpoints.size() > IdentityStore::maximumEndpointsPerPeer) return false;
        for (const auto& endpointValue : endpoints) {
            if (!endpointValue.isObject()) return false;
            const auto endpoint = endpointValue.toObject();
            const auto port = endpoint.value(QStringLiteral("port"));
            const int number = port.toInt();
            if (!addressValid(endpoint.value(QStringLiteral("address")).toString()) || number < 1 ||
                number > 65535 || port.toDouble() != number) return false;
        }
    }
    return true;
}

IdentityError publishNew(const QString& path, const QByteArray& bytes, std::error_code& error)
{
    const auto directory = nativePath(QFileInfo(path).absolutePath());
    error.clear();
    const auto staging = Platform::createExclusiveTemporaryFile(directory, error);
    if (staging.empty()) return IdentityError::Write;
    const auto claimed = Platform::readFileSnapshot(staging, error);
    if (!claimed) return IdentityError::Write;
    bool published{};
    const auto cleanup = qScopeGuard([&] {
        if (published) return;
        std::error_code ignored;
        const auto current = Platform::readFileSnapshot(staging, ignored);
        if (current && current->identity == claimed->identity) std::filesystem::remove(staging, ignored);
    });
    auto handle = Platform::openOwnedEmptyFileForWrite(staging, claimed->identity, error);
    if (!handle) return IdentityError::Write;
    // Prepare only the new empty object, before it contains any identity bytes.
    // Its inherited DACL must already be private; the current user's ownership
    // must not depend on the process token's default owner.
    if (!Platform::preparePrivateStagingFile(handle.get(), error) ||
        !Platform::isPrivateStorage(staging, false, error)) return IdentityError::Permissions;
    QFile output;
    if (!output.open(handle.get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle) ||
        output.write(bytes) != bytes.size() || !output.flush()) return IdentityError::Write;
    output.close();
    if (output.error() != QFileDevice::NoError || !handle.close(error)) return IdentityError::Write;
    const auto committed = Platform::commitFileNoReplace(staging, nativePath(path));
    published = committed.committed();
    if (committed.durable()) return IdentityError::None;
    error = committed.error;
    if (committed.committed()) return IdentityError::DurabilityUnknown;
    return committed.error == std::errc::file_exists ? IdentityError::ChangedOnDisk : IdentityError::Write;
}

int takeBuffer(HuhuBuffer* raw, QByteArray& bytes, qsizetype maximum)
{
    Buffer owner(raw, huhu_buffer_free);
    HuhuSlice view{};
    const auto status = huhu_buffer_view(owner.get(), &view);
    if (status != HUHU_OK) return status;
    if (!view.data || !view.length || view.length > static_cast<std::size_t>(maximum)) return HUHU_LIMIT_EXCEEDED;
    bytes = QByteArray(reinterpret_cast<const char*>(view.data), static_cast<qsizetype>(view.length));
    return HUHU_OK;
}

int validateIdentity(const QByteArray& bytes, QJsonObject& identity)
{
    HuhuBuffer* output{};
    auto status = huhu_identity_validate(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        static_cast<std::size_t>(bytes.size()), &output);
    if (status != HUHU_OK) { huhu_buffer_free(output); return status; }
    QByteArray verified;
    status = takeBuffer(output, verified, 1024);
    QJsonObject info;
    if (status != HUHU_OK || !parseObject(verified, info) || !parseObject(bytes, identity)) return HUHU_INVALID_ARGUMENT;
    const auto fingerprint = info.value(QStringLiteral("fingerprint")).toString();
    if (!fingerprintValid(fingerprint)) return HUHU_INVALID_ARGUMENT;
    identity.insert(QStringLiteral("fingerprint"), fingerprint.toUpper());
    return HUHU_OK;
}
} // namespace

IdentityStore::IdentityStore(QString privateConfigDirectory)
{
    if (privateConfigDirectory.isEmpty() || privateConfigDirectory.contains(QChar::Null) ||
        !QDir::isAbsolutePath(privateConfigDirectory) || privateConfigDirectory.startsWith(QLatin1Char(':'))) return;
    // The application chooses the user config root, never a peer or UI path.
    directory_ = QDir(QDir::cleanPath(privateConfigDirectory)).filePath(QStringLiteral("lan"));
}

QString IdentityStore::identityPath() const
{ return directory_.isEmpty() ? QString() : QDir(directory_).filePath(QStringLiteral("identity.json")); }
QString IdentityStore::trustPath() const
{ return directory_.isEmpty() ? QString() : QDir(directory_).filePath(QStringLiteral("trusted-peers.json")); }

IdentityResult IdentityStore::current(IdentityError error, int status) const
{
    if (error != IdentityError::None) return {error, status, {}, {}, {}, false};
    return {error, status, identity_, trust_.value(QStringLiteral("peers")).toArray(),
        identity_.value(QStringLiteral("fingerprint")).toString(), false};
}

IdentityResult IdentityStore::loadOrCreate(std::stop_token stop)
{
    loaded_ = false;
    if (stop.stop_requested()) return current(IdentityError::Cancelled);
    if (directory_.isEmpty()) return current(IdentityError::Path);
    if (!QDir().mkpath(directory_, ownerDirectory)) return current(IdentityError::Path);
    std::error_code error;
    if (!Platform::isPrivateStorage(nativePath(directory_), true, error)) return current(IdentityError::Permissions, error.value());
    directory_ = QDir(directory_).canonicalPath();
    if (directory_.isEmpty()) return current(IdentityError::Path);
    QLockFile lock(identityPath() + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return current(IdentityError::Locked);
    bool exists{};
    auto failure = readPrivate(identityPath(), maximumIdentityBytes, identityBytes_, exists, error);
    if (failure != IdentityError::None) return current(failure, error.value());
    failure = readPrivate(trustPath(), maximumTrustBytes, trustBytes_, trustExisted_, error);
    if (failure != IdentityError::None) return current(failure, error.value());
    trust_ = {{QStringLiteral("schema"), 1}, {QStringLiteral("peers"), QJsonArray{}}};
    if (trustExisted_ && (!parseObject(trustBytes_, trust_) || !trustValid(trust_)))
        return current(IdentityError::InvalidTrust);
    if (!exists && trustExisted_) return current(IdentityError::InvalidIdentity);
    bool created{};
    if (!exists) {
        if (stop.stop_requested()) return current(IdentityError::Cancelled);
        HuhuBuffer* generated{};
        auto status = huhu_identity_generate(&generated);
        if (status != HUHU_OK) { huhu_buffer_free(generated); return current(IdentityError::Write, status); }
        status = takeBuffer(generated, identityBytes_, maximumIdentityBytes);
        if (status != HUHU_OK) return current(IdentityError::InvalidIdentity, status);
        status = validateIdentity(identityBytes_, identity_);
        if (status != HUHU_OK) return current(IdentityError::InvalidIdentity, status);
        if (stop.stop_requested()) return current(IdentityError::Cancelled);
        failure = publishNew(identityPath(), identityBytes_, error);
        if (failure != IdentityError::None) return current(failure, error.value());
        created = true;
    } else {
        const auto status = validateIdentity(identityBytes_, identity_);
        if (status != HUHU_OK) return current(IdentityError::InvalidIdentity, status);
    }
    loaded_ = true;
    auto result = current();
    result.created = created;
    return result;
}

IdentityResult IdentityStore::rememberPeer(const QString& address, quint16 port,
    const QString& fingerprint, const QString& alias, std::stop_token stop)
{
    if (!loaded_ || !addressValid(address) || !port || !fingerprintValid(fingerprint) || !bounded(alias, 256))
        return current(IdentityError::InvalidTrust);
    if (stop.stop_requested()) return current(IdentityError::Cancelled);
    QLockFile lock(trustPath() + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return current(IdentityError::Locked);
    QByteArray currentIdentity, currentTrust;
    bool identityExists{}, trustExists{};
    std::error_code error;
    auto failure = readPrivate(identityPath(), maximumIdentityBytes, currentIdentity, identityExists, error);
    if (failure != IdentityError::None) return current(failure, error.value());
    failure = readPrivate(trustPath(), maximumTrustBytes, currentTrust, trustExists, error);
    if (failure != IdentityError::None) return current(failure, error.value());
    if (!identityExists || identityBytes_ != currentIdentity || trustExisted_ != trustExists || trustBytes_ != currentTrust)
        return current(IdentityError::ChangedOnDisk);

    auto candidate = trust_;
    auto peers = candidate.value(QStringLiteral("peers")).toArray();
    const auto normalized = fingerprint.toUpper();
    QJsonObject peer;
    for (qsizetype index = peers.size(); index > 0; --index) {
        const auto entry = peers[index - 1].toObject();
        if (entry.value(QStringLiteral("fingerprint")).toString().compare(normalized, ::Qt::CaseInsensitive) == 0) {
            peer = entry; peers.removeAt(index - 1); break;
        }
    }
    auto endpoints = peer.value(QStringLiteral("endpoints")).toArray();
    for (qsizetype index = endpoints.size(); index > 0; --index) {
        const auto endpoint = endpoints[index - 1].toObject();
        if (endpoint.value(QStringLiteral("address")).toString() == address &&
            endpoint.value(QStringLiteral("port")).toInt() == port) endpoints.removeAt(index - 1);
    }
    endpoints.append(QJsonObject{{QStringLiteral("address"), address}, {QStringLiteral("port"), port}});
    while (endpoints.size() > maximumEndpointsPerPeer) endpoints.removeFirst();
    peer.insert(QStringLiteral("fingerprint"), normalized);
    peer.insert(QStringLiteral("alias"), alias);
    peer.insert(QStringLiteral("endpoints"), endpoints);
    peer.insert(QStringLiteral("confirmedAt"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    peers.append(peer);
    while (peers.size() > maximumTrustedPeers) peers.removeFirst();
    candidate.insert(QStringLiteral("peers"), peers);
    const auto encoded = QJsonDocument(candidate).toJson(QJsonDocument::Indented);
    if (encoded.size() > maximumTrustBytes || !trustValid(candidate)) return current(IdentityError::InvalidTrust);
    if (stop.stop_requested()) return current(IdentityError::Cancelled);
    if (!trustExisted_) {
        failure = publishNew(trustPath(), encoded, error);
        if (failure != IdentityError::None) return current(failure, error.value());
    } else {
        QSaveFile output(trustPath());
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) return current(IdentityError::Write);
        // QSaveFile owns a distinct empty temporary object while the old trust
        // file remains intact. Establish its owner before writing, too.
        if (!Platform::preparePrivateStagingFile(output.handle(), error))
            return current(IdentityError::Permissions, error.value());
        if (output.write(encoded) != encoded.size()) return current(IdentityError::Write);
        QByteArray latest;
        bool stillExists{};
        failure = readPrivate(trustPath(), maximumTrustBytes, latest, stillExists, error);
        if (failure != IdentityError::None) return current(failure, error.value());
        if (!stillExists || latest != trustBytes_) return current(IdentityError::ChangedOnDisk);
        if (stop.stop_requested()) return current(IdentityError::Cancelled);
        if (!output.commit()) return current(IdentityError::Write);
    }
    trust_ = std::move(candidate);
    trustBytes_ = encoded;
    trustExisted_ = true;
    return current();
}

} // namespace HuhuCapture::Transfer
