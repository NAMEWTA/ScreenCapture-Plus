#include "services/update/ReleaseManifest.h"

#include "app/Brand.h"
#include "core/UpdateManifest.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace HuhuCapture::Updates {
QString packageKindName(PackageKind kind)
{
    const auto value = Core::updatePackagePolicy(kind).kind;
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}
namespace Manifest {
namespace {
namespace Policy = Core::UpdateManifest;
QString brand(std::wstring_view text) { return QString::fromWCharArray(text.data(), static_cast<qsizetype>(text.size())); }
QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
Policy::Identity identity()
{
    return {utf8(brand(Brand::name)), utf8(brand(Brand::repositoryLabel)),
        utf8(brand(Brand::releasesUrl)), utf8(brand(Brand::releaseAssetPrefix))};
}
bool safeUrl(const QUrl& value)
{ return value.isValid() && value.userInfo().isEmpty() && value.fragment().isEmpty(); }

class JsonView final {
public:
    explicit JsonView(QJsonValue value = QJsonValue(QJsonValue::Undefined)) : value_(std::move(value)) {}
    bool isObject() const { return value_.isObject(); }
    bool isArray() const { return value_.isArray(); }
    bool isNumber() const { return value_.isDouble(); }
    bool isBoolean() const { return value_.isBool(); }
    bool missing() const { return value_.isUndefined(); }
    double number() const { return value_.toDouble(); }
    bool boolean() const { return value_.toBool(); }
    std::string string() const { return utf8(value_.toString()); }
    std::size_t count() const { return static_cast<std::size_t>(value_.toArray().size()); }
    JsonView value(const char* key) const { return JsonView(value_.toObject().value(QString::fromLatin1(key))); }
    JsonView at(std::size_t index) const { return JsonView(value_.toArray().at(static_cast<qsizetype>(index))); }
    static bool validHttpsSource(const std::string& value)
    {
        const QUrl source(text(value));
        return safeUrl(source) && source.scheme() == QStringLiteral("https") && !source.host().isEmpty();
    }
    static std::optional<std::string> comparisonPath(const std::string& value, bool windows)
    { return windows ? utf8(text(value).toCaseFolded()) : value; }
private:
    QJsonValue value_;
};

bool document(const QByteArray& bytes, quint64 maximum, JsonView& view)
{
    if (bytes.isEmpty() || static_cast<quint64>(bytes.size()) > maximum) return false;
    QJsonParseError error;
    const auto value = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !value.isObject()) return false;
    view = JsonView(value.object()); return true;
}
Asset fromCore(const Policy::Asset& asset)
{ return {text(asset.name), asset.size, QByteArray::fromStdString(asset.sha256), QUrl(text(asset.url))}; }
Policy::Asset toCore(const Asset& asset)
{ return {utf8(asset.name), asset.sha256.toStdString(), utf8(asset.url.toString(QUrl::FullyEncoded)), asset.size}; }
Policy::Release toCore(const Release& value)
{
    Policy::Release result; result.version = utf8(value.version); result.tag = utf8(value.tag);
    for (auto it = value.assets.cbegin(); it != value.assets.cend(); ++it) result.assets.emplace(utf8(it.key()), toCore(it.value()));
    return result;
}
Policy::Selection toCore(const Selection& value)
{
    return {toCore(value.package), toCore(value.buildInfo), utf8(value.version), utf8(value.tag),
        utf8(value.sourceCommit), utf8(value.minimumSystem), value.kind};
}
} // namespace

QUrl latestUrl() { return QUrl(brand(Brand::latestReleaseApiUrl)); }
QUrl assetUrl(const QString& tag, const QString& name)
{ return QUrl(text(Policy::assetUrl(identity(), utf8(tag), utf8(name)))); }

bool canonicalVersion(const QString& value, bool tag)
{ return Policy::canonicalVersion(utf8(value), tag); }

bool parseRelease(const QByteArray& bytes, Release& result)
{
    JsonView object;
    Policy::Release parsed;
    if (!document(bytes, maximumMetadataBytes, object) || !Policy::parseRelease(object, identity(), parsed)) return false;
    Release candidate; candidate.version = text(parsed.version); candidate.tag = text(parsed.tag);
    for (const auto& [name, asset] : parsed.assets) candidate.assets.insert(text(name), fromCore(asset));
    result = std::move(candidate); return true;
}

bool selectPackage(const QByteArray& bytes, const Release& release, PackageKind kind, Selection& result)
{
    JsonView object;
    Policy::Selection selected;
    if (!document(bytes, maximumMetadataBytes, object) ||
        !Policy::selectPackage(object, identity(), toCore(release), kind, selected)) return false;
    result = {fromCore(selected.package), fromCore(selected.buildInfo), text(selected.version), text(selected.tag),
        text(selected.sourceCommit), text(selected.minimumSystem), selected.kind};
    return true;
}

bool validateBuildInfo(const QByteArray& bytes, const Selection& selected)
{
    JsonView object;
    return document(bytes, maximumBuildInfoBytes, object) &&
        Policy::validateBuildInfo(object, identity(), toCore(selected));
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
