#include "services/desktop/WindowActivation.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <array>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#ifdef Q_OS_WIN
#include "platform/windows/WindowFocus.h"
#endif
using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& predicate,
    const char* message = "window activation has one bounded terminal result")
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 8000) { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); QThread::msleep(2); }
    require(predicate(), message);
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
#ifdef Q_OS_WIN
void nativeWindowDiagnostic(const char* label, HWND window)
{
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(window, &process);
    std::array<wchar_t, 256> className{};
    const auto classLength = window ? GetClassNameW(window, className.data(), static_cast<int>(className.size())) : 0;
    const auto classError = classLength ? ERROR_SUCCESS : (window ? GetLastError() : ERROR_INVALID_WINDOW_HANDLE);
    std::cout << ' ' << label << '=' << window << ' ' << label << "Process=" << process
        << ' ' << label << "Thread=" << thread << ' ' << label << "Class="
        << std::quoted(QString::fromWCharArray(className.data(), classLength).toStdString())
        << ' ' << label << "ClassError=" << classError;
    const auto handle = process ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process) : nullptr;
    const auto openError = handle ? ERROR_SUCCESS : (process ? GetLastError() : ERROR_INVALID_PARAMETER);
    if (!handle) {
        std::cout << ' ' << label << "ImageError=" << openError;
        return;
    }
    std::array<wchar_t, 32768> image{};
    auto imageLength = static_cast<DWORD>(image.size());
    const auto queried = QueryFullProcessImageNameW(handle, 0, image.data(), &imageLength);
    const auto error = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (queried) std::cout << ' ' << label << "Image="
        << std::quoted(QString::fromWCharArray(image.data(), static_cast<qsizetype>(imageLength)).toStdString());
    else std::cout << ' ' << label << "ImageError=" << error;
}
void guiThreadDiagnostic(const char* label, DWORD thread)
{
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(thread, &info)) {
        const auto error = GetLastError();
        std::cout << ' ' << label << "GuiError=" << error; return;
    }
    std::cout << ' ' << label << "GuiFlags=" << info.flags << ' ' << label << "Active=" << info.hwndActive
        << ' ' << label << "Focus=" << info.hwndFocus << ' ' << label << "Capture=" << info.hwndCapture
        << ' ' << label << "MenuOwner=" << info.hwndMenuOwner << ' ' << label << "MoveSize=" << info.hwndMoveSize;
}
#endif
void diagnostic(const char* stage, QWindow* window, const Desktop::WindowActivationResult* result = nullptr)
{
    static const auto elapsed = [] { QElapsedTimer timer; timer.start(); return timer; }();
    std::cout << "FOCUS " << stage << " elapsed_ms=" << elapsed.elapsed() << " qpa=" << QGuiApplication::platformName().toStdString()
        << " object=" << std::quoted(window ? window->objectName().toStdString() : std::string())
        << " visible=" << (window && window->isVisible()) << " exposed=" << (window && window->isExposed())
        << " qtActive=" << (window && window->isActive()) << " qtFocusMatches=" << (window && QGuiApplication::focusWindow() == window)
        << " nativeFocusMatches=" << nativeFocus(window);
    if (result) std::cout << " requested=" << result->requested << " active=" << result->active
        << " error=" << static_cast<int>(result->failure.code) << " native=" << result->failure.nativeCode
        << " detail=" << result->failure.detail.toStdString();
#ifdef Q_OS_WIN
    const auto target = window && window->handle() ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    const auto foreground = GetForegroundWindow();
    std::cout << " threadFocus=" << GetFocus() << " process=" << GetCurrentProcessId();
    nativeWindowDiagnostic("target", target); nativeWindowDiagnostic("foreground", foreground);
    guiThreadDiagnostic("caller", GetCurrentThreadId()); guiThreadDiagnostic("foreground", 0);
#endif
    std::cout << std::endl;
}
const char* eventName(QEvent::Type type)
{
    switch (type) {
    case QEvent::Show: return "Show";
    case QEvent::Hide: return "Hide";
    case QEvent::Expose: return "Expose";
    case QEvent::FocusIn: return "FocusIn";
    case QEvent::FocusOut: return "FocusOut";
    case QEvent::WindowActivate: return "WindowActivate";
    case QEvent::WindowDeactivate: return "WindowDeactivate";
    case QEvent::MouseButtonPress: return "MouseButtonPress";
    case QEvent::MouseButtonRelease: return "MouseButtonRelease";
    default: return nullptr;
    }
}
class FocusTrace final : public QObject {
public:
    FocusTrace()
    {
        qApp->installEventFilter(this);
        connect(qApp, &QGuiApplication::focusWindowChanged, this, [](QWindow* window) { diagnostic("focusWindowChanged", window); });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [](Qt::ApplicationState state) {
            std::cout << "FOCUS applicationStateChanged state=" << static_cast<int>(state) << std::endl;
        });
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (const auto* name = eventName(event->type()); name && watched->objectName().startsWith(QStringLiteral("activation-"))) {
            auto* window = qobject_cast<QWindow*>(watched);
            if (auto* widget = qobject_cast<QWidget*>(watched)) window = widget->windowHandle();
            std::cout << "FOCUS event=" << name << " spontaneous=" << event->spontaneous()
                << " receiver=" << std::quoted(watched->objectName().toStdString()) << std::endl;
            diagnostic("event-state", window);
        }
        return QObject::eventFilter(watched, event);
    }
};
class ProbeWindow final : public QWidget {
public:
    int paints{}, spontaneousPresses{}, spontaneousReleases{};
    explicit ProbeWindow(const QString& name) { setObjectName(name); setWindowTitle(name); resize(320, 180); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter painter(this); painter.fillRect(rect(), QColor(39, 118, 203)); ++paints; }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && event->spontaneous()) ++spontaneousPresses;
        QWidget::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && event->spontaneous()) ++spontaneousReleases;
        QWidget::mouseReleaseEvent(event);
    }
};
void waitExposed(ProbeWindow& window)
{
    waitFor([&] { return window.windowHandle() && window.windowHandle()->isExposed() && window.paints > 0; },
        "the actual activation fixture is exposed and has painted before requesting focus");
    diagnostic("exposed", window.windowHandle());
}
#ifdef Q_OS_WIN
struct CursorRestore {
    POINT original{};
    bool available{};
    explicit CursorRestore(bool enabled) : available(enabled && GetCursorPos(&original) != FALSE) {}
    ~CursorRestore() { if (available) SetCursorPos(original.x, original.y); }
};
void clickOwnedWindow(ProbeWindow& window)
{
    const auto handle = reinterpret_cast<HWND>(window.windowHandle()->winId());
    RECT client{};
    require(GetClientRect(handle, &client) != FALSE, "read the actual input fixture's native client rectangle");
    POINT point{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
    require(ClientToScreen(handle, &point) != FALSE && SetCursorPos(point.x, point.y) != FALSE,
        "position the real cursor within the owned input fixture");
    diagnostic("before-native-click", window.windowHandle());
    require(GetAncestor(WindowFromPoint(point), GA_ROOT) == handle,
        "the real input point belongs to the owned fixture, with no competing window over it");
    require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0, "the test does not interfere with an already held mouse button");
    std::array<INPUT, 2> input{};
    input[0].type = input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const auto inserted = SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
    const auto error = inserted == input.size() ? ERROR_SUCCESS : GetLastError();
    // A partial insertion must not leave this test's button held on the desktop.
    if (inserted == 1) SendInput(1, &input[1], sizeof(INPUT));
    std::cout << "FOCUS native-click inserted=" << inserted << " error=" << error << std::endl;
    require(inserted == input.size(), "Windows accepts the complete real input fixture click");
    waitFor([&] { return window.spontaneousPresses == 1 && window.spontaneousReleases == 1 && nativeFocus(window.windowHandle()); },
        "the owned fixture receives the real native click and actual foreground focus");
    diagnostic("native-click-received", window.windowHandle());
}
#endif
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        FocusTrace trace;
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
        const auto platform = QGuiApplication::platformName();
        ProbeWindow first(QStringLiteral("activation-target"));
