#include "services/update/UpdateService.h"
#include "services/update/ReleaseManifest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QHostAddress>
#include <QPointer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace updates = HuhuCapture::Updates;
namespace manifest = HuhuCapture::Updates::Manifest;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Condition> void waitFor(Condition condition, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        settle(); std::this_thread::sleep_for(1ms);
    }
}
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Open real retained download");
    return file.readAll();
}
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QJsonObject descriptor(const QString& name, const QByteArray& bytes)
{ return {{QStringLiteral("name"), name}, {QStringLiteral("size"), static_cast<double>(bytes.size())}, {QStringLiteral("sha256"), sha(bytes)}}; }

struct Response {
    QByteArray body{};
    int status{200};
    QByteArray redirect{};
    qint64 declaredLength{-1};
    bool slow{};
};

class ResponseWriter final : public QObject {
public:
    ResponseWriter(QTcpSocket* socket, Response response) : QObject(socket), socket_(socket), response_(std::move(response))
    {
        const auto length = response_.declaredLength < 0 ? response_.body.size() : response_.declaredLength;
        QByteArray header = "HTTP/1.1 " + QByteArray::number(response_.status) + " Result\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(length) + "\r\n";
        if (!response_.redirect.isEmpty()) header += "Location: " + response_.redirect + "\r\n";
        header += "\r\n";
        socket_->write(header);
        timer_ = new QTimer(this); timer_->setInterval(response_.slow ? 3 : 0);
        connect(timer_, &QTimer::timeout, this, [this] {
            if (socket_->state() != QAbstractSocket::ConnectedState) { timer_->stop(); return; }
            if (offset_ >= response_.body.size()) { timer_->stop(); socket_->disconnectFromHost(); return; }
            const auto length = (std::min)(response_.body.size() - offset_, qsizetype(response_.slow ? 256 : 4096));
            const auto written = socket_->write(response_.body.constData() + offset_, length);
            if (written < 0) { timer_->stop(); return; }
            offset_ += written;
        });
        timer_->start();
    }
private:
    QTcpSocket* socket_;
    Response response_;
    QTimer* timer_{};
    qsizetype offset_{};
};

class FixtureServer final : public QObject {
public:
    QMap<QString, Response> routes;
    QMap<QString, unsigned> requests;
    FixtureServer()
    {
        require(server_.listen(QHostAddress::LocalHost, 0), "Bind an actual loopback HTTP fixture");
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                auto served = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request, served] {
                    if (*served) { socket->readAll(); return; }
                    *request += socket->readAll();
                    if (request->size() > 16384) { socket->abort(); return; }
                    if (!request->contains("\r\n\r\n")) return;
                    const auto line = request->left(request->indexOf("\r\n")).split(' ');
                    if (line.size() != 3 || line[0] != "GET") { socket->abort(); return; }
                    *served = true;
                    const auto path = QUrl::fromEncoded(line[1]).path();
                    ++requests[path];
                    const auto response = routes.value(path, Response{QByteArray("missing"), 404});
                    new ResponseWriter(socket, response);
                });
            }
        });
    }
    QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }
private:
    QTcpServer server_;
};

struct Fixture {
    const QString tag{QStringLiteral("v0.0.5")};
    const QString packageName{QStringLiteral("huhucapture-v0.0.5-windows-x64.zip")};
    QByteArray package;
    QJsonObject build;
    QJsonObject release;
    QJsonObject manifestDocument;

