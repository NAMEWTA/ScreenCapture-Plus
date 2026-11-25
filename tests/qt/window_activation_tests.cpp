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
#ifdef Q_OS_WIN
#include "platform/windows/WindowFocus.h"
#endif
using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 8000) { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); QThread::msleep(2); }
    require(predicate(), "window activation has one bounded terminal result");
}
bool nativeFocus(QWindow* window)
{
    if (!window || !window->handle() || QGuiApplication::focusWindow() != window) return false;
#ifdef Q_OS_WIN
    return Platform::Windows::hasForegroundFocus(reinterpret_cast<HWND>(window->winId()));
#else
    return window->isActive();
#endif
}
void diagnostic(const char* stage, QWindow* window, const Desktop::WindowActivationResult* result = nullptr)
{
    std::cout << "FOCUS " << stage << " qpa=" << QGuiApplication::platformName().toStdString()
        << " visible=" << (window && window->isVisible()) << " exposed=" << (window && window->isExposed())
        << " qtActive=" << (window && window->isActive()) << " qtFocusMatches=" << (window && QGuiApplication::focusWindow() == window)
        << " nativeFocusMatches=" << nativeFocus(window);
    if (result) std::cout << " requested=" << result->requested << " active=" << result->active
        << " error=" << static_cast<int>(result->failure.code) << " native=" << result->failure.nativeCode
        << " detail=" << result->failure.detail.toStdString();
#ifdef Q_OS_WIN
    const auto target = window && window->handle() ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    const auto foreground = GetForegroundWindow();
    DWORD foregroundProcess{};
    const auto foregroundThread = GetWindowThreadProcessId(foreground, &foregroundProcess);
    std::cout << " target=" << target << " foreground=" << foreground << " threadFocus=" << GetFocus()
        << " process=" << GetCurrentProcessId() << " foregroundProcess=" << foregroundProcess
        << " foregroundThread=" << foregroundThread;
#endif
    std::cout << '\n';
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        Desktop::WindowActivation activation;
        std::map<quint64, Desktop::WindowActivationResult> results;
        std::map<quint64, QPointer<QWindow>> targets;
        QObject::connect(&activation, &Desktop::WindowActivation::finished, &activation,
            [&](quint64 token, const Desktop::WindowActivationResult& result) {
                require(!results.contains(token), "one activation token has exactly one result");
                diagnostic("delivery", targets[token], &result); results.emplace(token, result);
            });
        const auto missing = activation.request(nullptr); waitFor([&] { return results.contains(missing); });
        require(results.at(missing).failure.code == Desktop::Error::Unavailable && !results.at(missing).active, "a missing window cannot claim activation");
        QWidget first; first.resize(320, 180); first.show();
        diagnostic("before-request", first.windowHandle());
        const auto one = activation.request(first.windowHandle()); targets[one] = first.windowHandle();
        waitFor([&] { return results.contains(one); });
        const auto platform = QGuiApplication::platformName();
        if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")) {
            require(results.at(one).failure.code == Desktop::Error::Unavailable && !results.at(one).requested && !results.at(one).active,
                "a non-desktop QPA cannot masquerade as real activation");
            std::cout << "PASS actual unsupported-QPA activation refusal (not a desktop focus test)\n"; return 0;
        }
        diagnostic("after-result", first.windowHandle(), &results.at(one));
        require(!results.at(one).failure && results.at(one).requested && results.at(one).active && first.windowHandle()->isActive() && nativeFocus(first.windowHandle()),
            "successful activation is corroborated by the actual native window focus");
        QWidget second; second.resize(320, 180); second.show();
        const auto stopped = activation.request(second.windowHandle()); targets[stopped] = second.windowHandle(); activation.cancel(stopped);
        waitFor([&] { return results.contains(stopped); });
        require(results.at(stopped).failure.code == Desktop::Error::Cancelled, "cancelling a queued activation does not report success");
        auto* closed = new QWindow; closed->resize(320, 180); closed->show();
        const auto closing = activation.request(closed); targets[closing] = closed; delete closed;
        waitFor([&] { return results.contains(closing); });
        require(results.at(closing).failure.code == Desktop::Error::Cancelled, "a deleted native window settles its own request");
        QWidget disappearing; disappearing.resize(320, 180);
        disappearing.setAttribute(Qt::WA_ShowWithoutActivating); disappearing.show();
        bool hiddenOnFocus{};
        const auto focusObserver = QObject::connect(qGuiApp, &QGuiApplication::focusWindowChanged, &activation, [&](QWindow* window) {
            if (window == disappearing.windowHandle()) { hiddenOnFocus = true; disappearing.hide(); }
        });
        const auto transient = activation.request(disappearing.windowHandle()); targets[transient] = disappearing.windowHandle();
        waitFor([&] { return results.contains(transient); });
        QObject::disconnect(focusObserver);
        require(hiddenOnFocus && results.at(transient).failure && !results.at(transient).active,
            "a real window hidden during its focus notification cannot deliver a stale successful activation");
        std::cout << "PASS actual desktop focus, bounded cancellation and native-window destruction\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