#ifdef Q_OS_WIN
        first.setAttribute(Qt::WA_ShowWithoutActivating);
#endif
        if (auto* screen = QGuiApplication::primaryScreen()) first.move(screen->availableGeometry().center() - first.rect().center());
        first.show();
        const bool desktop = platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal");
        if (desktop) waitExposed(first);
#ifdef Q_OS_WIN
        CursorRestore restoreCursor(desktop);
        ProbeWindow input(QStringLiteral("activation-owned-input"));
        if (desktop) {
            // Establish actual last-input/foreground ownership. The target is a
            // different ordinary window and must be activated by the service.
            // A small test-only topmost input surface avoids clicking a runner's
            // console. Its actual hit owner is still checked before SendInput.
            input.setWindowFlag(Qt::WindowStaysOnTopHint); input.resize(180, 90);
            if (auto* screen = QGuiApplication::primaryScreen()) input.move(screen->availableGeometry().topLeft() + QPoint(24, 24));
            input.show(); waitExposed(input); clickOwnedWindow(input);
            require(!nativeFocus(first.windowHandle()), "the target does not already own focus before the service activates it");
        }
#endif
        diagnostic("before-request", first.windowHandle());
        const auto one = activation.request(first.windowHandle()); targets[one] = first.windowHandle();
        waitFor([&] { return results.contains(one); });
        if (!desktop) {
            require(results.at(one).failure.code == Desktop::Error::Unavailable && !results.at(one).requested && !results.at(one).active,
                "a non-desktop QPA cannot masquerade as real activation");
            std::cout << "PASS actual unsupported-QPA activation refusal (not a desktop focus test)\n"; return 0;
        }
        diagnostic("after-result", first.windowHandle(), &results.at(one));
        require(!results.at(one).failure && results.at(one).requested && results.at(one).active && first.windowHandle()->isActive() && nativeFocus(first.windowHandle()),
            "successful activation is corroborated by the actual native window focus");
        ProbeWindow second(QStringLiteral("activation-cancelled")); second.show();
        const auto stopped = activation.request(second.windowHandle()); targets[stopped] = second.windowHandle(); activation.cancel(stopped);
        waitFor([&] { return results.contains(stopped); });
        require(results.at(stopped).failure.code == Desktop::Error::Cancelled, "cancelling a queued activation does not report success");
        auto* closed = new QWindow; closed->setObjectName(QStringLiteral("activation-destroyed")); closed->resize(320, 180); closed->show();
        const auto closing = activation.request(closed); targets[closing] = closed; delete closed;
        waitFor([&] { return results.contains(closing); });
        require(results.at(closing).failure.code == Desktop::Error::Cancelled, "a deleted native window settles its own request");
        ProbeWindow disappearing(QStringLiteral("activation-hidden-on-focus"));
        disappearing.setAttribute(Qt::WA_ShowWithoutActivating); disappearing.show();
        waitExposed(disappearing);
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
