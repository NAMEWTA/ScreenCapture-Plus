#pragma once

#include "platform/PrivateStorage.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <stop_token>

namespace HuhuCapture::Transfer {

enum class IdentityError {
    None, Cancelled, Path, Permissions, Locked, Read, InvalidIdentity,
    InvalidTrust, ChangedOnDisk, Write, DurabilityUnknown,
};

enum class IdentityFilePhase {
    None, CreateDirectory, DirectoryPermissions, ReadDirectory, ReadPermissions,
    ReadOpen, ReadSnapshot, ReadContents, ReadRecheck, CreateTemporary,
    ClaimTemporary, OpenTemporary, InitializeTemporary, WriteTemporary,
    CloseTemporary, PublishTemporary, OpenReplacement, ClaimReplacement,
    InitializeReplacement, WriteReplacement, CommitReplacement,
};

// Safe diagnostic state for file failures; nativeStatus below remains the Rust
// status. No OS error is invented for a Qt operation that exposes only a Qt code.
struct IdentityFileDiagnostic {
    IdentityFilePhase phase{IdentityFilePhase::None};
    std::error_code error;
    int qtFileError{};
    Platform::PrivateFileCreation creation;
};

struct IdentityResult {
    IdentityError error{IdentityError::None};
    int nativeStatus{};
    // The native JSON contains the private key. Never log, put in a Qt signal,
    // or retain it in a presentation snapshot. Only the app builds native config.
    QJsonObject nativeIdentity;
    QJsonArray trustedPeers;
    QString fingerprint;
    bool created{};
    IdentityFileDiagnostic fileDiagnostic;
    bool ok() const noexcept { return error == IdentityError::None; }
};

// One application worker owns this synchronous store. Config/alias/interface
// preferences remain in the application's shared SettingsStore. Private TLS
// keys and explicitly confirmed fingerprints remain in this private directory,
// including when the ordinary portable config lives beside the executable.
class IdentityStore final {
public:
    explicit IdentityStore(QString privateConfigDirectory);
    IdentityResult loadOrCreate(std::stop_token stop = {});
    IdentityResult rememberPeer(const QString& address, quint16 port,
        const QString& fingerprint, const QString& alias, std::stop_token stop = {});
    QString identityPath() const;
    QString trustPath() const;

    static constexpr qsizetype maximumIdentityBytes = 65536;
    static constexpr qsizetype maximumTrustBytes = 256 * 1024;
    static constexpr qsizetype maximumTrustedPeers = 128;
    static constexpr qsizetype maximumEndpointsPerPeer = 8;
private:
    IdentityResult current(IdentityError error = IdentityError::None, int status = 0) const;
    QString directory_;
    QByteArray identityBytes_;
    QByteArray trustBytes_;
    QJsonObject identity_;
    QJsonObject trust_;
    bool loaded_{};
    bool trustExisted_{};
    IdentityFileDiagnostic diagnostic_;
};

} // namespace HuhuCapture::Transfer
