// Native runtime validation against an installed tree; never shipped as the app.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QSet>
#include <QScopeGuard>
#include <QSslCertificate>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QXmlStreamReader>
#include <iostream>
#include <stdexcept>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <vector>
#endif

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QByteArray readFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 8 * 1024 * 1024, "Cannot read bounded native runtime input");
    return file.readAll();
}
QSet<QString> loadedModules()
{
    QSet<QString> result;
#ifdef Q_OS_WIN
    std::vector<HMODULE> handles(4096);
    DWORD bytes{};
    require(K32EnumProcessModules(GetCurrentProcess(), handles.data(),
        static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &bytes) && bytes <= handles.size() * sizeof(HMODULE),
        "Cannot inspect the actual loaded Windows DLLs");
    for (std::size_t i{}; i < bytes / sizeof(HMODULE); ++i) {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(handles[i], path, 32768);
        require(length && length < 32768, "Loaded native DLL path is absent or truncated");
        result.insert(QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).canonicalFilePath());
    }
#else
    // Inspect this actual process, not ldd output from a different executable.
    for (const auto& line : readFile(QStringLiteral("/proc/self/maps")).split('\n')) {
        const auto start = line.indexOf('/');
        if (start >= 0) result.insert(QFileInfo(QString::fromUtf8(line.mid(start))).canonicalFilePath());
    }
#endif
    result.remove(QString()); return result;
}
QJsonObject checkedModules(const QString& prefix)
{
    const auto modules = loadedModules();
#ifdef Q_OS_WIN
    const QStringList required{QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Network.dll"),
        QStringLiteral("qschannelbackend.dll"), QStringLiteral("vcruntime140.dll"), QStringLiteral("msvcp140.dll")};
    constexpr auto comparison = Qt::CaseInsensitive;
#else
    const QStringList required{QStringLiteral("libQt6Core.so."), QStringLiteral("libQt6Network.so."),
        QStringLiteral("libqopensslbackend.so"), QStringLiteral("libssl.so.3"), QStringLiteral("libcrypto.so.3")};
    constexpr auto comparison = Qt::CaseSensitive;
#endif
    QJsonObject identities;
    for (const auto& expected : required) {
        QSet<QString> matches;
        for (const auto& path : modules) {
            const auto name = QFileInfo(path).fileName();
            if (expected.endsWith(u'.') ? name.startsWith(expected, comparison) : name.compare(expected, comparison) == 0) {
                if (!path.startsWith(prefix + u'/', comparison))
                    throw std::runtime_error((QStringLiteral("Native runtime escaped installed prefix: ") + path).toStdString());
                matches.insert(path);
            }
        }
        require(matches.size() == 1, "Required native library/backend was not uniquely loaded from the installed tree");
        identities.insert(expected, *matches.cbegin());
    }
    return identities;
}
#ifdef Q_OS_WIN
QJsonObject productManifest(const QString& prefix)
{
    // Read resources from the actual packaged product without executing its
    // entry point. A linked test probe's own default manifest is not evidence.
    const auto executable = QDir(prefix).filePath(QStringLiteral("huhucapture.exe")).toStdWString();
    const auto module = LoadLibraryExW(executable.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    require(module != nullptr, "Cannot load the actual packaged product's resources");
    const auto release = qScopeGuard([&] { FreeLibrary(module); });
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    require(resource != nullptr, "Actual packaged product has no process manifest");
    const auto size = SizeofResource(module, resource);
    require(size && size <= 65536, "Actual product manifest is empty or exceeds its bound");
    const auto loaded = LoadResource(module, resource);
    const auto data = loaded ? LockResource(loaded) : nullptr;
    require(data != nullptr, "Cannot read actual product manifest bytes");
    const QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
    QXmlStreamReader xml(bytes);
    unsigned dpi{}, execution{}, longPaths{};
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == u"dpiAwareness") {
            require(xml.namespaceUri() == u"http://schemas.microsoft.com/SMI/2016/WindowsSettings" &&
                xml.readElementText() == QStringLiteral("PerMonitorV2"),
                "Actual packaged product must declare PerMonitorV2, not only legacy True/PM");
            ++dpi;
        } else if (xml.name() == u"longPathAware") {
            require(xml.namespaceUri() == u"http://schemas.microsoft.com/SMI/2016/WindowsSettings" &&
                xml.readElementText() == QStringLiteral("true"), "Actual product must declare long path support");
            ++longPaths;
        } else if (xml.name() == u"requestedExecutionLevel") {
            require(xml.attributes().value(u"level") == u"asInvoker" &&
                xml.attributes().value(u"uiAccess") == u"false", "Actual product must remain an ordinary per-user process");
            ++execution;
        }
    }
    require(!xml.hasError() && dpi == 1 && execution == 1 && longPaths == 1,
        "Actual product manifest has invalid or ambiguous process declarations");
    return {{QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("dpiAwareness"), QStringLiteral("PerMonitorV2")},
            {QStringLiteral("executionLevel"), QStringLiteral("asInvoker")}, {QStringLiteral("longPathAware"), true}};
}
#endif
class NativeTlsServer final : public QTcpServer {
public:
    NativeTlsServer(QSslCertificate certificate, QSslKey key) : certificate_(std::move(certificate)), key_(std::move(key)) {}
protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        socket->setLocalCertificate(certificate_); socket->setPrivateKey(key_);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone); // Server does not request a client certificate.
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
        socket->startServerEncryption();
    }