    Fixture()
    {
        // A valid stored ZIP, generated by Python's standard zipfile with three
        // explicitly labelled transport fixture entries. Its comment increases
        // streamed bytes. It does NOT claim to be an installable Qt product or
        // substitute for the actual packaged-artifact installation gates.
        package = QByteArray::fromBase64("UEsDBBQAAAAAAAAAIVwe+Tn5OAAAADgAAAAPAAAAaHVodWNhcHR1cmUuZXhlVHJhbnNwb3J0LWNvbnRyYWN0IGZpeHR1cmU7IG5vdCBhbiBpbnN0YWxsYWJsZSBwcm9kdWN0LgpQSwMEFAAAAAAAAAAhXB75Ofk4AAAAOAAAAAsAAABRdDZDb3JlLmRsbFRyYW5zcG9ydC1jb250cmFjdCBmaXh0dXJlOyBub3QgYW4gaW5zdGFsbGFibGUgcHJvZHVjdC4KUEsDBBQAAAAAAAAAIVwe+Tn5OAAAADgAAAAeAAAAcGx1Z2lucy9wbGF0Zm9ybXMvcXdpbmRvd3MuZGxsVHJhbnNwb3J0LWNvbnRyYWN0IGZpeHR1cmU7IG5vdCBhbiBpbnN0YWxsYWJsZSBwcm9kdWN0LgpQSwECFAMUAAAAAAAAACFcHvk5+TgAAAA4AAAADwAAAAAAAAAAAAAAgAEAAAAAaHVodWNhcHR1cmUuZXhlUEsBAhQDFAAAAAAAAAAhXB75Ofk4AAAAOAAAAAsAAAAAAAAAAAAAAIABZQAAAFF0NkNvcmUuZGxsUEsBAhQDFAAAAAAAAAAhXB75Ofk4AAAAOAAAAB4AAAAAAAAAAAAAAIABxgAAAHBsdWdpbnMvcGxhdGZvcm1zL3F3aW5kb3dzLmRsbFBLBQYAAAAAAwADAMIAAAA6AQAAAAA=");
        // Keep the byte-perfect valid fixture seed; two-byte ZIP comment length
        // is the final field of its end record, followed by exactly that comment.
        package[package.size() - 2] = char(0xff); package[package.size() - 1] = char(0xff);
        package += QByteArray(65535, 'U');
        const QJsonObject identity{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("product"), QStringLiteral("huhucapture")},
            {QStringLiteral("repository"), QStringLiteral("NAMEWTA/huhucapture")}, {QStringLiteral("tag"), tag},
            {QStringLiteral("version"), QStringLiteral("0.0.5")}, {QStringLiteral("sourceCommit"), QString(40, QLatin1Char('1'))},
            {QStringLiteral("developmentOnly"), false}, {QStringLiteral("sourceDirty"), false}};
        build = identity; manifestDocument = identity;
        build.insert(QStringLiteral("platform"), QStringLiteral("windows"));
        build.insert(QStringLiteral("architecture"), QStringLiteral("x64"));
        QJsonArray dependencies;
        for (const auto* name : {"qt", "gifski", "localsend", "ffmpeg"}) dependencies.append(QJsonObject{
            {QStringLiteral("name"), QString::fromLatin1(name)}, {QStringLiteral("version"), QStringLiteral("fixture-version")},
            {QStringLiteral("license"), QStringLiteral("Test fixture; no dependency distribution")},
            {QStringLiteral("source"), QStringLiteral("https://github.com/NAMEWTA/huhucapture")}});
        build.insert(QStringLiteral("dependencies"), dependencies);
        const QByteArray content("Transport-contract fixture; not an installable product.\n");
        QJsonArray files;
        for (const auto* name : {"huhucapture.exe", "Qt6Core.dll", "plugins/platforms/qwindows.dll"}) files.append(QJsonObject{
            {QStringLiteral("path"), QString::fromLatin1(name)}, {QStringLiteral("size"), content.size()},
            {QStringLiteral("sha256"), sha(content)}});
        build.insert(QStringLiteral("files"), files);
        auto selected = descriptor(packageName, package); selected.insert(QStringLiteral("packageKind"), QStringLiteral("portable-zip"));
        build.insert(QStringLiteral("packages"), QJsonArray{selected});
        selected.insert(QStringLiteral("platform"), QStringLiteral("windows"));
        selected.insert(QStringLiteral("architecture"), QStringLiteral("x64"));
        selected.insert(QStringLiteral("minimumSystem"), QStringLiteral("Windows 10 1809"));
        manifestDocument.insert(QStringLiteral("assets"), QJsonArray{selected});
        release = {{QStringLiteral("draft"), false}, {QStringLiteral("prerelease"), false},
            {QStringLiteral("tag_name"), tag}, {QStringLiteral("html_url"), QStringLiteral("https://github.com/NAMEWTA/huhucapture/releases/tag/") + tag}};
    }
    QString path(const QString& name) const { return manifest::assetUrl(tag, name).path(); }
    void publish(FixtureServer& server)
    {
        const auto buildBytes = QJsonDocument(build).toJson(QJsonDocument::Compact);
        auto entries = manifestDocument.value(QStringLiteral("assets")).toArray();
        auto selected = entries[0].toObject();
        selected.insert(QStringLiteral("buildInfo"), descriptor(QStringLiteral("build-info-windows-x64.json"), buildBytes));
        entries[0] = selected; manifestDocument.insert(QStringLiteral("assets"), entries);
        const auto manifestDocumentBytes = QJsonDocument(manifestDocument).toJson(QJsonDocument::Compact);
        QJsonArray assets;
        const auto add = [&](const QString& name, const QByteArray& bytes) {
            assets.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("size"), bytes.size()},
                {QStringLiteral("digest"), QStringLiteral("sha256:") + sha(bytes)},
                {QStringLiteral("browser_download_url"), manifest::assetUrl(tag, name).toString()}});
            server.routes.insert(path(name), Response{bytes});
        };
        add(packageName, package); add(QStringLiteral("build-info-windows-x64.json"), buildBytes);
        add(QStringLiteral("release-manifest.json"), manifestDocumentBytes);
        release.insert(QStringLiteral("assets"), assets);
        server.routes.insert(manifest::latestUrl().path(), Response{QJsonDocument(release).toJson(QJsonDocument::Compact)});
    }
};

