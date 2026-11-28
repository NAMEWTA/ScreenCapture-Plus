#include "app/SingleInstance.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using HuhuCapture::Application::InstanceResult;
using HuhuCapture::Application::InstanceRole;
using HuhuCapture::Application::SingleInstance;

void record(const QJsonObject& object)
{
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}

QString option(const QStringList& arguments, const QString& name, const QString& fallback = {})
{
    for (const auto& argument : arguments)
        if (argument.startsWith(name + '=')) return argument.sliced(name.size() + 1);
    return fallback;
}

int rawPeer(QCoreApplication& app, const QStringList& arguments)
{
    QLocalSocket socket;
    QTimer deadline;
    deadline.setSingleShot(true);
    const auto mode = option(arguments, QStringLiteral("--raw"));
    bool connected = false;
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        record({{"event", "raw"}, {"passed", false}, {"detail", "peer did not close within the bound"}});
        app.exit(3);
    });
    QObject::connect(&socket, &QLocalSocket::connected, &app, [&] {
        connected = true;
        QByteArray bytes;
        if (mode == QStringLiteral("control-v1") || mode == QStringLiteral("control-v2-flags") || mode == QStringLiteral("control-v2-launch")) {
            bytes = QByteArrayLiteral("HHCP");
            bytes.append(mode == QStringLiteral("control-v1") ? char(1) : char(2));
            bytes.append(char(0));
            bytes.append(mode == QStringLiteral("control-v2-flags") ? char(1) : char(0));
            bytes.append(mode == QStringLiteral("control-v2-launch") ? char(0) : char(1));
            bytes.append(QUuid::createUuid().toRfc4122());
        } else bytes = mode == "partial" ? QByteArray("HH") : QByteArray(65536, 'x');
        socket.write(bytes);
    });
    QObject::connect(&socket, &QLocalSocket::disconnected, &app, [&] {
        record({{"event", "raw"}, {"passed", connected}});
        app.exit(connected ? 0 : 3);
    });
    QObject::connect(&socket, &QLocalSocket::errorOccurred, &app, [&](QLocalSocket::LocalSocketError error) {
        if (connected && error == QLocalSocket::PeerClosedError) return;
        record({{"event", "raw"}, {"passed", false}, {"detail", socket.errorString()}});
        app.exit(3);
    });
    deadline.start(6000);
    socket.connectToServer(option(arguments, QStringLiteral("--endpoint")));
    return app.exec();
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = QCoreApplication::arguments();
    if (!option(arguments, QStringLiteral("--raw")).isEmpty()) return rawPeer(app, arguments);
    std::vector<std::string_view> commandLine;
    for (int index = 1; index < argc; ++index) commandLine.emplace_back(argv[index]);
    auto request = HuhuCapture::Core::parseStartupArguments<char>(commandLine);
    const auto proof = option(arguments, QStringLiteral("--verification"));
    if (!proof.isEmpty()) {
        const auto bytes = QByteArray::fromHex(proof.toLatin1());
        if (bytes.size() != 16 || bytes.toHex() != proof.toLatin1().toLower()) return 4;
        request = {};
        request.action = HuhuCapture::Core::StartupAction::VerifyCaptureAction;
        std::copy(bytes.begin(), bytes.end(), request.verification.begin());
    }
    int heartbeats = 0;
    int activations = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
    heartbeat.start();
    std::unique_ptr<SingleInstance> instance;
    const int exitAfter = option(arguments, QStringLiteral("--exit-after"), "0").toInt();
    const bool reject = arguments.contains(QStringLiteral("--reject"));
    const bool destroyOnActivation = arguments.contains(QStringLiteral("--destroy-on-activation"));
    const bool throwAfterActivation = arguments.contains(QStringLiteral("--throw-after-activation"));
    instance = std::make_unique<SingleInstance>(option(arguments, QStringLiteral("--runtime")),
        option(arguments, QStringLiteral("--scope"), "probe"), [&](const auto& action) {
            ++activations;
            record({{"event", "activation"}, {"entry", int(action.entry)}, {"background", action.background},
                {"oneShot", action.oneShot}, {"count", activations}, {"action", int(action.action)},
                {"verification", QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(action.verification.data()),
                    action.verification.size()).toHex())}});
            if (destroyOnActivation) {
                instance.reset();
                QTimer::singleShot(30, &app, &QCoreApplication::quit);
                return false;
            }
            if (exitAfter > 0 && activations >= exitAfter)
                QTimer::singleShot(80, &app, &QCoreApplication::quit);
            if (throwAfterActivation) throw std::runtime_error("Host failed after taking ownership of the activation");
            return !reject;
        });
    QObject::connect(instance.get(), &SingleInstance::startupFinished, &app, [&](const InstanceResult& result) {
        if (result.role == InstanceRole::Primary && arguments.contains(QStringLiteral("--stop-accepting")))
            instance->stopAccepting();
        record({{"event", "startup"}, {"role", int(result.role)}, {"failure", int(result.failure)},
            {"detail", result.detail}, {"endpoint", instance->endpoint()}, {"heartbeats", heartbeats},
            {"entry", int(request.entry)}, {"background", request.background}, {"oneShot", request.oneShot},
            {"action", int(request.action)}});
        if (result.role == InstanceRole::Primary)
            QTimer::singleShot(option(arguments, QStringLiteral("--hold"), "80").toInt(), &app, &QCoreApplication::quit);
        else QTimer::singleShot(0, &app, [&app, role = result.role] { app.exit(role == InstanceRole::Failed ? 2 : 0); });
    });
    if (!instance->start(request) || instance->start(request)) return 4;
    const int result = app.exec();
    record({{"event", "exit"}, {"activations", activations}, {"heartbeats", heartbeats}});
    return result;
}
