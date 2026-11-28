#include <libportal/portal.h>
#include "platform/linux/portal/PortalCapture.h"

#include <QAbstractEventDispatcher>
#include <QEventLoop>
#include <QGuiApplication>
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
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// This uses a real session bus and a deliberately absent service. There is no
// fake Portal, fabricated screenshot, fake permission dialog, or PipeWire stream.
struct MissingServiceCases {
    struct Call {
        MissingServiceCases* test{};
        int index{};
        GCancellable* cancel{};
        int completions{};
    };
    std::array<Call, 3> calls;
    int completed{};
    int activeCallbacks{};
    bool portalReleased{};
    bool glibDispatched{};

    void maybeFinish() {
        if (completed == static_cast<int>(calls.size()) && portalReleased && glibDispatched)
            QTimer::singleShot(0, qGuiApp, [] { QCoreApplication::exit(EXIT_SUCCESS); });
    }

    static void screenshotFinished(GObject* source, GAsyncResult* result, gpointer data) {
        auto& call = *static_cast<Call*>(data);
        require(QThread::currentThread() == qGuiApp->thread(), "Portal callback left the Qt GUI thread");
        require(++call.completions == 1, "Portal request completed more than once");
        ++call.test->activeCallbacks;
        g_autoptr(GError) error = nullptr;
        g_autofree char* uri = xdp_portal_take_screenshot_finish(XDP_PORTAL(source), result, &error);
        require(!uri && error, "Missing Portal service produced a successful screenshot");
        if (call.index == 0) {
            require(g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
                    || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER),
                    "Missing service did not preserve its real D-Bus error");
        } else {
            require(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED),
                    "Canceled screenshot did not report cancellation");
        }
        // Cleanup cancellation after a terminal error must not call a freed
        // handler. The immediately canceled case can still have a late reply.
        g_cancellable_cancel(call.cancel);
        g_clear_object(&call.cancel);
        ++call.test->completed;
        --call.test->activeCallbacks;
        call.test->maybeFinish();
    }

    void run() {
        g_autoptr(GError) error = nullptr;
        // The controller already obtained this cached bus asynchronously.
        auto* portal = xdp_portal_initable_new(&error);
        require(portal && !error, "The real session bus is unavailable; run this test under dbus-run-session");
        g_object_weak_ref(G_OBJECT(portal), [](gpointer data, GObject*) {
            auto* test = static_cast<MissingServiceCases*>(data);
            require(QThread::currentThread() == qGuiApp->thread(), "Portal's last native reference was released on another thread");
            require(test->activeCallbacks == 0, "Portal released its last native reference inside a public request completion");
            test->portalReleased = true;
            test->maybeFinish();
        }, this);

        auto* idle = g_idle_source_new();
        g_source_set_callback(idle, [](gpointer data) -> gboolean {
            auto* test = static_cast<MissingServiceCases*>(data);
            require(QThread::currentThread() == qGuiApp->thread(), "Default GLib context is dispatched on another thread");
            test->glibDispatched = true;
            test->maybeFinish();
            return G_SOURCE_REMOVE;
        }, this, nullptr);
        g_source_attach(idle, g_main_context_default());
        g_source_unref(idle);

        for (int index = 0; index < static_cast<int>(calls.size()); ++index) {
            auto& call = calls[index];
            call.test = this;
            call.index = index;
            call.cancel = g_cancellable_new();
            if (index == 2) g_cancellable_cancel(call.cancel);
            xdp_portal_take_screenshot(portal, nullptr, XDP_SCREENSHOT_FLAG_NONE,
                                      call.cancel, screenshotFinished, &call);
            if (index == 1) g_cancellable_cancel(call.cancel);
        }
        g_object_unref(portal);
        // The weak notification proves pending D-Bus references were released.
        // maybeFinish queues the exit after this stack: GObject invokes weak
        // notifications before running the object's finalize method.
    }
};

} // namespace

