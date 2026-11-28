#include "services/localization/LanguageCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonValue>
#include <QResource>

#include <array>
#include <utility>

// Q_INIT_RESOURCE must be outside a C++ namespace. The explicit reference also
// retains the qrc object when this service is linked as a static library.
static void initializeLanguageResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(huhucapture_languages);
        return true;
    }();
    (void)initialized;
}

namespace HuhuCapture::Localization {
namespace {

struct Builtin {
    const char16_t* name;
    const char16_t* code;
    const char16_t* file;
};

constexpr std::array<Builtin, 6> builtins{{
    {u"简体中文", u"zh-CN", u"简体中文.zh-CN.json"},
    {u"English", u"en-US", u"English.en-US.json"},
    {u"繁體中文", u"zh-TW", u"繁體中文.zh-TW.json"},
    {u"日本語", u"ja-JP", u"日本語.ja-JP.json"},
    {u"Русский", u"ru-RU", u"Русский.ru-RU.json"},
    {u"Bahasa Indonesia", u"id-ID", u"Bahasa Indonesia.id-ID.json"},
}};

QString resourcePath(const Builtin& entry)
{
    return QStringLiteral(":/huhucapture/Lang/") + QString::fromUtf16(entry.file);
}

QString builtinPath(const QString& code)
{
    for (const auto& entry : builtins)
        if (QStringView(entry.code) == code) return resourcePath(entry);
    return {};
}

Config::JsonTextResult readCatalog(const QString& path, QJsonObject& output)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {Config::TextError::EmptyDocument, file.errorString()};
    if (file.size() > Config::maximumJsonBytes)
        return {Config::TextError::Size, QStringLiteral("Language file exceeds the JSON size budget")};
    const auto data = file.read(Config::maximumJsonBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return {Config::TextError::EmptyDocument, file.errorString()};
    return Config::parseJsonObject(data, output);
}

QString lookup(const QJsonObject& object, QStringView key)
{
    const auto sections = key.toString().split(u'.');
    if (sections.size() < 2) return {};
    QJsonValue value(object);
    for (const auto& section : sections) {
        if (section.isEmpty() || !value.isObject()) return {};
        value = value.toObject().value(section);
    }
    return value.isString() ? value.toString() : QString{};
}

} // namespace

LanguageCatalog::LanguageCatalog(LanguagePaths paths, QObject* parent)
    : QObject(parent), paths_(std::move(paths))
{
    initializeLanguageResources();
}

QVector<LanguageEntry> LanguageCatalog::externalEntries() const
{
    QString directory;
    if (paths_.policy == ResourcePolicy::WindowsDirectoryFallback) {
        const auto portable = QDir(paths_.applicationDirectory).filePath(QStringLiteral("Lang"));
        // Presence of this directory, including an empty one, suppresses the
        // user fallback. Never combine the two Windows resource directories.
        directory = QFileInfo(portable).isDir() ? portable
            : QDir(paths_.userDataDirectory).filePath(QStringLiteral("Lang"));
    } else {
        directory = QDir(paths_.userDataDirectory).filePath(QStringLiteral("Lang"));
    }
    QVector<LanguageEntry> entries;
    const auto files = QDir(directory).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& file : files) {
        const auto parts = file.fileName().split(u'.');
        if (parts.size() != 3 || parts[0].isEmpty() || parts[1].isEmpty()
            || parts[2] != QStringLiteral("json")) continue;
        if (parts[1] == QStringLiteral("zh-CN") || parts[1] == QStringLiteral("en-US")) continue;
        // Deterministic first entry for a code, instead of an ambiguous
        // substring match such as "en" accidentally loading "en-US".
        bool duplicate = false;
        for (const auto& entry : entries) if (entry.code == parts[1]) { duplicate = true; break; }
        if (!duplicate) entries.append({parts[0], parts[1], file.absoluteFilePath()});
    }
    return entries;
}

QVector<LanguageEntry> LanguageCatalog::available() const
{
    QVector<LanguageEntry> entries;
    const auto count = paths_.policy == ResourcePolicy::LinuxUserResources ? builtins.size() : 2;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& entry = builtins[index];
        entries.append({QString::fromUtf16(entry.name), QString::fromUtf16(entry.code), resourcePath(entry)});
    }
    for (const auto& entry : externalEntries()) {
        bool replaced = false;
        for (auto& existing : entries) {
            if (existing.code == entry.code) { existing = entry; replaced = true; break; }
        }
        if (!replaced) entries.append(entry);
    }
    return entries;
}

LanguageResult LanguageCatalog::load(const QString& code)
{
    LanguageResult result;
    result.requestedCode = code;
    QJsonObject english;
    result.error = readCatalog(builtinPath(QStringLiteral("en-US")), english);
    if (!result.error.ok()) return result; // Keep a previously working catalog.
    QString source;
    if (code == QStringLiteral("zh-CN") || code == QStringLiteral("en-US")) source = builtinPath(code);
    else {
        for (const auto& entry : externalEntries())
            if (entry.code == code) { source = entry.source; break; }
        if (source.isEmpty() && paths_.policy == ResourcePolicy::LinuxUserResources) source = builtinPath(code);
    }
    QJsonObject selected;
    if (source.isEmpty())
        result.error = {Config::TextError::EmptyDocument, QStringLiteral("Selected language file was not found")};
    else result.error = readCatalog(source, selected);
    result.fellBack = !result.error.ok();
    result.effectiveCode = result.fellBack ? QStringLiteral("en-US") : code;
    result.source = result.fellBack ? builtinPath(QStringLiteral("en-US")) : source;
    current_ = result.fellBack ? english : selected;
    english_ = std::move(english);
    requested_ = code;
    effective_ = result.effectiveCode;
    cache_.clear();
    result.usable = true;
    Q_EMIT changed();
    return result;
}

QString LanguageCatalog::text(QStringView key) const
{
    const auto ownedKey = key.toString();
    const auto cached = cache_.constFind(ownedKey);
    if (cached != cache_.cend()) return cached.value();
    auto value = lookup(current_, key);
    if (value.isEmpty()) value = lookup(english_, key);
    if (value.isEmpty()) value = ownedKey;
    cache_.insert(ownedKey, value);
    return value;
}

} // namespace HuhuCapture::Localization
