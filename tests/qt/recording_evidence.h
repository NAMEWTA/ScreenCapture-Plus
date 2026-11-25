#pragma once

#include "platform/FileIdentity.h"
#include "services/files/QtPaths.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>

#include <algorithm>
#include <stdexcept>

namespace HuhuCapture::Tests {

// CI omits hidden staging files. Retain a verified, non-hidden copy in the
// probe's exclusive run directory before an inspection assertion can throw.
// The original recovery file remains owned by the archive and is never moved,
// renamed, rewritten or removed by this diagnostic operation.
inline QJsonObject retainRecordingEvidence(const QString& originalPath, const QString& copyPath)
{
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    std::error_code error;
    const auto originalNative = Files::nativePath(originalPath);
    const auto copyNative = Files::nativePath(copyPath);
    auto originalHandle = Platform::openRegularFileReadOnly(originalNative, error);
    require(originalHandle && !error, "Open the original recovery file without following links");
    const auto original = Platform::readFileDescriptorSnapshot(originalHandle.get(), error);
    require(original && !error && original->bytes && original->bytes <= 256ULL * 1024 * 1024,
        "The short native recording evidence must be a bounded nonempty regular file");
    require(Platform::readFileSnapshot(originalNative, error) == original && !error,
        "The recovery path must still identify the opened original file");

    QFile input;
    require(input.open(originalHandle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle),
        "Borrow the unchanged recovery file descriptor for evidence");
    QFile output(copyPath);
    require(output.open(QIODevice::ReadWrite | QIODevice::NewOnly),
        "Exclusively create the non-hidden recording evidence copy");
    const auto copyIdentity = Platform::readFileDescriptorSnapshot(output.handle(), error);
    require(copyIdentity && !error && !copyIdentity->bytes && copyIdentity->identity != original->identity,
        "The diagnostic copy must be a distinct empty regular file");

    QCryptographicHash originalHash(QCryptographicHash::Sha256);
    std::uint64_t copied{};
    while (copied < original->bytes) {
        const auto block = input.read(static_cast<qint64>((std::min)(original->bytes - copied, std::uint64_t{65536})));
        require(!block.isEmpty() && input.error() == QFileDevice::NoError,
            "Read every original recording byte into its diagnostic copy");
        originalHash.addData(block);
        qint64 written{};
        while (written < block.size()) {
            const auto count = output.write(block.constData() + written, block.size() - written);
            require(count > 0, "Write every diagnostic recording byte");
            written += count;
        }
        copied += static_cast<std::uint64_t>(block.size());
    }
    require(input.read(1).isEmpty() && input.error() == QFileDevice::NoError,
        "The original recording must not grow while retaining evidence");
    require(output.flush() && output.seek(0), "Flush and rewind the actual diagnostic copy");
    QCryptographicHash copyHash(QCryptographicHash::Sha256);
    require(copyHash.addData(&output) && copyHash.result() == originalHash.result(),
        "The actual diagnostic copy must match the original recording SHA-256");
    const auto copy = Platform::readFileDescriptorSnapshot(output.handle(), error);
    require(copy && !error && copy->identity == copyIdentity->identity && copy->bytes == original->bytes,
        "The diagnostic copy must retain its own identity and the complete original length");
    require(Platform::readFileSnapshot(copyNative, error) == copy && !error,
        "The diagnostic pathname must still identify the verified copy");
    require(Platform::readFileDescriptorSnapshot(originalHandle.get(), error) == original && !error &&
        Platform::readFileSnapshot(originalNative, error) == original && !error,
        "The original recovery file identity, length and revision must remain unchanged");

    QJsonObject identity;
    // File identities are opaque 64-bit values, not JSON floating point counts.
    identity[QStringLiteral("volume")] = QString::number(original->identity.volume);
    identity[QStringLiteral("file")] = QString::number(original->identity.file);
    QJsonObject result;
    result[QStringLiteral("path")] = copyPath;
    result[QStringLiteral("bytes")] = static_cast<qint64>(original->bytes);
    result[QStringLiteral("sha256")] = QString::fromLatin1(originalHash.result().toHex());
    result[QStringLiteral("originalIdentity")] = identity;
    result[QStringLiteral("originalUnchanged")] = true;
    result[QStringLiteral("copyVerified")] = true;
    return result;
}

} // namespace HuhuCapture::Tests
