#include "platform/linux/portal/PortalCapture.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QWindow>
#include <fcntl.h>
#include <unistd.h>
#include <memory>

using namespace HuhuCapture::Platform::Linux;

namespace {

void print(QJsonObject object)
{
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
}

QJsonObject describe(const PortalFailure& error)
{
    return {{"code", static_cast<int>(error.code)}, {"domain", error.domain},
            {"nativeCode", error.nativeCode}, {"remoteError", error.remoteError},
            {"message", error.message}};
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("huhucapture-portal-probe"));
    app.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    QCommandLineParser arguments;
    arguments.setApplicationDescription(QStringLiteral("Actual Portal control-plane probe; no capture frames are synthesized"));
    arguments.addHelpOption();
    arguments.addOption({"screenshot", "Request an interactive screenshot"});
    arguments.addOption({"unparented", "Use the real parent-less Screenshot path from a tray/shortcut entry"});
    arguments.addOption({"noninteractive", "Request a noninteractive Screenshot where the actual desktop permits it"});
    arguments.addOption({"screencast", "Request monitors and open their real PipeWire remote; does not decode frames"});
    arguments.addOption({"cancel-after-ms", "Cancel pending authorization after this interval", "milliseconds"});
    arguments.addOption({"destroy-parent-after-ms", "Destroy the authorization parent after this interval", "milliseconds"});
    arguments.process(app);
    if (arguments.isSet("screenshot") && arguments.isSet("screencast")) return 2;

    PortalCapture control;
    auto window = std::make_unique<QWindow>();
    window->setTitle(QStringLiteral("huhucapture Portal control validation"));
    window->resize(640, 240);
    const bool screenshot = arguments.isSet("screenshot");
    const bool screenCast = arguments.isSet("screencast");
    if (screenCast || (screenshot && !arguments.isSet("unparented"))) window->show();
    if (screenCast && arguments.isSet("unparented")) return 2;
    quint64 request{};
    int remoteFd{-1};
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &control, [&] {
        if (remoteFd >= 0) { ::close(remoteFd); remoteFd = -1; }
        control.closeScreenCast();
    });
    QObject::connect(&control, &PortalCapture::initialized, &app,
        [&](const PortalCapabilities& capabilities, const PortalFailure& error) {
            print({{"event", "initialized"}, {"platform", capabilities.qtPlatform},
                   {"dispatcher", capabilities.dispatcher},
                   {"screenshotVersion", static_cast<qint64>(capabilities.screenshot.version)},
                   {"screenCastVersion", static_cast<qint64>(capabilities.screenCast.version)},
                   {"sourceTypes", static_cast<qint64>(capabilities.sourceTypes)},
                   {"cursorModes", static_cast<qint64>(capabilities.cursorModes)},
                   {"screenshotFailure", describe(capabilities.screenshot.failure)},
                   {"screenCastFailure", describe(capabilities.screenCast.failure)},
                   {"failure", describe(error)}});
            if (error) { app.exit(1); return; }
            if (!screenshot && !screenCast) { app.exit(0); return; }
            request = screenshot ? control.requestScreenshot(arguments.isSet("unparented") ? nullptr : window.get(),
                !arguments.isSet("noninteractive")) : control.requestScreenCast(window.get());
            if (arguments.isSet("cancel-after-ms")) {
                bool ok = false;
                const auto delay = arguments.value("cancel-after-ms").toInt(&ok);
                if (!ok || delay < 0) { app.exit(2); return; }
                QTimer::singleShot(delay, &control, [&] { control.cancelRequest(request); });
            }
            if (arguments.isSet("destroy-parent-after-ms")) {
                bool ok = false;
                const auto delay = arguments.value("destroy-parent-after-ms").toInt(&ok);
                if (!ok || delay < 0) { app.exit(2); return; }
                app.setQuitOnLastWindowClosed(false);
                QTimer::singleShot(delay, &control, [&] { window.reset(); });
            }
        });
    QObject::connect(&control, &PortalCapture::screenshotFinished, &app,
        [&](quint64 id, const QUrl& uri, const PortalFailure& error) {
            print({{"event", "screenshotFinished"}, {"id", static_cast<qint64>(id)},
                   {"uri", uri.toString(QUrl::FullyEncoded)}, {"failure", describe(error)}});
            app.exit(error ? 1 : 0);
        });
    QObject::connect(&control, &PortalCapture::screenCastFinished, &app,
        [&](quint64 id, const QVector<PortalStream>& streams, const QString& restoreToken, const PortalFailure& error) {
            QJsonArray nodes;
            for (const auto& stream : streams) {
                QJsonObject node{{"nodeId", static_cast<qint64>(stream.nodeId)},
                                 {"sourceType", static_cast<qint64>(stream.sourceType)}, {"id", stream.id}};
                if (stream.pipeWireSerial) node["pipeWireSerial"] = QString::number(*stream.pipeWireSerial);
                if (stream.position) node["logicalPosition"] = QJsonArray{stream.position->x(), stream.position->y()};
                if (stream.size) node["logicalSize"] = QJsonArray{stream.size->width(), stream.size->height()};
                nodes.push_back(node);
            }
            if (!error) remoteFd = control.takePipeWireFd();
            const bool validFd = remoteFd >= 0 && fcntl(remoteFd, F_GETFD) >= 0;
            print({{"event", "screenCastFinished"}, {"id", static_cast<qint64>(id)}, {"streams", nodes},
                   {"hasRestoreToken", !restoreToken.isEmpty()}, {"remoteFdValid", validFd},
                   {"framesDecoded", false}, {"failure", describe(error)}});
            if (error || !validFd) { app.exit(1); return; }
            QTimer::singleShot(5000, &app, [&] { app.exit(0); });
        });
    QObject::connect(&control, &PortalCapture::screenCastClosed, &app, [&](const PortalFailure& error) {
        print({{"event", "screenCastClosed"}, {"failure", describe(error)}});
        if (error) app.exit(1);
    });
    QTimer::singleShot(0, &control, &PortalCapture::initialize);
    QTimer::singleShot(120000, &app, [&] {
        control.cancelRequest(request);
        print({{"event", "probeDeadline"}, {"passed", false}});
        app.exit(1);
    });
    return app.exec();
}
