#include "platform/linux/portal/PortalShortcuts.h"

#include <QCommandLineParser>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QWindow>
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
            {"nativeCode", error.nativeCode}, {"remoteError", error.remoteError}, {"message", error.message}};
}

QJsonArray assignments(const QVector<PortalShortcutBinding>& bindings)
{
    QJsonArray result;
    for (const auto& binding : bindings)
        result.push_back(QJsonObject{{"id", binding.id}, {"description", binding.description},
                                     {"triggerDescription", binding.triggerDescription}});
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("huhucapture-shortcuts-probe"));
    app.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser arguments;
    arguments.setApplicationDescription(QStringLiteral("Actual GlobalShortcuts probe; requires a desktop Portal and real key activation"));
    arguments.addHelpOption();
    arguments.addOption({"bind", "Bind a real shortcut and require an Activated signal"});
    arguments.addOption({"trigger", "Preferred XDG shortcut trigger", "trigger", "CTRL+SHIFT+F11"});
    arguments.addOption({"hold-ms", "Time to press the assigned shortcut after the final binding result", "milliseconds", "15000"});
    arguments.addOption({"cancel-after-ms", "Cancel the first pending binding", "milliseconds"});
    arguments.addOption({"destroy-parent-after-ms", "Destroy the first authorization parent", "milliseconds"});
    arguments.addOption({"close-parent-after-bind", "Destroy the parent after successful binding, then require activation"});
    arguments.addOption({"rebind-trigger", "Try a second real session with another preferred trigger", "trigger"});
    arguments.addOption({"cancel-rebind-after-ms", "Cancel the replacement request and require the old shortcut to still activate", "milliseconds"});
    arguments.addOption({"shutdown-after-bind", "Drain from the actual successful binding callback"});
    arguments.addOption({"shutdown-on-refresh", "After binding, change the real desktop assignment and drain from its refresh callback"});
    arguments.process(app);
    if ((arguments.isSet("cancel-after-ms") && arguments.isSet("destroy-parent-after-ms"))
        || (arguments.isSet("rebind-trigger") && (arguments.isSet("close-parent-after-bind")
            || arguments.isSet("cancel-after-ms") || arguments.isSet("destroy-parent-after-ms")))
        || (arguments.isSet("cancel-rebind-after-ms") && !arguments.isSet("rebind-trigger"))
        || ((arguments.isSet("shutdown-after-bind") || arguments.isSet("shutdown-on-refresh")) &&
            (!arguments.isSet("bind") || arguments.isSet("rebind-trigger") || arguments.isSet("cancel-after-ms") ||
             arguments.isSet("destroy-parent-after-ms")))
        || (arguments.isSet("shutdown-after-bind") && arguments.isSet("shutdown-on-refresh"))) return 2;
    const auto delay = [&](const char* name, int fallback = -1) {
        if (!arguments.isSet(name) && fallback < 0) return fallback;
        bool ok = false;
        const auto value = arguments.value(name).toInt(&ok);
        return ok && value >= 0 ? value : -2;
    };
    const int holdMs = delay("hold-ms", 15000);
    const int cancelMs = delay("cancel-after-ms");
    const int destroyMs = delay("destroy-parent-after-ms");
    const int cancelRebindMs = delay("cancel-rebind-after-ms");
    if (holdMs < 0 || cancelMs == -2 || destroyMs == -2 || cancelRebindMs == -2) return 2;

    PortalShortcuts control;
    auto window = std::make_unique<QWindow>();
    window->setTitle(QStringLiteral("huhucapture GlobalShortcuts validation — press the assigned shortcut"));
    window->resize(640, 240);
    if (arguments.isSet("bind")) window->show();
    quint64 request{};
    int completedBindings{};
    int activations{};
    bool rebindStarted{};
    bool pending{};
    QJsonArray previous;
    QTimer activationDeadline;
    activationDeadline.setSingleShot(true);
    QTimer drainDeadline; drainDeadline.setSingleShot(true);
    int exitStatus{-1}, changes{};
    bool drained{};
    const auto finish = [&](int status) {
        if (exitStatus >= 0) return;
        exitStatus = status; activationDeadline.stop(); drainDeadline.start(15000);
        control.shutdown();
    };
    QObject::connect(&control, &PortalShortcuts::shutdownFinished, &app, [&] {
        drained = true; drainDeadline.stop();
        print({{"event", "shutdownFinished"}, {"pending", pending}, {"bindingResults", completedBindings}});
        app.exit(exitStatus >= 0 && !pending ? exitStatus : 1);
    });
    QObject::connect(&drainDeadline, &QTimer::timeout, &app, [&] {
        print({{"event", "shutdownDeadline"}, {"passed", false}}); app.exit(1);
    });
    const auto begin = [&](const QString& trigger) {
        pending = true;
        request = control.bindShortcuts(window.get(), {{QStringLiteral("capture"), QStringLiteral("Capture screen"), trigger}});
    };
    QObject::connect(&control, &PortalShortcuts::initialized, &app,
        [&](const PortalShortcutCapabilities& capabilities, const PortalFailure& error) {
            print({{"event", "initialized"}, {"platform", capabilities.qtPlatform}, {"dispatcher", capabilities.dispatcher},
                   {"globalShortcutsVersion", static_cast<qint64>(capabilities.globalShortcuts.version)},
                   {"failure", describe(error)}});
            if (error) { finish(1); return; }
            if (!arguments.isSet("bind")) { finish(0); return; }
            begin(arguments.value("trigger"));
            if (cancelMs >= 0) QTimer::singleShot(cancelMs, &control, [&] { control.cancelRequest(request); });
            if (destroyMs >= 0) QTimer::singleShot(destroyMs, &control, [&] { window.reset(); });
        });
    QObject::connect(&control, &PortalShortcuts::bindingFinished, &app,
        [&](quint64 id, const QVector<PortalShortcutBinding>& assigned, const PortalFailure& error) {
            pending = false;
            ++completedBindings;
            const auto current = assignments(control.bindings());
            const bool previousRetained = rebindStarted && control.hasActiveBindings() && current == previous;
            print({{"event", "bindingFinished"}, {"id", static_cast<qint64>(id)}, {"assigned", assignments(assigned)},
                   {"active", current}, {"previousRetained", previousRetained}, {"failure", describe(error)}});
            if (cancelMs >= 0) { finish(error.code == PortalFailure::Code::Cancelled ? 0 : 1); return; }
            if (destroyMs >= 0) { finish(error.code == PortalFailure::Code::ParentDestroyed ? 0 : 1); return; }
            if (rebindStarted && cancelRebindMs >= 0) {
                if (error.code != PortalFailure::Code::Cancelled || !previousRetained) { finish(1); return; }
            } else if (error) { finish(1); return; }
            if (arguments.isSet("shutdown-after-bind")) {
                finish(0);
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                if (drained) { print({{"event", "shutdownBeforeBindingReturned"}, {"passed", false}}); app.exit(1); }
                return;
            }
            activations = 0;
            if (!rebindStarted && arguments.isSet("rebind-trigger")) {
                previous = current;
                rebindStarted = true;
                QTimer::singleShot(1000, &control, [&] {
                    begin(arguments.value("rebind-trigger"));
                    if (cancelRebindMs >= 0)
                        QTimer::singleShot(cancelRebindMs, &control, [&] { control.cancelRequest(request); });
                });
                return;
            }
            if (arguments.isSet("close-parent-after-bind")) window.reset();
            if (!arguments.isSet("shutdown-on-refresh")) activationDeadline.start(holdMs);
        });
    QObject::connect(&control, &PortalShortcuts::activated, &app,
        [&](const QString& id, quint64 timestamp, const QString& token) {
            ++activations;
            print({{"event", "activated"}, {"id", id}, {"timestamp", static_cast<qint64>(timestamp)},
                   {"hasActivationToken", !token.isEmpty()}});
        });
    QObject::connect(&control, &PortalShortcuts::deactivated, &app,
        [&](const QString& id, quint64 timestamp, const QString& token) {
            print({{"event", "deactivated"}, {"id", id}, {"timestamp", static_cast<qint64>(timestamp)},
                   {"hasActivationToken", !token.isEmpty()}});
        });
    QObject::connect(&control, &PortalShortcuts::bindingsChanged, &app,
        [&](const QVector<PortalShortcutBinding>& assigned) {
            print({{"event", "bindingsChanged"}, {"assigned", assignments(assigned)}});
            ++changes;
            // The first notification accompanies binding; a later notification
            // requires a real desktop ShortcutsChanged signal and ListShortcuts
            // reply. No fake Portal or injected signal supplies this case.
            if (arguments.isSet("shutdown-on-refresh") && changes > 1 && exitStatus < 0) {
                finish(0);
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                if (drained) { print({{"event", "shutdownBeforeRefreshReturned"}, {"passed", false}}); app.exit(1); }
            }
        });
    QObject::connect(&control, &PortalShortcuts::bindingsRefreshFailed, &app,
        [&](const PortalFailure& error) { print({{"event", "bindingsRefreshFailed"}, {"failure", describe(error)}}); finish(1); });
    QObject::connect(&control, &PortalShortcuts::bindingsClosed, &app,
        [&](const PortalFailure& error) {
            print({{"event", "bindingsClosed"}, {"failure", describe(error)}});
            if (error) finish(1);
        });
    QObject::connect(&activationDeadline, &QTimer::timeout, &app, [&] {
        const bool passed = !pending && activations > 0 && control.hasActiveBindings();
        print({{"event", "activationObservation"}, {"passed", passed}, {"activations", activations}, {"bindingResults", completedBindings}});
        finish(passed ? 0 : 1);
    });
    QTimer::singleShot(0, &control, &PortalShortcuts::initialize);
    QTimer::singleShot(120000, &app, [&] {
        control.cancelRequest(request);
        print({{"event", "probeDeadline"}, {"passed", false}});
        finish(1);
    });
    return app.exec();
}
