#pragma once

#include "ui/qt/DesktopTypes.h"
#include "services/transfer/TransferService.h"
#include <QJsonArray>
#include <QLocale>
#include <limits>

namespace HuhuCapture::UI::Qt::TransferPresentation {

inline QString text(const DesktopTranslator& translator, QStringView key)
{ return translator ? translator(key) : key.toString(); }

inline QString bytes(std::uint64_t value)
{
    if (value < 1024) return QString::number(static_cast<qulonglong>(value)) + QStringLiteral(" B");
    if (value < 1024 * 1024) return QLocale().toString(static_cast<double>(value) / 1024, 'f', 1) + QStringLiteral(" KiB");
    if (value < 1024ull * 1024 * 1024) return QLocale().toString(static_cast<double>(value) / (1024 * 1024), 'f', 1) + QStringLiteral(" MiB");
    return QLocale().toString(static_cast<double>(value) / (1024ull * 1024 * 1024), 'f', 1) + QStringLiteral(" GiB");
}

inline QString status(const DesktopTranslator& translator, int value, int httpStatus = 0)
{
    QStringView key = u"lan.failed";
    switch (httpStatus) {
    case 401: key = u"lan.pinRequired"; break;
    case 403: key = u"lan.declined"; break;
    case 409: key = u"lan.busy"; break;
    case 413: key = u"lan.limit"; break;
    case 422: key = u"lan.integrity"; break;
    case 507: key = u"lan.storage"; break;
    default:
        switch (value) {
        case HUHU_OK: key = u"lan.completed"; break;
        case HUHU_PARTIAL: key = u"lan.partial"; break;
        case HUHU_TIMEOUT: key = u"lan.timeout"; break;
        case HUHU_INVALID_ARGUMENT: case HUHU_INVALID_STATE: key = u"lan.invalidState"; break;
        case HUHU_IO: key = u"lan.ioFailure"; break;
        case HUHU_CANCELLED: key = u"lan.cancelled"; break;
        case HUHU_BUSY: key = u"lan.busy"; break;
        case HUHU_PROTOCOL: key = u"lan.connectionFailure"; break;
        case HUHU_CHECKSUM_MISMATCH: case HUHU_LENGTH_MISMATCH: key = u"lan.integrity"; break;
        case HUHU_INVALID_MEDIA: key = u"lan.invalidMedia"; break;
        case HUHU_DURABILITY_UNKNOWN: key = u"lan.durabilityUnknown"; break;
        case HUHU_CLOSED: key = u"lan.closed"; break;
        case HUHU_LIMIT_EXCEEDED: key = u"lan.limit"; break;
        case HUHU_ADDRESS_IN_USE: key = u"lan.portInUse"; break;
        default: break;
        }
    }
    return text(translator, key);
}

inline QString kind(std::uint32_t value)
{
    switch (value) { case HUHU_PNG: return QStringLiteral("PNG"); case HUHU_GIF: return QStringLiteral("GIF");
        case HUHU_MP4: return QStringLiteral("MP4"); default: return {}; }
}

inline QString kind(const QJsonObject& file)
{
    const auto mime = file.value(QStringLiteral("fileType")).toString();
    if (mime == QStringLiteral("image/png")) return QStringLiteral("PNG");
    if (mime == QStringLiteral("image/gif")) return QStringLiteral("GIF");
    if (mime == QStringLiteral("video/mp4")) return QStringLiteral("MP4");
    return {};
}

inline QString endpoint(const QJsonObject& peer)
{ return peer.value(QStringLiteral("address")).toString() + u':' + QString::number(peer.value(QStringLiteral("port")).toInt()); }

inline bool fingerprint(const QString& value)
{
    if (value.size() != 64) return false;
    for (const auto character : value)
        if (!((character >= u'0' && character <= u'9') || (character >= u'A' && character <= u'F') ||
            (character >= u'a' && character <= u'f'))) return false;
    return true;
}

inline QString taskKey(const Transfer::Event& event)
{
    const auto session = event.receiveProgress ? event.receiveProgress->sessionId :
        event.data.value(QStringLiteral("sessionId")).toString();
    return QString::number(static_cast<qulonglong>(event.generation)) + (session.isEmpty() ? QStringLiteral("/send/") + QString::number(static_cast<qulonglong>(event.request)) :
        QStringLiteral("/receive/") + session);
}

inline QString serviceState(const DesktopTranslator& translator, Transfer::ServiceState value)
{
    switch (value) {
    case Transfer::ServiceState::Disabled: return text(translator, u"lan.disabled");
    case Transfer::ServiceState::Starting: return text(translator, u"lan.starting");
    case Transfer::ServiceState::Enabled: return text(translator, u"lan.enabled");
    case Transfer::ServiceState::Stopping: return text(translator, u"lan.stopping");
    case Transfer::ServiceState::Closed: return text(translator, u"lan.closed");
    }
    return {};
}
} // namespace HuhuCapture::UI::Qt::TransferPresentation
