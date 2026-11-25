#include <libportal/portal.h>
#include "platform/linux/portal/PortalCapture.h"
#include "platform/linux/portal/PortalShortcuts.h"

#include <QGuiApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace HuhuCapture::Platform::Linux;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

struct NativeMissingService {
    struct Call {
        NativeMissingService* test{};
        int index{};
        GCancellable* cancellable{};
        int completions{};
    };
    std::array<Call, 3> calls;
    int completed{};
    bool portalReleased{};

    void maybeFinish() {
        if (completed == static_cast<int>(calls.size()) && portalReleased)
            QTimer::singleShot(0, qGuiApp, [] { QCoreApplication::exit(EXIT_SUCCESS); });
    }
    static void finished(GObject* source, GAsyncResult* result, gpointer data) {
        auto& call = *static_cast<Call*>(data);
        require(QThread::currentThread() == qGuiApp->thread(), "Shortcut callback left the Qt GUI thread");
        require(++call.completions == 1, "Shortcut request completed more than once");
        g_autoptr(GError) error = nullptr;
        auto* session = xdp_portal_create_global_shortcuts_session_finish(XDP_PORTAL(source), result, &error);
        require(!session && error, "Missing Portal produced a successful shortcut session");
        if (call.index == 0)
            require(g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
                    || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER),
                    "Shortcut request lost the real missing-service error");
        else
            require(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED),
                    "Canceled shortcut request did not report cancellation");
        // Re-cancel after completion detects a stale cancellable handler.
        g_cancellable_cancel(call.cancellable);
        g_clear_object(&call.cancellable);
        ++call.test->completed;
        call.test->maybeFinish();
    }
    void run() {
        g_autoptr(GError) error = nullptr;
        auto* portal = xdp_portal_initable_new(&error);
        require(portal && !error, "Run shortcut fault tests with a real session bus");
        g_object_weak_ref(G_OBJECT(portal), [](gpointer data, GObject*) {
            auto* test = static_cast<NativeMissingService*>(data);
            test->portalReleased = true;
            test->maybeFinish();
        }, this);
        for (int index = 0; index < static_cast<int>(calls.size()); ++index) {
            auto& call = calls[index];
            call.test = this;
            call.index = index;
            call.cancellable = g_cancellable_new();
            if (index == 2) g_cancellable_cancel(call.cancellable);
            xdp_portal_create_global_shortcuts_session(portal, call.cancellable, finished, &call);
            if (index == 1) g_cancellable_cancel(call.cancellable);
        }
        g_object_unref(portal);
        // Success requires actual finalization after all late callbacks release
        // their Call refs; a time interval alone is not evidence of cleanup.
    }
};

struct Disconnection {
    QProcess daemon;
    PortalCapture* capture{};
    PortalShortcuts* shortcuts{};
    GDBusConnection* bus{};
    gulong closedHandler{};
    int failedRequests{};
    bool closed{};
    bool daemonExited{};

