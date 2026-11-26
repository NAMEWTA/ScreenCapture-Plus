#pragma once

#include "core/UpdatePolicy.h"

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace HuhuCapture::Core::UpdateManifest {

// Release policy extracted from the reviewed project updater (PR #7, 251bc007).
// The views below are thin readers over Qt QJsonValue or Windows IJsonValue.
// Parsing, URL parsing and Unicode filesystem comparison remain framework/OS
// operations. This is deliberately not a JSON parser, DOM or serialization API.
struct Identity {
    std::string product, repository, releasesUrl, assetPrefix;
};
struct Asset {
    std::string name, sha256, url;
    std::uint64_t size{};
};
struct Release {
    std::string version, tag;
    std::map<std::string, Asset, std::less<>> assets;
};
struct Selection {
    Asset package, buildInfo;
    std::string version, tag, sourceCommit, minimumSystem;
    UpdatePackageKind kind{};
};
enum class ReleaseMode { CompletePackage, LegacyBridge };
inline constexpr std::uint64_t legacyExecutableLimit = 64ull * 1024 * 1024;

inline bool canonicalVersion(std::string_view value, bool tag = false)
{
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c < 128; })) return false;
    return canonicalUpdateVersion(std::wstring(value.begin(), value.end()), tag);
}
inline bool bounded(std::string_view value, std::size_t maximum)
{ return !value.empty() && value.size() <= maximum && value.find('\0') == std::string_view::npos; }
inline bool hex(std::string_view value, std::size_t length)
{
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline std::string assetUrl(const Identity& identity, std::string_view tag, std::string_view name)
{ return identity.assetPrefix + std::string(tag) + '/' + std::string(name); }
inline std::string packageName(const Identity& identity, std::string_view tag, UpdatePackageKind kind)
{
    const auto policy = updatePackagePolicy(kind);
    return policy.suffix.empty() ? std::string{} : identity.product + '-' + std::string(tag) + std::string(policy.suffix);
}

template<class View>
bool size(const View& value, std::uint64_t maximum, std::uint64_t& result, bool allowEmpty = false)
{
    if (!value.isNumber()) return false;
    const auto number = value.number();
    if (!std::isfinite(number) || number < (allowEmpty ? 0 : 1) ||
        number > static_cast<double>(maximum) || std::floor(number) != number) return false;
    result = static_cast<std::uint64_t>(number); return true;
}
template<class View>
bool identityMatches(const View& object, const Identity& identity, std::string_view tag,
    std::string_view version, std::string_view commit)
{
    for (const auto* key : {"developmentOnly", "sourceDirty"}) {
        const auto flag = object.value(key);
        if (!flag.missing() && (!flag.isBoolean() || flag.boolean())) return false;
    }
    const auto schema = object.value("schemaVersion");
    return schema.isNumber() && schema.number() == 1 &&
        object.value("product").string() == identity.product &&
        object.value("repository").string() == identity.repository &&
        object.value("tag").string() == tag && object.value("version").string() == version &&
        object.value("sourceCommit").string() == commit && hex(commit, 40);
}
template<class View>
bool descriptor(const View& object, std::uint64_t maximum, const Identity& identity,
    const Release& release, Asset& result)
{
    Asset candidate;
    candidate.name = object.value("name").string();
    candidate.sha256 = object.value("sha256").string();
    if (!updateAssetName(candidate.name) || !hex(candidate.sha256, 64) ||
        !size(object.value("size"), maximum, candidate.size)) return false;
    candidate.url = assetUrl(identity, release.tag, candidate.name);
    const auto found = release.assets.find(candidate.name);
    if (found == release.assets.end() || found->second.size != candidate.size ||
        found->second.sha256 != candidate.sha256 || found->second.url != candidate.url) return false;
    result = std::move(candidate); return true;
}

template<class View>
bool parseRelease(const View& object, const Identity& identity, Release& result,
    ReleaseMode mode = ReleaseMode::CompletePackage)
{
    if (!object.isObject() || !object.value("draft").isBoolean() || object.value("draft").boolean() ||
        !object.value("prerelease").isBoolean() || object.value("prerelease").boolean()) return false;
    Release candidate;
    candidate.tag = object.value("tag_name").string();
    if (!canonicalVersion(candidate.tag, true) ||
        object.value("html_url").string() != identity.releasesUrl + "/tag/" + candidate.tag) return false;
    candidate.version = candidate.tag.substr(1);
    const auto list = object.value("assets");
    if (!list.isArray() || !list.count() || list.count() > 128) return false;
    for (std::size_t index = 0; index < list.count(); ++index) {
        const auto item = list.at(index);
        if (!item.isObject()) return false;
        Asset asset;
        asset.name = item.value("name").string();
        if (!updateAssetName(asset.name) || candidate.assets.contains(asset.name) ||
            !size(item.value("size"), updateMaximumPackage, asset.size)) return false;
        const auto digest = item.value("digest").string();
        if (!digest.starts_with("sha256:") || !hex(std::string_view(digest).substr(7), 64)) return false;
        asset.sha256 = digest.substr(7);
        asset.url = item.value("browser_download_url").string();
        if (asset.url != assetUrl(identity, candidate.tag, asset.name)) return false;
        candidate.assets.emplace(asset.name, asset);
    }
    const auto manifest = candidate.assets.find("release-manifest.json");
    if (manifest == candidate.assets.end()) {
        if (mode != ReleaseMode::LegacyBridge) return false;
    } else if (manifest->second.size > updateMetadataLimit) return false;
    result = std::move(candidate); return true;
}

// The old standalone executable is a fallback only for a release with no
// manifest asset. A present but malformed/unavailable manifest must never be
// hidden by downgrading to a bare executable from that same release.
inline bool selectLegacyExecutable(const Identity& identity, const Release& release, Asset& result)
{
    if (release.assets.contains("release-manifest.json")) return false;
    const auto found = release.assets.find(identity.product + ".exe");
    if (found == release.assets.end() || found->second.size < 2 ||
        found->second.size > legacyExecutableLimit) return false;
    result = found->second; return true;
}

template<class View>
bool selectPackage(const View& object, const Identity& identity, const Release& release,
    UpdatePackageKind kind, Selection& result)
{
    const auto policy = updatePackagePolicy(kind);
    if (!object.isObject() || policy.kind.empty()) return false;
    const auto commit = object.value("sourceCommit").string();
    if (!identityMatches(object, identity, release.tag, release.version, commit)) return false;
    const auto assets = object.value("assets");
    if (!assets.isArray() || !assets.count() || assets.count() > 16) return false;
    std::optional<Selection> found;
    for (std::size_t index = 0; index < assets.count(); ++index) {
        const auto entry = assets.at(index);
        if (!entry.isObject()) return false;
        if (entry.value("platform").string() != policy.platform ||
            entry.value("architecture").string() != policy.architecture ||
            entry.value("packageKind").string() != policy.kind) continue;
        if (found) return false;
        Selection selected;
        selected.kind = kind; selected.tag = release.tag; selected.version = release.version; selected.sourceCommit = commit;
        selected.minimumSystem = entry.value("minimumSystem").string();
        if (!bounded(selected.minimumSystem, 256) ||
            !descriptor(entry, policy.maximumBytes, identity, release, selected.package) ||
            selected.package.name != packageName(identity, release.tag, kind) ||
            !entry.value("buildInfo").isObject() ||
            !descriptor(entry.value("buildInfo"), updateBuildInfoLimit, identity, release, selected.buildInfo) ||
            selected.buildInfo.name != "build-info-" + std::string(policy.platform) + '-' +
                std::string(policy.architecture) + ".json") return false;
        found = std::move(selected);
    }
    if (!found) return false;
    result = std::move(*found); return true;
}

template<class View>
bool validateBuildInfo(const View& object, const Identity& identity, const Selection& selected)
{
    const auto policy = updatePackagePolicy(selected.kind);
    if (!object.isObject() || policy.kind.empty() ||
        !identityMatches(object, identity, selected.tag, selected.version, selected.sourceCommit) ||
        object.value("platform").string() != policy.platform || object.value("architecture").string() != policy.architecture) return false;
    const auto dependencies = object.value("dependencies");
    if (!dependencies.isArray() || !dependencies.count() || dependencies.count() > 128) return false;
    std::set<std::string, std::less<>> names;
    for (std::size_t index = 0; index < dependencies.count(); ++index) {
        const auto item = dependencies.at(index);
        if (!item.isObject()) return false;
        const auto dependency = item.value("name").string();
        const auto source = item.value("source").string();
        if (!updateAssetName(dependency) || !names.insert(dependency).second ||
            !bounded(item.value("version").string(), 128) || !bounded(item.value("license").string(), 1024) ||
            !bounded(source, 2048) || !View::validHttpsSource(source)) return false;
    }
    for (const auto* required : {"qt", "gifski", "localsend", "ffmpeg"})
        if (!names.contains(required)) return false;
    const auto packages = object.value("packages");
    if (!packages.isArray() || !packages.count() || packages.count() > 16) return false;
    unsigned matches{};
    std::set<std::string, std::less<>> packageNames;
    std::string contentPrefix;
    for (std::size_t index = 0; index < packages.count(); ++index) {
        const auto item = packages.at(index);
        if (!item.isObject()) return false;
        const auto package = item.value("name").string();
        if (!updateAssetName(package) || !packageNames.insert(package).second) return false;
        if (package != selected.package.name) continue;
        const auto contentRoot = item.value("contentRoot").string();
        std::uint64_t length{};
        if (item.value("packageKind").string() != policy.kind || contentRoot != policy.kind ||
            !size(item.value("size"), policy.maximumBytes, length) || length != selected.package.size ||
            item.value("sha256").string() != selected.package.sha256) return false;
        contentPrefix = contentRoot + '/'; ++matches;
    }
    if (matches != 1) return false;
    const bool windows = policy.platform == "windows";
    const auto prefixKey = View::comparisonPath(contentPrefix, windows);
    if (!prefixKey) return false;
    const auto files = object.value("files");
    if (!files.isArray() || !files.count() || files.count() > 8192) return false;
    std::set<std::string, std::less<>> paths;
    bool executable{}, qt{}, platformPlugin{}, wayland{};
    for (std::size_t index = 0; index < files.count(); ++index) {
        const auto item = files.at(index);
        if (!item.isObject()) return false;
        const auto path = item.value("path").string();
        if (!updateInventoryPath(path)) return false;
        const auto key = View::comparisonPath(path, windows);
        std::uint64_t length{};
        if (!key || !paths.insert(*key).second ||
            !size(item.value("size"), updateMaximumPackage, length, true) ||
            !hex(item.value("sha256").string(), 64)) return false;
        if (!key->starts_with(*prefixKey)) continue;
        // The content root is an ASCII package-kind token, so its byte count is
        // unchanged by either framework's Unicode case normalization.
        const auto relative = std::string_view(path).substr(contentPrefix.size());
        if (!updateInventoryPath(relative)) return false;
        const auto slash = relative.find_last_of('/');
        const auto leaf = relative.substr(slash == std::string_view::npos ? 0 : slash + 1);
        executable = executable || (length && leaf == (windows ? identity.product + ".exe" : identity.product));
        qt = qt || (length && (windows ? leaf == "Qt6Core.dll" :
            leaf == "libQt6Core.so" || leaf.starts_with("libQt6Core.so.")));
        if (length && (relative.starts_with("platforms/") || relative.find("/platforms/") != std::string_view::npos)) {
            platformPlugin = platformPlugin || leaf == (windows ? "qwindows.dll" : "libqxcb.so");
            wayland = wayland || (leaf.starts_with("libqwayland") && leaf.ends_with(".so"));
        }
    }
    return executable && qt && platformPlugin && (windows || wayland);
}

} // namespace HuhuCapture::Core::UpdateManifest