updates::Options options(const QString& folder, const FixtureServer& server)
{ return {folder, QStringLiteral("0.0.5"), updates::PackageKind::WindowsPortableZip, server.origin()}; }
void stop(updates::UpdateService& service)
{
    bool joined{}; QObject context;
    QObject::connect(&service, &updates::UpdateService::shutdownFinished, &context, [&] { joined = true; });
    service.shutdown(); waitFor([&] { return joined; }, "Actual network worker shutdown did not join");
    require(service.snapshot().stopped && !service.busy(), "Shutdown disables further tasks after native cleanup");
}
void noStaging(const QString& folder)
{
    const auto files = QDir(folder).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const auto& file : files) require(!file.endsWith(QStringLiteral(".part")), "Failure removes only this task's real owned staging file");
}

void downloadsAndLifetime(FixtureServer& server, const QString& root)
{
    Fixture fixture; fixture.publish(server);
    const auto folder = QDir(root).filePath(QStringLiteral("actual downloads 中文"));
    require(QDir().mkpath(folder), "Create a real redirected Downloads fixture");
    const auto originalPath = QDir(folder).filePath(fixture.packageName);
    { QFile original(originalPath); require(original.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
        original.write("user existing download") == 22, "Claim a user-owned filename collision"); }
    updates::UpdateService service(options(folder, server));
    require(!service.busy() && server.requests.isEmpty(), "Constructing the application service issues no network request");
    require(service.check(), "Admit actual latest metadata check");
    waitFor([&] { return !service.busy(); }, "Actual metadata chain did not finish");
    require(service.snapshot().stage == updates::Stage::Idle && service.snapshot().version == QStringLiteral("0.0.5"),
        "Same stable version is not falsely announced as a newer release");
    require(!server.requests.contains(fixture.path(fixture.packageName)), "Check fetches metadata and dependency inventory without package bytes");
    auto observer = std::make_unique<QObject>(); unsigned observations{};
    QObject::connect(&service, &updates::UpdateService::changed, observer.get(), [&](const updates::Snapshot&) { ++observations; });
    server.routes[fixture.path(fixture.packageName)].slow = true;
    require(service.download() && !service.download(), "One real explicit task is admitted, including the current stable version");
    waitFor([&] { return service.snapshot().stage == updates::Stage::Downloading && service.snapshot().received > 0; },
        "Real streaming package progress did not arrive");
    observer.reset(); const auto observedBefore = observations;
    waitFor([&] { return !service.busy(); }, "Closing a subscriber must not cancel the service's actual download");
    const auto completed = service.snapshot();
    require(completed.stage == updates::Stage::Downloaded && completed.received == static_cast<quint64>(fixture.package.size()) &&
        completed.file != originalPath && read(completed.file) == fixture.package && read(originalPath) == "user existing download" &&
        observations == observedBefore, "Exact SHA-verified bytes commit without overwriting the user file or calling a dead subscriber");
    require(service.download(), "A completed task permits an explicit repeat download");
    waitFor([&] { return !service.busy(); }, "Actual repeated download did not finish");
    require(service.snapshot().stage == updates::Stage::Downloaded && service.snapshot().file != completed.file &&
        read(completed.file) == fixture.package, "Repeated same-version downloads remain separate verified files");
    stop(service); noStaging(folder);
    require(read(completed.file) == fixture.package, "Application exit preserves its already-published package");
}

