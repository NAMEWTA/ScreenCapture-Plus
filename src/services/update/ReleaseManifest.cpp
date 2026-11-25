#include "services/update/ReleaseManifest.h"

#include "app/Brand.h"
#include "core/ReleaseVersion.h"
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace HuhuCapture::Updates {
QString packageKindName(PackageKind kind)
{
    const auto value = Core::updatePackagePolicy(kind).kind;
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}
namespace Manifest {
namespace {
QString brand(std::wstring_view text) { return QString::fromWCharArray(text.data(), static_cast<qsizetype>(text.size())); }
bool windows(PackageKind kind) { return kind == PackageKind::WindowsInstaller || kind == PackageKind::WindowsPortableZip; }
QString platform(PackageKind kind)
{
    const auto value = Core::updatePackagePolicy(kind).platform;
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}
QString architecture(PackageKind kind)
{
    const auto value = Core::updatePackagePolicy(kind).architecture;
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}

bool bounded(const QString& value, qsizetype maximum)
{ return !value.isEmpty() && !value.contains(QChar::Null) && value.toUtf8().size() <= maximum; }
bool hex(const QString& value, qsizetype length)
{
    return value.size() == length && std::all_of(value.cbegin(), value.cend(), [](QChar c) {
        return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
    });
}
bool size(const QJsonValue& value, quint64 maximum, quint64& result, bool empty = false)
{
    if (!value.isDouble()) return false;
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < (empty ? 0 : 1) || number > maximum || std::floor(number) != number) return false;
    result = static_cast<quint64>(number); return true;
}
bool name(const QString& value)
{
    const auto encoded = value.toUtf8();
    return Core::updateAssetName(std::string_view(encoded.constData(), static_cast<std::size_t>(encoded.size())));
}
bool relativeFile(const QString& value)
{
    const auto encoded = value.toUtf8();
    return Core::updateInventoryPath(std::string_view(encoded.constData(), static_cast<std::size_t>(encoded.size())));
}
bool document(const QByteArray& bytes, quint64 maximum, QJsonObject& object)
{
    if (bytes.isEmpty() || static_cast<quint64>(bytes.size()) > maximum) return false;
    QJsonParseError error;
    const auto value = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !value.isObject()) return false;
    object = value.object(); return true;
}
bool identity(const QJsonObject& object, const QString& tag, const QString& version, const QString& commit)
{
    // Schema 1 permits these flags to be absent. A present flag must be an
    // actual false boolean: neither a development
    // bundle nor malformed truthy metadata can enter the stable update path.
    for (const auto* key : {"developmentOnly", "sourceDirty"}) {
        const auto value = object.value(QString::fromLatin1(key));
        if (!value.isUndefined() && value != QJsonValue(false)) return false;
    }
    return object.value(QStringLiteral("schemaVersion")) == QJsonValue(1) &&
        object.value(QStringLiteral("product")).toString() == brand(Brand::name) &&
        object.value(QStringLiteral("repository")).toString() == brand(Brand::repositoryLabel) &&
        object.value(QStringLiteral("tag")).toString() == tag &&
        object.value(QStringLiteral("version")).toString() == version &&
        object.value(QStringLiteral("sourceCommit")).toString() == commit && hex(commit, 40);
}
bool descriptor(const QJsonObject& object, quint64 maximum, const Release& release, Asset& output)
{
    output.name = object.value(QStringLiteral("name")).toString();
    if (!name(output.name) || !size(object.value(QStringLiteral("size")), maximum, output.size)) return false;
    const auto hash = object.value(QStringLiteral("sha256")).toString();
    if (!hex(hash, 64)) return false;
    output.sha256 = hash.toLatin1(); output.url = assetUrl(release.tag, output.name);
    const auto found = release.assets.constFind(output.name);
    return found != release.assets.cend() && found->size == output.size && found->sha256 == output.sha256 && found->url == output.url;
}
QString packageName(const QString& tag, PackageKind kind)
{
    const auto policy = Core::updatePackagePolicy(kind);
    if (policy.suffix.empty()) return {};
    return brand(Brand::name) + u'-' + tag + QString::fromLatin1(policy.suffix.data(), static_cast<qsizetype>(policy.suffix.size()));
}
bool safeUrl(const QUrl& value)
{ return value.isValid() && value.userInfo().isEmpty() && value.fragment().isEmpty(); }
} // namespace

QUrl latestUrl() { return QUrl(brand(Brand::latestReleaseApiUrl)); }
QUrl assetUrl(const QString& tag, const QString& name)
{ return QUrl(brand(Brand::releaseAssetPrefix) + tag + u'/' + name); }

bool canonicalVersion(const QString& value, bool tag)
{
    return Core::canonicalUpdateVersion(value.toStdWString(), tag);
}

