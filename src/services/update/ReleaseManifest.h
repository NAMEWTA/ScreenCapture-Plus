#pragma once

#include "services/update/UpdateService.h"

#include <QByteArray>
#include <QJsonArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <optional>

namespace HuhuCapture::Updates::Manifest {

inline constexpr quint64 maximumMetadataBytes = Core::updateMetadataLimit;
inline constexpr quint64 maximumBuildInfoBytes = Core::updateBuildInfoLimit;
inline constexpr quint64 maximumPackageBytes = Core::updateMaximumPackage;
constexpr quint64 packageBudget(PackageKind kind)
{ return Core::updatePackagePolicy(kind).maximumBytes; }

struct Asset {
    QString name;
    quint64 size{};
    QByteArray sha256;
    QUrl url;
};
struct Release {
    QString version, tag;
    QMap<QString, Asset> assets;
};
struct Selection {
    Asset package, buildInfo;
    QString version, tag, sourceCommit, minimumSystem;
    PackageKind kind{};
};

QUrl latestUrl();
QUrl assetUrl(const QString& tag, const QString& name);
bool canonicalVersion(const QString& value, bool tag = false);
bool parseRelease(const QByteArray& bytes, Release& result);
bool selectPackage(const QByteArray& bytes, const Release& release, PackageKind kind, Selection& result);
bool validateBuildInfo(const QByteArray& bytes, const Selection& selected);
bool validTestOrigin(const QUrl& origin);
QUrl requestUrl(const QUrl& canonical, const std::optional<QUrl>& testOrigin);
bool allowedRedirect(const QUrl& next, const QUrl& canonical, const std::optional<QUrl>& testOrigin);

} // namespace HuhuCapture::Updates::Manifest