private:
    QSslCertificate certificate_;
    QSslKey key_;
};
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        const auto arguments = application.arguments();
        require(arguments.size() == 5, "Expected installed prefix, certificate, test key and exact Qt version");
        const auto prefix = QFileInfo(arguments[1]).canonicalFilePath();
        require(!prefix.isEmpty() && QString::fromLatin1(qVersion()) == arguments[4], "Installed Qt does not match its build identity");
        require(QFileInfo(QLibraryInfo::path(QLibraryInfo::PrefixPath)).canonicalFilePath() == prefix,
                "Installed qt.conf did not resolve the actual product prefix");
        require(QSslSocket::supportsSsl(), "The actual Qt TLS backend cannot load its native crypto runtime");
#ifdef Q_OS_WIN
        require(QSslSocket::activeBackend() == QStringLiteral("schannel"), "Windows package must use its actual native Schannel backend");
#else
        require(QSslSocket::activeBackend() == QStringLiteral("openssl"), "Linux package must load its actual OpenSSL backend");
#endif
        const QSslCertificate certificate(readFile(arguments[2]));
        const QSslKey key(readFile(arguments[3]), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        require(!certificate.isNull() && !key.isNull(), "Real self-signed TLS fixture could not be parsed by the deployed backend");
        NativeTlsServer server(certificate, key);
        require(server.listen(QHostAddress::LocalHost, 0), "Bind an actual ephemeral loopback TLS server");
        QSslSocket client;
        client.setPeerVerifyMode(QSslSocket::VerifyPeer);
        QEventLoop loop;
        QTimer deadline; deadline.setSingleShot(true);
        bool encrypted{}, timedOut{}, rejected{};
        QJsonArray errors;
        QObject::connect(&client, &QSslSocket::sslErrors, &loop, [&](const QList<QSslError>& values) {
            for (const auto& error : values) errors.append(error.errorString());
            // No ignoreSslErrors: the deployed client must reject untrusted TLS.
        });
        QObject::connect(&client, &QSslSocket::encrypted, &loop, [&] { encrypted = true; loop.quit(); });
        QObject::connect(&client, &QSslSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError error) {
            rejected = error == QAbstractSocket::SslHandshakeFailedError; loop.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { timedOut = true; loop.quit(); });
        deadline.start(10000);
        client.connectToHostEncrypted(QStringLiteral("127.0.0.1"), server.serverPort(), QStringLiteral("localhost"));
        loop.exec();
        require(!timedOut && rejected && !encrypted && !errors.isEmpty(), "Deployed TLS did not genuinely reject the self-signed peer");
        QJsonObject result{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
            {QStringLiteral("backend"), QSslSocket::activeBackend()}, {QStringLiteral("sslRuntimeVersion"), QSslSocket::sslLibraryVersionString()},
            {QStringLiteral("sslBuildVersion"), QSslSocket::sslLibraryBuildVersionString()},
            {QStringLiteral("modules"), checkedModules(prefix)}, {QStringLiteral("untrustedPeerRejected"), true},
            {QStringLiteral("trustErrors"), errors}};
#ifdef Q_OS_WIN
        result.insert(QStringLiteral("productManifest"), productManifest(prefix));
#endif
        client.abort(); server.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "Installed Qt TLS validation failed: " << error.what() << '\n'; return 1; }
}
