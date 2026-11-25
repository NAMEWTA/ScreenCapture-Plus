#include "services/desktop/WindowActivation.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 8000) { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); QThread::msleep(2); }
    require(predicate(), "window activation has one bounded terminal result");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        Desktop::WindowActivation activation;
        std::map<quint64, Desktop::WindowActivationResult> results;
        QObject::connect(&activation, &Desktop::WindowActivation::finished, &activation,
            [&](quint64 token, const Desktop::WindowActivationResult& result) {
                require(!results.contains(token), "one activation token has exactly one result"); results.emplace(token, result);
            });
        const auto missing = activation.request(nullptr); waitFor([&] { return results.contains(missing); });
        require(results.at(missing).failure.code == Desktop::Error::Unavailable && !results.at(missing).active, "a missing window cannot claim activation");
        QWidget first; first.resize(320, 180); first.show();
        const auto one = activation.request(first.windowHandle()); waitFor([&] { return results.contains(one); });
        const auto platform = QGuiApplication::platformName();
        if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")) {
            require(results.at(one).failure.code == Desktop::Error::Unavailable && !results.at(one).requested && !results.at(one).active,
                "a non-desktop QPA cannot masquerade as real activation");
            std::cout << "PASS actual unsupported-QPA activation refusal (not a desktop focus test)\n"; return 0;
        }
        require(!results.at(one).failure && results.at(one).requested && results.at(one).active && first.windowHandle()->isActive(),
            "successful activation is corroborated by the actual native window focus");
        QWidget second; second.resize(320, 180); second.show();
        const auto stopped = activation.request(second.windowHandle()); activation.cancel(stopped);
        waitFor([&] { return results.contains(stopped); });
        require(results.at(stopped).failure.code == Desktop::Error::Cancelled, "cancelling a queued activation does not report success");
        auto* closed = new QWindow; closed->resize(320, 180); closed->show();
        const auto closing = activation.request(closed); delete closed;
        waitFor([&] { return results.contains(closing); });
        require(results.at(closing).failure.code == Desktop::Error::Cancelled, "a deleted native window settles its own request");
        std::cout << "PASS actual desktop focus, bounded cancellation and native-window destruction\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