int main(int argc, char** argv)
{
    bool noBus = false, stopInitializing = false, lateRequest = false, remoteWithoutSession = false;
    for (int index = 1; index < argc; ++index) {
        noBus |= std::string_view(argv[index]) == "--no-bus";
        stopInitializing |= std::string_view(argv[index]) == "--shutdown-initializing";
        lateRequest |= std::string_view(argv[index]) == "--shutdown-with-late-request";
        remoteWithoutSession |= std::string_view(argv[index]) == "--remote-without-session";
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("LIBPORTAL_PORTAL_BUS_NAME", "org.huhucapture.PortalUnavailable");
    if (noBus) qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/huhucapture-session-bus");
    QGuiApplication app(argc, argv);
    PortalCapture control;
    MissingServiceCases nativeCases;
    int initializationCompletions = 0, shutdownCompletions = 0, remoteCompletions = 0;
    int deliveryDepth = 0;
    quint64 remoteRequest{};
    const auto requireDeferredShutdown = [&] {
        // A real host can dispatch Qt events while handling a terminal signal.
        // Neither cancellation nor a queued acknowledgement may overtake the
        // currently executing public completion and its native result cleanup.
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        require(shutdownCompletions == 0, "Portal shutdown preceded the return of its actual completion callback");
    };
    QObject::connect(&control, &PortalCapture::pipeWireRemoteOpened, &app,
        [&](quint64 request, const PortalFailure& error) {
            ++deliveryDepth;
            require(remoteWithoutSession && request == remoteRequest && ++remoteCompletions == 1,
                "Remote admission completed an unexpected request");
            require(error.code == PortalFailure::Code::InvalidState && control.takePipeWireFd() == -1,
                "Opening a remote without authorization produced a connection");
            control.shutdown();
            requireDeferredShutdown();
            --deliveryDepth;
        });
    QObject::connect(&control, &PortalCapture::shutdownFinished, &app, [&] {
        require(deliveryDepth == 0, "Portal shutdown reentered a public completion callback");
        require(++shutdownCompletions == 1, "Portal shutdown completed more than once");
        require(initializationCompletions == (lateRequest || remoteWithoutSession ? 0 : 1), "Portal shutdown preceded its actual initialization completion");
        require(control.requestScreenshot(nullptr) == 0 && control.requestScreenCast(nullptr) == 0 && control.openPipeWireRemote() == 0,
            "A drained Portal controller reopened request admission");
        if (noBus || stopInitializing || lateRequest || remoteWithoutSession) QCoreApplication::exit(EXIT_SUCCESS);
        else nativeCases.run();
    });
    QObject::connect(&control, &PortalCapture::initialized, &app,
        [&](const PortalCapabilities& capabilities, const PortalFailure& error) {
            ++deliveryDepth;
            require(QThread::currentThread() == app.thread(), "Initialization completion left the GUI thread");
            require(++initializationCompletions == 1, "Initialization completed more than once");
            require(error.code == (stopInitializing ? PortalFailure::Code::Cancelled : PortalFailure::Code::Unavailable),
                    "Unavailable/cancelled initialization did not retain its actual outcome");
            require(!capabilities.screenshot.available() && !capabilities.screenCast.available(),
                    "Unavailable Portal advertised capture support");
            require(QAbstractEventDispatcher::instance()->inherits("QEventDispatcherGlib"),
                    "Qt is not dispatching the GLib context");
            if (!stopInitializing) control.shutdown();
            requireDeferredShutdown();
            --deliveryDepth;
        });
    QTimer::singleShot(0, &control, [&] {
        if (remoteWithoutSession) {
            remoteRequest = control.openPipeWireRemote();
            require(remoteRequest != 0 && remoteCompletions == 0, "Remote admission rejected without its asynchronous terminal");
            control.shutdown();
        } else if (lateRequest) {
            control.shutdown();
            require(control.requestScreenshot(nullptr) == 0 && control.requestScreenCast(nullptr) == 0 && control.openPipeWireRemote() == 0,
                "Shutdown must close admission before queueing its drained signal");
        } else {
            control.initialize();
            if (stopInitializing) control.shutdown();
        }
    });
    QTimer::singleShot(10000, &app, [&] {
        std::cerr << "Portal fault test did not complete or release its requests\n";
        QCoreApplication::exit(EXIT_FAILURE);
    });
    const auto result = app.exec();
    if (result == EXIT_SUCCESS)
        std::cout << (remoteWithoutSession ? "Unauthorized PipeWire remote admission failed without creating an FD and drained\n" :
            lateRequest ? "Shutdown closes real Portal request admission before its queued terminal signal\n" :
            stopInitializing ? "Actual initialization cancellation drained before Portal shutdown completion\n" :
            noBus ? "Unavailable session bus propagated and shutdown drained without abort\n"
                  : "Real D-Bus failure, cancellation, callback release and controller shutdown checks passed\n");
    return result;
}