void rejectedInputs(FixtureServer& server, const QString& root)
{
    for (int scenario = 0; scenario < 7; ++scenario) {
        Fixture fixture;
        if (scenario == 0) fixture.build.remove(QStringLiteral("dependencies"));
        if (scenario == 1) fixture.manifestDocument.insert(QStringLiteral("repository"), QStringLiteral("other/project"));
        if (scenario == 2) {
            auto entries = fixture.manifestDocument.value(QStringLiteral("assets")).toArray();
            auto entry = entries[0].toObject(); entry.insert(QStringLiteral("architecture"), QStringLiteral("arm64"));
            entries[0] = entry; fixture.manifestDocument.insert(QStringLiteral("assets"), entries);
        }
        fixture.publish(server);
        auto& package = server.routes[fixture.path(fixture.packageName)];
        if (scenario == 3) package.body[package.body.size() / 2] ^= 1; // Wrong wire bytes, unchanged authenticated SHA.
        if (scenario == 4) { package.declaredLength = package.body.size(); package.body.chop(17); }
        if (scenario == 5) package = Response{{}, 302, QByteArray("http://example.invalid/untrusted.zip")};
        if (scenario == 6) server.routes[manifest::latestUrl().path()] = Response{QByteArray(1024 * 1024 + 1, 'x')};
        const auto folder = QDir(root).filePath(QStringLiteral("rejected-%1").arg(scenario));
        updates::UpdateService service(options(folder, server));
        require(service.download(), "Admit actual invalid-input transport fixture");
        waitFor([&] { return !service.busy(); }, "Invalid input must reach a real terminal result");
        require(service.snapshot().stage == updates::Stage::Failed && service.snapshot().file.isEmpty(),
            "Reject bad inventory/repository/architecture/hash/length/redirect/metadata without publishing a package");
        if (scenario == 3) require(service.snapshot().error == updates::Error::Integrity, "Corrupted actual bytes retain an integrity diagnosis");
        noStaging(folder); stop(service);
    }
}

