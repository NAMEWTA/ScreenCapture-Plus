#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace HuhuCapture::Config {

// Both legacy config.json and external Lang files were written as UTF-16LE
// with a BOM. A decoding failure is an error, never an empty/default document.
enum class TextError { None, Encoding, Json, RootType, Size, EmptyDocument };

struct JsonTextResult {
    TextError error{TextError::None};
    QString detail;
    bool ok() const noexcept { return error == TextError::None; }
};

inline constexpr qsizetype maximumJsonBytes = 8 * 1024 * 1024;
JsonTextResult parseJsonObject(const QByteArray& bytes, QJsonObject& output);

} // namespace HuhuCapture::Config