bool parseRelease(const QByteArray& bytes, Release& result)
{
    QJsonObject object;
    if (!document(bytes, maximumMetadataBytes, object) || !object.value(QStringLiteral("draft")).isBool() ||
        object.value(QStringLiteral("draft")).toBool() || !object.value(QStringLiteral("prerelease")).isBool() ||
        object.value(QStringLiteral("prerelease")).toBool()) return false;
    Release candidate;
    candidate.tag = object.value(QStringLiteral("tag_name")).toString();
    if (!canonicalVersion(candidate.tag, true) || object.value(QStringLiteral("html_url")).toString() !=
        brand(Brand::releasesUrl) + QStringLiteral("/tag/") + candidate.tag) return false;
    candidate.version = candidate.tag.mid(1);
    const auto list = object.value(QStringLiteral("assets"));
    if (!list.isArray() || list.toArray().isEmpty() || list.toArray().size() > 128) return false;
    for (const auto& value : list.toArray()) {
        if (!value.isObject()) return false;
        const auto item = value.toObject();
        Asset asset; asset.name = item.value(QStringLiteral("name")).toString();
        if (!name(asset.name) || candidate.assets.contains(asset.name) ||
            !size(item.value(QStringLiteral("size")), maximumPackageBytes, asset.size)) return false;
        const auto digest = item.value(QStringLiteral("digest")).toString();
        if (!digest.startsWith(QStringLiteral("sha256:")) || !hex(digest.mid(7), 64)) return false;
        asset.sha256 = digest.mid(7).toLatin1();
        asset.url = QUrl(item.value(QStringLiteral("browser_download_url")).toString());
        if (asset.url != assetUrl(candidate.tag, asset.name)) return false;
        candidate.assets.insert(asset.name, asset);
    }
    const auto manifest = candidate.assets.constFind(QStringLiteral("release-manifest.json"));
    if (manifest == candidate.assets.cend() || manifest->size > maximumMetadataBytes) return false;
    result = std::move(candidate); return true;
}

bool selectPackage(const QByteArray& bytes, const Release& release, PackageKind kind, Selection& result)
{
    QJsonObject object;
    if (!document(bytes, maximumMetadataBytes, object)) return false;
    const auto commit = object.value(QStringLiteral("sourceCommit")).toString();
    if (!identity(object, release.tag, release.version, commit)) return false;
    const auto assets = object.value(QStringLiteral("assets"));
    if (!assets.isArray() || assets.toArray().isEmpty() || assets.toArray().size() > 16) return false;
    std::optional<Selection> found;
    for (const auto& value : assets.toArray()) {
        if (!value.isObject()) return false;
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("platform")).toString() != platform(kind) ||
            entry.value(QStringLiteral("architecture")).toString() != architecture(kind) ||
            entry.value(QStringLiteral("packageKind")).toString() != packageKindName(kind)) continue;
        if (found) return false;
        Selection selected;
        selected.kind = kind; selected.tag = release.tag; selected.version = release.version; selected.sourceCommit = commit;
        selected.minimumSystem = entry.value(QStringLiteral("minimumSystem")).toString();
        if (!bounded(selected.minimumSystem, 256) || !descriptor(entry, packageBudget(kind), release, selected.package) ||
            selected.package.name != packageName(release.tag, kind) || !entry.value(QStringLiteral("buildInfo")).isObject() ||
            !descriptor(entry.value(QStringLiteral("buildInfo")).toObject(), maximumBuildInfoBytes, release, selected.buildInfo) ||
            selected.buildInfo.name != QStringLiteral("build-info-") + platform(kind) + u'-' + architecture(kind) + QStringLiteral(".json")) return false;
        found = std::move(selected);
    }
    if (!found) return false;
    result = std::move(*found); return true;
}