void cancellationAndRedirect(FixtureServer& server, const QString& root)
{
    Fixture fixture; fixture.publish(server);
    server.routes.insert(QStringLiteral("/signed-fixture"), Response{fixture.package});
    server.routes[fixture.path(fixture.packageName)] = Response{{}, 302, server.origin().resolved(QUrl(QStringLiteral("/signed-fixture"))).toEncoded()};
    {
        updates::UpdateService service(options(QDir(root).filePath(QStringLiteral("redirect")), server));
        require(service.download(), "Admit actual permitted redirect");
        waitFor([&] { return !service.busy(); }, "Permitted real redirect did not finish");
        require(service.snapshot().stage == updates::Stage::Downloaded && read(service.snapshot().file) == fixture.package,
            "Controlled same-origin redirect preserves the original asset hash and metadata requirements");
        stop(service);
    }
    fixture.publish(server); server.routes[fixture.path(fixture.packageName)].slow = true;
    const auto folder = QDir(root).filePath(QStringLiteral("cancelled"));
    auto service = std::make_unique<updates::UpdateService>(options(folder, server));
    QPointer<updates::UpdateService> lifetime(service.get());
    require(service->download(), "Admit actual slow streaming task");
    waitFor([&] { return service->snapshot().stage == updates::Stage::Downloading && service->snapshot().received > 0; },
        "Actual cancellable network body did not arrive");
    QObject host; bool joined{};
    QObject::connect(service.get(), &updates::UpdateService::shutdownFinished, &host, [&] {
        joined = service->snapshot().stopped && !service->busy(); service.reset();
    });
    service->shutdown();
    waitFor([&] { return lifetime.isNull(); }, "Host must be able to delete service after actual worker join");
    require(joined, "Shutdown acknowledgement follows actual worker/network/file cleanup");
    noStaging(folder);
    require(QDir(folder).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(), "Cancelled partial body creates no final package");
}

void developmentReleasesRejected(FixtureServer& server, const QString& root)
{
    unsigned scenario{};
    for (const bool buildInfo : {false, true}) {
        for (const auto* flag : {"developmentOnly", "sourceDirty"}) {
            for (const auto& value : {QJsonValue(true), QJsonValue(QStringLiteral("false")),
                                     QJsonValue(0), QJsonValue(QJsonValue::Null)}) {
                Fixture fixture;
                auto& document = buildInfo ? fixture.build : fixture.manifestDocument;
                document.insert(QString::fromLatin1(flag), value);
                // Recompute all transport descriptors: rejection must be due
                // to provenance, even when every received SHA256 is correct.
                fixture.publish(server);
                const auto before = server.requests.value(fixture.path(fixture.packageName));
                const auto folder = QDir(root).filePath(QStringLiteral("development-rejected-%1").arg(scenario++));
                updates::UpdateService service(options(folder, server));
                require(service.download(), "Admit real authenticated development metadata fixture");
                waitFor([&] { return !service.busy(); }, "Development metadata rejection did not terminate");
                require(service.snapshot().stage == updates::Stage::Failed && service.snapshot().error == updates::Error::Metadata &&
                    service.snapshot().file.isEmpty() && server.requests.value(fixture.path(fixture.packageName)) == before,
                    "Development, dirty, or malformed provenance must fail before requesting any package bytes");
                noStaging(folder); stop(service);
            }
        }
    }
}

