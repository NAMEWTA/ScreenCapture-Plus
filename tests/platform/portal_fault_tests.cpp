#include <libportal/portal.h>
#include "platform/linux/portal/PortalCapture.h"

#include <QAbstractEventDispatcher>
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
        call.test->maybeFinish();
    }

    void run() {
        g_autoptr(GError) error = nullptr;
        // The controller already obtained this cached bus asynchronously.
        auto* portal = xdp_portal_initable_new(&error);
        require(portal && !error, "The real session bus is unavailable; run this test under dbus-run-session");
        g_object_weak_ref(G_OBJECT(portal), [](gpointer data, GObject*) {
            auto* test = static_cast<MissingServiceCases*>(data);
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
        // The weak notification is the completion condition: pending D-Bus
        // callbacks must release their references, rather than just timing out.
    }
};

} // namespace

int main(int argc, char** argv)
{
    const bool noBus = argc == 2 && std::string_view(argv[1]) == "--no-bus";
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("LIBPORTAL_PORTAL_BUS_NAME", "org.huhucapture.PortalUnavailable");
    if (noBus) qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/huhucapture-session-bus");
    QGuiApplication app(argc, argv);
    PortalCapture control;
    MissingServiceCases nativeCases;
    QObject::connect(&control, &PortalCapture::initialized, &app,
        [&](const PortalCapabilities& capabilities, const PortalFailure& error) {
            require(QThread::currentThread() == app.thread(), "Initialization completion left the GUI thread");
            require(error.code == PortalFailure::Code::Unavailable,
                    "Unavailable bus/service was not reported as unavailable");
            require(!capabilities.screenshot.available() && !capabilities.screenCast.available(),
                    "Unavailable Portal advertised capture support");
            require(QAbstractEventDispatcher::instance()->inherits("QEventDispatcherGlib"),
                    "Qt is not dispatching the GLib context");
            if (noBus) QCoreApplication::exit(EXIT_SUCCESS);
            else nativeCases.run();
        });
    QTimer::singleShot(0, &control, &PortalCapture::initialize);
    QTimer::singleShot(10000, &app, [&] {
        std::cerr << "Portal fault test did not complete or release its requests\n";
        QCoreApplication::exit(EXIT_FAILURE);
    });
    const auto result = app.exec();
    if (result == EXIT_SUCCESS)
        std::cout << (noBus ? "Unavailable session bus propagated without abort\n"
                            : "Real D-Bus failure, cancellation and callback release checks passed\n");
    return result;
}