bool validateBuildInfo(const QByteArray& bytes, const Selection& selected)
{
    QJsonObject object;
    if (!document(bytes, maximumBuildInfoBytes, object) || !identity(object, selected.tag, selected.version, selected.sourceCommit) ||
        object.value(QStringLiteral("platform")).toString() != platform(selected.kind) ||
        object.value(QStringLiteral("architecture")).toString() != architecture(selected.kind)) return false;
    const auto dependencies = object.value(QStringLiteral("dependencies"));
    if (!dependencies.isArray() || dependencies.toArray().isEmpty() || dependencies.toArray().size() > 128) return false;
    QSet<QString> names;
    for (const auto& value : dependencies.toArray()) {
        if (!value.isObject()) return false;
        const auto item = value.toObject();
        const auto dependency = item.value(QStringLiteral("name")).toString();
        if (!name(dependency) || names.contains(dependency) || !bounded(item.value(QStringLiteral("version")).toString(), 128) ||
            !bounded(item.value(QStringLiteral("license")).toString(), 1024) ||
            !bounded(item.value(QStringLiteral("source")).toString(), 2048)) return false;
        const QUrl source(item.value(QStringLiteral("source")).toString());
        if (!safeUrl(source) || source.scheme() != QStringLiteral("https") || source.host().isEmpty()) return false;
        names.insert(dependency);
    }
    for (const auto* required : {"qt", "gifski", "localsend", "ffmpeg"})
        if (!names.contains(QString::fromLatin1(required))) return false;
    // The two package formats have distinct deployment trees and installation
    // markers. Bind this selection before accepting any required file; another
    // package's Qt/plugins cannot make an incomplete selected tree acceptable.
    const auto packages = object.value(QStringLiteral("packages"));
    if (!packages.isArray() || packages.toArray().isEmpty() || packages.toArray().size() > 16) return false;
    unsigned matches{};
    QSet<QString> packageNames;
    QString contentPrefix;
    for (const auto& value : packages.toArray()) {
        if (!value.isObject()) return false;
        const auto item = value.toObject();
        const auto packageName = item.value(QStringLiteral("name")).toString();
        if (!name(packageName) || packageNames.contains(packageName)) return false;
        packageNames.insert(packageName);
        if (packageName != selected.package.name) continue;
        const auto contentRoot = item.value(QStringLiteral("contentRoot"));
        quint64 length{};
        if (item.value(QStringLiteral("packageKind")).toString() != packageKindName(selected.kind) ||
            !contentRoot.isString() || contentRoot.toString() != packageKindName(selected.kind) ||
            !size(item.value(QStringLiteral("size")), packageBudget(selected.kind), length) || length != selected.package.size ||
            item.value(QStringLiteral("sha256")).toString().toLatin1() != selected.package.sha256) return false;
        contentPrefix = contentRoot.toString() + u'/';
        ++matches;
    }
    if (matches != 1) return false;
    const auto files = object.value(QStringLiteral("files"));
    if (!files.isArray() || files.toArray().isEmpty() || files.toArray().size() > 8192) return false;
    QSet<QString> paths;
    bool executable{}, qt{}, platformPlugin{}, wayland{};
    for (const auto& value : files.toArray()) {
        if (!value.isObject()) return false;
        const auto item = value.toObject();
        const auto path = item.value(QStringLiteral("path")).toString();
        const auto comparisonPath = windows(selected.kind) ? path.toCaseFolded() : path;
        quint64 length{};
        if (!relativeFile(path) || paths.contains(comparisonPath) || !size(item.value(QStringLiteral("size")), maximumPackageBytes, length, true) ||
            !hex(item.value(QStringLiteral("sha256")).toString(), 64)) return false;
        paths.insert(comparisonPath);
        if (!path.startsWith(contentPrefix, windows(selected.kind) ? Qt::CaseInsensitive : Qt::CaseSensitive)) continue;
        const auto relative = path.mid(contentPrefix.size());
        if (!relativeFile(relative)) return false;
        const auto leaf = relative.mid(relative.lastIndexOf(u'/') + 1);
        executable = executable || (length && leaf == (windows(selected.kind) ? QStringLiteral("huhucapture.exe") : QStringLiteral("huhucapture")));
        qt = qt || (length && (windows(selected.kind) ? leaf == QStringLiteral("Qt6Core.dll") :
            leaf == QStringLiteral("libQt6Core.so") || leaf.startsWith(QStringLiteral("libQt6Core.so."))));
        if ((relative.startsWith(QStringLiteral("platforms/")) || relative.contains(QStringLiteral("/platforms/"))) && length) {
            platformPlugin = platformPlugin || leaf == (windows(selected.kind) ? QStringLiteral("qwindows.dll") : QStringLiteral("libqxcb.so"));
            wayland = wayland || (leaf.startsWith(QStringLiteral("libqwayland")) && leaf.endsWith(QStringLiteral(".so")));
        }
    }
    return executable && qt && platformPlugin && (windows(selected.kind) || wayland);
}

bool validTestOrigin(const QUrl& origin)
{
    const auto host = origin.host();
    return safeUrl(origin) && origin.scheme() == QStringLiteral("http") && origin.port() > 0 &&
        origin.query().isEmpty() && (origin.path().isEmpty() || origin.path() == QStringLiteral("/")) &&
        (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1"));
}
QUrl requestUrl(const QUrl& canonical, const std::optional<QUrl>& testOrigin)
{
    if (!testOrigin) return canonical;
    if (!validTestOrigin(*testOrigin)) return {};
    auto result = *testOrigin; result.setPath(canonical.path()); result.setQuery(canonical.query()); return result;
}
bool allowedRedirect(const QUrl& next, const QUrl& canonical, const std::optional<QUrl>& testOrigin)
{
    if (!safeUrl(next)) return false;
    if (testOrigin) return validTestOrigin(*testOrigin) && next.scheme() == testOrigin->scheme() &&
        next.host() == testOrigin->host() && next.port() == testOrigin->port();
    if (next.scheme() != QStringLiteral("https") || (next.port(-1) != -1 && next.port() != 443)) return false;
    if (canonical == latestUrl()) return next == canonical;
    if (next.host() == QStringLiteral("github.com")) return next == canonical;
    return next.host() == QStringLiteral("release-assets.githubusercontent.com") || next.host() == QStringLiteral("objects.githubusercontent.com");
}
} // namespace Manifest
} // namespace HuhuCapture::Updates
