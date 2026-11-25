#include "services/config/JsonText.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringDecoder>

namespace HuhuCapture::Config {

JsonTextResult parseJsonObject(const QByteArray& bytes, QJsonObject& output)
{
    if (bytes.size() > maximumJsonBytes)
        return {TextError::Size, QStringLiteral("JSON exceeds the 8 MiB configuration limit")};

    auto encoding = QStringDecoder::Utf8;
    qsizetype skip = 0;
    if (bytes.startsWith(QByteArray::fromHex("fffe"))) {
        encoding = QStringDecoder::Utf16LE;
        skip = 2;
    } else if (bytes.startsWith(QByteArray::fromHex("feff"))) {
        encoding = QStringDecoder::Utf16BE;
        skip = 2;
    } else if (bytes.startsWith(QByteArray::fromHex("efbbbf"))) {
        skip = 3;
    }
    if (encoding != QStringDecoder::Utf8 && (bytes.size() - skip) % 2 != 0)
        return {TextError::Encoding, QStringLiteral("Truncated UTF-16 code unit")};

    // Stateless conversion reports incomplete final sequences instead of
    // waiting for another input chunk that will never arrive.
    QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
    const QString text = decoder(QByteArrayView(bytes).sliced(skip));
    if (decoder.hasError())
        return {TextError::Encoding, QStringLiteral("Invalid or truncated Unicode encoding")};

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return {TextError::Json, QStringLiteral("JSON byte %1: %2")
            .arg(parseError.offset).arg(parseError.errorString())};
    if (!document.isObject())
        return {TextError::RootType, QStringLiteral("The JSON root must be an object")};
    output = document.object();
    return {};
}

} // namespace HuhuCapture::Config