    ~Disconnection() {
        if (bus) g_clear_signal_handler(&closedHandler, bus);
        g_clear_object(&bus);
        if (daemon.state() != QProcess::NotRunning) {
            daemon.kill();
            daemon.waitForFinished(3000);
        }
    }
    void startDaemon() {
        // This is a real, isolated daemon, not an implementation of Portal.
        daemon.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"), QStringLiteral("--nofork"),
            QStringLiteral("--print-address=1"), QStringLiteral("--nopidfile")});
        require(daemon.waitForStarted(3000), "Could not start the real temporary dbus-daemon");
        QElapsedTimer deadline;
        deadline.start();
        while (!daemon.canReadLine() && deadline.elapsed() < 3000)
            daemon.waitForReadyRead(static_cast<int>(3000 - deadline.elapsed()));
        require(daemon.canReadLine(), "Temporary dbus-daemon did not publish its address");
        const auto address = daemon.readLine().trimmed();
        require(address.startsWith("unix:"), "Temporary dbus-daemon returned an unexpected address");
        qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    }
    void maybeFinish() {
        if (closed && daemonExited && failedRequests == 2)
            QTimer::singleShot(0, qGuiApp, [] { QCoreApplication::exit(EXIT_SUCCESS); });
    }
    void disconnect() {
        g_autoptr(GError) error = nullptr;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error); // already initialized/cached
        require(bus && !error, "Cannot obtain the already initialized session bus");
        require(!g_dbus_connection_get_exit_on_close(bus), "Shared session bus can still terminate the application");
        closedHandler = g_signal_connect(bus, "closed", G_CALLBACK(+[](GDBusConnection*, gboolean, GError*, gpointer data) {
            auto* test = static_cast<Disconnection*>(data);
            require(QThread::currentThread() == qGuiApp->thread(), "Bus closure was dispatched outside the Qt GLib context");
            require(!test->closed, "Bus disconnected more than once");
            test->closed = true;
            test->capture->requestScreenshot(nullptr);
            test->shortcuts->bindShortcuts(nullptr, {{QStringLiteral("capture"), QStringLiteral("Capture"), QStringLiteral("CTRL+SHIFT+S")}});
            test->maybeFinish();
        }), this);
        QObject::connect(&daemon, &QProcess::finished, qGuiApp, [this](int, QProcess::ExitStatus) {
            daemonExited = true;
            maybeFinish();
        });
        QObject::connect(capture, &PortalCapture::screenshotFinished, qGuiApp,
            [this](quint64, const QUrl& uri, const PortalFailure& failure) {
                require(!uri.isValid() && failure.code == PortalFailure::Code::Unavailable,
                        "Capture accepted a request after session bus disconnection");
                ++failedRequests;
                maybeFinish();
            });
        QObject::connect(shortcuts, &PortalShortcuts::bindingFinished, qGuiApp,
            [this](quint64, const QVector<PortalShortcutBinding>& assigned, const PortalFailure& failure) {
                require(assigned.isEmpty() && failure.code == PortalFailure::Code::Unavailable,
                        "Shortcuts accepted a request after session bus disconnection");
                ++failedRequests;
                maybeFinish();
            });
        daemon.terminate();
    }
};

int shutdownSequence(QGuiApplication& app)
{
    struct Case {
        PortalShortcuts control;
        int initialized{}, rejected{}, drained{};
        quint64 request{};
    };
    std::array<Case, 4> cases;
    int finished{};
    const QVector<PortalShortcutRequest> requested{{QStringLiteral("capture"), QStringLiteral("Capture"), QStringLiteral("CTRL+SHIFT+F11")}};
    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto& item = cases[index];
        QObject::connect(&item.control, &PortalShortcuts::initialized, &app,
            [&, index](const PortalShortcutCapabilities&, const PortalFailure& failure) {
                auto& current = cases[index];
                require((index == 0 || index == 3) && ++current.initialized == 1 && failure.code ==
                    (index == 0 ? PortalFailure::Code::Cancelled : PortalFailure::Code::Unavailable),
                    "Actual initialization cancellation or missing service must complete exactly once before drain");
                if (index == 3) current.control.shutdown();
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                require(current.drained == 0, "Shortcut shutdown completed inside its live initialization callback");
            });
        QObject::connect(&item.control, &PortalShortcuts::bindingFinished, &app,
            [&, index](quint64 id, const QVector<PortalShortcutBinding>& assigned, const PortalFailure& failure) {
                auto& current = cases[index];
                require(index == 1 && id == current.request && ++current.rejected == 1 && assigned.isEmpty() &&
                    failure.code == PortalFailure::Code::Unavailable,
                    "The actual uninitialized binding rejection must keep its one terminal result");
                current.control.shutdown();
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                require(current.drained == 0, "Shortcut shutdown completed before its queued rejection callback returned");
            });
        QObject::connect(&item.control, &PortalShortcuts::shutdownFinished, &app, [&, index] {
            auto& current = cases[index];
            require(++current.drained == 1 && current.initialized == (index == 0 || index == 3 ? 1 : 0) &&
                current.rejected == (index == 1 ? 1 : 0), "Shortcut shutdown preceded its real pending deliveries");
            require(!current.control.available() && !current.control.hasActiveBindings() && current.control.bindings().isEmpty(),
                "A drained shortcut owner still advertises a native binding");
            require(current.control.bindShortcuts(nullptr, requested) == 0 && current.control.restoreShortcuts(requested) == 0,
                "A drained shortcut owner reopened request admission");
            current.control.initialize(); current.control.shutdown();
            if (++finished == static_cast<int>(cases.size()))
                QTimer::singleShot(0, &app, [] { QCoreApplication::exit(EXIT_SUCCESS); });
        });
    }
    QTimer::singleShot(0, &app, [&] {
        cases[0].control.initialize(); cases[0].control.shutdown();
        cases[1].request = cases[1].control.bindShortcuts(nullptr, requested);
        require(cases[1].request && cases[1].rejected == 0, "The real admission error is delivered asynchronously");
        cases[1].control.shutdown();
        cases[2].control.shutdown();
        cases[3].control.initialize();
        for (std::size_t index = 0; index < 3; ++index) {
            auto& item = cases[index];
            require(item.drained == 0, "Shortcut shutdown must wait for its asynchronous drain notification");
            require(item.control.bindShortcuts(nullptr, requested) == 0, "Shutdown closes admission before its queued acknowledgement");
        }
    });
    QTimer::singleShot(15000, &app, [] {
        std::cerr << "Actual shortcut initialization/rejection shutdown did not drain\n";
        QCoreApplication::exit(EXIT_FAILURE);
    });
    const auto status = app.exec();
    if (status == EXIT_SUCCESS) std::cout << "Real shortcut initialization, rejection and closed-admission callbacks drained in order\n";
    return status;
}

} // namespace

