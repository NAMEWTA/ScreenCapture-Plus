#include "services/desktop/DesktopCaptureAction.h"

#include <QGuiApplication>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using namespace HuhuCapture;
constexpr char proofEnvironment[] = "HUHUCAPTURE_DESKTOP_CAPTURE_PROBE";
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
void environmentCases()
{
    for (const auto& value : {QByteArray(), QByteArray("bad"), QByteArray(32, '0'), QByteArray(32, 'g'), QByteArray(4096, 'a')}) {
        Core::StartupRequest request;
        request.action = Core::StartupAction::FinishCapture;
        qputenv(proofEnvironment, value);
        check(bool(Desktop::DesktopCaptureAction::applyProbeEnvironment(request)), "A malformed verification environment was accepted");
        check(request.action == Core::StartupAction::Invalid && !qEnvironmentVariableIsSet(proofEnvironment),
            "Malformed verification must not execute FinishCapture or leak its environment");
    }
    const auto proof = QUuid::createUuid().toRfc4122();
    Core::StartupRequest wrong;
    qputenv(proofEnvironment, proof.toHex());
    check(bool(Desktop::DesktopCaptureAction::applyProbeEnvironment(wrong)), "A verification environment cannot alter an ordinary launch");
    Core::StartupRequest request;
    request.action = Core::StartupAction::FinishCapture;
    qputenv(proofEnvironment, proof.toHex());
    check(!Desktop::DesktopCaptureAction::applyProbeEnvironment(request), "A valid one-use verification challenge did not parse");
    check(request.action == Core::StartupAction::VerifyCaptureAction && !request.oneShot && !request.background &&
        std::equal(request.verification.begin(), request.verification.end(), reinterpret_cast<const std::uint8_t*>(proof.constData())) &&
        !qEnvironmentVariableIsSet(proofEnvironment), "Verification did not preserve the exact bounded challenge");
}
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    environmentCases();
    auto route = std::make_unique<Desktop::DesktopCaptureAction>(QCoreApplication::applicationFilePath());
    int terminals{}, shutdowns{};
    quint64 token{};
    QObject::connect(route.get(), &Desktop::DesktopCaptureAction::verified, &app,
        [&](quint64 actual, const Desktop::Failure& error) {
            check(actual == token && ++terminals == 1, "The unavailable route emitted an unexpected terminal");
            check(error.code == Desktop::Error::Unavailable && !route->available(),
                "A non-desktop QPA advertised a reachable desktop finish action");
            Core::StartupRequest stale;
            stale.action = Core::StartupAction::VerifyCaptureAction; stale.verification.fill(1);
            check(!route->acceptVerification(stale), "An unknown or closed challenge was accepted");
            route->shutdown();
            check(route->verify() == 0, "Shutdown did not close desktop-action admission");
        });
    QObject::connect(route.get(), &Desktop::DesktopCaptureAction::shutdownFinished, &app, [&] {
        check(++shutdowns == 1 && terminals == 1 && !route->available(), "Desktop action shutdown preceded its real terminal callback");
        route.reset();
        auto reentrant = std::make_shared<std::unique_ptr<Desktop::DesktopCaptureAction>>(
            std::make_unique<Desktop::DesktopCaptureAction>(QCoreApplication::applicationFilePath()));
        QObject::connect(reentrant->get(), &Desktop::DesktopCaptureAction::verified, &app,
            [reentrant](quint64, const Desktop::Failure& error) {
                check(error.code == Desktop::Error::Unavailable, "Reentrant route did not retain its actual failure");
                reentrant->reset();
                QTimer::singleShot(0, qGuiApp, &QCoreApplication::quit);
            });
        check((*reentrant)->verify() != 0, "A real unavailable request was not admitted asynchronously");
    });
    QTimer::singleShot(0, &app, [&] {
        token = route->verify();
        check(token && terminals == 0, "Desktop route failure must retain asynchronous delivery");
        route->shutdown(); // queued rejection must precede the shutdown acknowledgment
    });
    QTimer::singleShot(5000, &app, [] { check(false, "Desktop action lifetime test did not drain"); });
    const auto result = app.exec();
    if (!result) std::cout << "PASS bounded proof parsing, no invented desktop capability, closed admission and real Qt callback drain/reentrant destruction\n";
    return result;
}