void selectedPackageInventory(FixtureServer& server, const QString& root)
{
    Fixture fixture;
    auto packages = fixture.build.value(QStringLiteral("packages")).toArray();
    auto selected = packages[0].toObject();
    selected.insert(QStringLiteral("contentRoot"), QStringLiteral("portable-zip"));
    packages[0] = selected; fixture.build.insert(QStringLiteral("packages"), packages);
    auto files = fixture.build.value(QStringLiteral("files")).toArray();
    for (qsizetype i = 0; i < files.size(); ++i) {
        auto file = files[i].toObject();
        file.insert(QStringLiteral("path"), QStringLiteral("installer/") + file.value(QStringLiteral("path")).toString());
        files[i] = file;
    }
    fixture.build.insert(QStringLiteral("files"), files);
    fixture.publish(server);
    const auto before = server.requests.value(fixture.path(fixture.packageName));
    updates::UpdateService service(options(QDir(root).filePath(QStringLiteral("wrong-inventory-root")), server));
    require(service.download(), "Admit aggregate build inventory fixture");
    waitFor([&] { return !service.busy(); }, "Wrong inventory root rejection did not terminate");
    require(service.snapshot().stage == updates::Stage::Failed && service.snapshot().error == updates::Error::Metadata &&
        server.requests.value(fixture.path(fixture.packageName)) == before,
        "Another package's files cannot satisfy the selected package's required runtime inventory");
    stop(service);

    for (qsizetype i = 0; i < files.size(); ++i) {
        auto file = files[i].toObject();
        auto path = file.value(QStringLiteral("path")).toString();
        path.replace(0, QStringLiteral("installer").size(), QStringLiteral("portable-zip"));
        file.insert(QStringLiteral("path"), path); files[i] = file;
    }
    fixture.build.insert(QStringLiteral("files"), files); fixture.publish(server);
    updates::UpdateService complete(options(QDir(root).filePath(QStringLiteral("matching-inventory-root")), server));
    require(complete.download(), "Admit selected inventory fixture");
    waitFor([&] { return !complete.busy(); }, "Matching selected inventory did not terminate");
    require(complete.snapshot().stage == updates::Stage::Downloaded && read(complete.snapshot().file) == fixture.package,
        "Selected package runtime files and canonical inventory prefix remain accepted");
    stop(complete);
}

void subscriberReentrancy(FixtureServer& server, const QString& root)
{
    Fixture fixture; fixture.publish(server);
    updates::UpdateService service(options(QDir(root).filePath(QStringLiteral("subscriber-reentrancy")), server));
    QObject host;
    unsigned completed{}, observed{};
    bool readmitted{};
    QObject::connect(&service, &updates::UpdateService::changed, &host, [&](const updates::Snapshot& value) {
        if (value.stage != updates::Stage::Idle || value.version.isEmpty()) return;
        if (++completed == 1) readmitted = service.check();
    });
    QObject::connect(&service, &updates::UpdateService::changed, &host, [&](const updates::Snapshot& value) {
        if (value.stage == updates::Stage::Idle && !value.version.isEmpty()) ++observed;
    });
    require(service.check(), "Admit real check with reentrant subscribers");
    waitFor([&] { return completed == 2 && !service.busy(); }, "Real reentrant metadata checks did not both finish");
    require(readmitted && observed == 2, "Every subscriber receives the immutable completed snapshot even when an earlier subscriber starts the next task");
    stop(service);

    auto unstarted = std::make_unique<updates::UpdateService>(options(root, server));
    QPointer<updates::UpdateService> lifetime(unstarted.get());
    QObject::connect(unstarted.get(), &updates::UpdateService::changed, &host, [&](const updates::Snapshot& value) {
        if (value.stopped) unstarted.reset();
    });
    unstarted->shutdown();
    require(lifetime.isNull(), "A host may destroy an unstarted service synchronously during its stopped snapshot");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(QSslSocket::supportsSsl(), "Actual Qt TLS backend and native crypto runtime must be usable for production HTTPS");
        QTemporaryDir temporary; require(temporary.isValid(), "Create actual updater filesystem fixtures");
        FixtureServer server;
        downloadsAndLifetime(server, temporary.path()); rejectedInputs(server, temporary.path());
        developmentReleasesRejected(server, temporary.path());
        selectedPackageInventory(server, temporary.path());
        cancellationAndRedirect(server, temporary.path()); subscriberReentrancy(server, temporary.path());
        std::cout << "Qt updater: real loopback HTTP, canonical metadata chain, streaming/hash, duplicate names, subscriber destruction, rejection and joined cancellation passed. Transport fixture only; packaged product installation is a separate gate.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Qt updater validation failed: " << error.what() << '\n'; return 1;
    }
}