int main(int argc, char** argv)
{
    bool noBus{}, disconnectBus{}, shutdown{};
    for (int index = 1; index < argc; ++index) {
        noBus |= std::string_view(argv[index]) == "--no-bus";
        disconnectBus |= std::string_view(argv[index]) == "--disconnect-bus";
        shutdown |= std::string_view(argv[index]) == "--shutdown";
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("LIBPORTAL_PORTAL_BUS_NAME", "org.huhucapture.PortalUnavailable");
    if (noBus) qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/huhucapture-session-bus");
    QGuiApplication app(argc, argv);
    if (shutdown) return shutdownSequence(app);
    Disconnection disconnection;
    if (disconnectBus) disconnection.startDaemon();
    PortalCapture capture;
    PortalShortcuts shortcuts;
    disconnection.capture = &capture;
    disconnection.shortcuts = &shortcuts;
    NativeMissingService native;
    int initialized{};
    const auto initializationFinished = [&] {
        if (++initialized != 2) return;
        if (noBus) QCoreApplication::exit(EXIT_SUCCESS);
        else if (disconnectBus) disconnection.disconnect();
        else native.run();
    };
    QObject::connect(&capture, &PortalCapture::initialized, &app,
        [&](const PortalCapabilities& capabilities, const PortalFailure& failure) {
            require(failure.code == PortalFailure::Code::Unavailable
                    && !capabilities.screenshot.available() && !capabilities.screenCast.available(),
                    "Capture advertised unavailable Portal capabilities");
            initializationFinished();
        });
    QObject::connect(&shortcuts, &PortalShortcuts::initialized, &app,
        [&](const PortalShortcutCapabilities& capabilities, const PortalFailure& failure) {
            require(QThread::currentThread() == app.thread(), "Shortcut initialization left the GUI thread");
            require(failure.code == PortalFailure::Code::Unavailable && !capabilities.globalShortcuts.available(),
                    "Shortcuts advertised an unavailable service");
            initializationFinished();
        });
    QTimer::singleShot(0, &capture, &PortalCapture::initialize);
    QTimer::singleShot(0, &shortcuts, &PortalShortcuts::initialize);
    QTimer::singleShot(15000, &app, [] {
        std::cerr << "Real Portal shortcut/bus fault test did not complete or release callbacks\n";
        QCoreApplication::exit(EXIT_FAILURE);
    });
    const auto status = app.exec();
    if (status == EXIT_SUCCESS)
        std::cout << (disconnectBus ? "Actual session bus termination survived with unavailable results\n"
                                   : "Real shortcut service/bus failure and cancellation checks passed\n");
    return status;
}
