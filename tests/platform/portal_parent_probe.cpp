#include "platform/linux/portal/PortalParent.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <wayland-client.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <memory>

using namespace HuhuCapture::Platform::Linux;

namespace {

void print(QJsonObject object)
{
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

class Probe final : public QObject {
public:
    enum class Stage { Warmup, WarmupSync, Waiting, Running, Result, Drain };
    QString mode;
    std::unique_ptr<QWidget> window{std::make_unique<QWidget>()};
    std::shared_ptr<PortalParent> parent;
    std::weak_ptr<PortalParent> released;
    GCancellable* cancellable{g_cancellable_new()};
    QSocketNotifier input{STDIN_FILENO, QSocketNotifier::Read};
    QByteArray inputBytes;
    QTimer heartbeat;
    QElapsedTimer elapsed;
    wl_callback* sync{};
    Stage stage{Stage::Warmup};
    int beats{};
    int completions{};
    bool passed{};

    explicit Probe(QString value) : mode(std::move(value)) {
        window->resize(480, 180);
        window->setWindowTitle(QStringLiteral("huhucapture asynchronous Wayland parent validation"));
        window->show();
        heartbeat.setInterval(10);
        QObject::connect(&heartbeat, &QTimer::timeout, this, [this] { ++beats; });
        const auto flags = fcntl(STDIN_FILENO, F_GETFL);
        if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
            qFatal("Cannot make test coordinator input nonblocking");
        QObject::connect(&input, &QSocketNotifier::activated, this, [this] { readCommands(); });
    }
    ~Probe() override {
        if (sync) wl_callback_destroy(sync);
        parent.reset();
        g_clear_object(&cancellable);
    }

    void beginParent() {
        parent = PortalParent::create();
        QObject::connect(parent.get(), &PortalParent::prepared, this, [this](const PortalFailure& error) {
            if (stage == Stage::Warmup) {
                if (error || !parent->nativeParent()) {
                    print({{"event", "warmupFailed"}, {"code", static_cast<int>(error.code)}, {"message", error.message}});
                    qApp->exit(1);
                    return;
                }
                if (mode != QStringLiteral("export") && mode != QStringLiteral("invalidate")) {
                    released = parent;
                    parent.reset();
                }
                stage = Stage::WarmupSync;
                requestSync();
                return;
            }
            if (stage != Stage::Running) { result(false, "Unexpected parent completion"); return; }
            ++completions;
            bool expected = false;
            if (mode == QStringLiteral("cancel")) expected = error.code == PortalFailure::Code::Cancelled;
            if (mode == QStringLiteral("destroy")) expected = error.code == PortalFailure::Code::ParentDestroyed;
            if (mode == QStringLiteral("timeout"))
                expected = error.code == PortalFailure::Code::Unavailable
                    && error.message.contains(QStringLiteral("timed out")) && elapsed.elapsed() >= 4900;
            result(expected && completions == 1 && beats >= 2 && !parent->nativeParent(), error.message);
        });
        QObject::connect(parent.get(), &PortalParent::invalidated, this, [this](const PortalFailure& error) {
            const bool expected = stage == Stage::Running && mode == QStringLiteral("invalidate")
                && error.code == PortalFailure::Code::ParentDestroyed && !parent->nativeParent() && beats >= 2;
            result(expected, error.message);
        });
        parent->prepare(window->windowHandle(), cancellable);
    }

    void startCase() {
        if (stage != Stage::Waiting) { result(false, "Coordinator started the probe twice"); return; }
        stage = Stage::Running;
        elapsed.start();
        beats = 0;
        heartbeat.start();
        if (mode == QStringLiteral("export")) { result(parent && parent->nativeParent(), {}); return; }
        if (mode == QStringLiteral("invalidate")) {
            QTimer::singleShot(200, this, [this] { window.reset(); });
            return;
        }
        if (!released.expired()) { result(false, "Warmup export retained its owner"); return; }
        beginParent();
        if (mode == QStringLiteral("cancel"))
            QTimer::singleShot(200, this, [this] { g_cancellable_cancel(cancellable); });
        if (mode == QStringLiteral("destroy"))
            QTimer::singleShot(200, this, [this] { window.reset(); });
        if (mode == QStringLiteral("destroy-owner"))
            QTimer::singleShot(200, this, [this] {
                released = parent;
                parent.reset();
                result(released.expired() && beats >= 2 && completions == 0, {});
            });
    }

    void result(bool success, QString message) {
        if (stage == Stage::Result || stage == Stage::Drain) return;
        stage = Stage::Result;
        heartbeat.stop();
        passed = success;
        print({{"event", "result"}, {"mode", mode}, {"passed", passed}, {"heartbeats", beats},
               {"elapsedMs", elapsed.isValid() ? elapsed.elapsed() : 0}, {"message", message}});
        // The external coordinator resumes its own compositor before cleanup.
        // This probe accepts no PID and never signals another process.
    }

    void requestSync() {
        auto* native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!native || sync) { qApp->exit(1); return; }
        sync = wl_display_sync(native->display());
        static const wl_callback_listener listener{+[](void* data, wl_callback* callback, uint32_t) {
            if (QThread::currentThread() != qApp->thread()) qFatal("Wayland sync was not dispatched on the Qt thread");
            auto* test = static_cast<Probe*>(data);
            wl_callback_destroy(callback);
            test->sync = nullptr;
            if (test->stage == Stage::WarmupSync) {
                test->stage = Stage::Waiting;
                print({{"event", "ready"}, {"mode", test->mode}});
            } else if (test->stage == Stage::Drain) {
                const bool released = test->released.expired();
                print({{"event", "drained"}, {"ownerReleased", released}, {"passed", test->passed && released}});
                qApp->exit(test->passed && released ? 0 : 1);
            }
        }};
        if (!sync || wl_callback_add_listener(sync, &listener, this) < 0) qApp->exit(1);
        // Async sync is a real server barrier, not a blocking roundtrip or a
        // sleep used to assume that late native events have been handled.
    }

    void readCommands() {
        char bytes[256];
        for (;;) {
            const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count > 0) { inputBytes.append(bytes, static_cast<qsizetype>(count)); continue; }
            if (count == 0) { input.setEnabled(false); qApp->exit(1); return; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) qApp->exit(1);
            break;
        }
        qsizetype end;
        while ((end = inputBytes.indexOf('\n')) >= 0) {
            const auto command = inputBytes.left(end);
            inputBytes.remove(0, end + 1);
            if (command == "start") startCase();
            else if (command == "resume" && stage == Stage::Result) {
                if (parent) released = parent;
                parent.reset();
                window.reset();
                stage = Stage::Drain;
                requestSync();
            } else { qApp->exit(1); return; }
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser args;
    args.addHelpOption();
    args.addOption({"mode", "Real Wayland case, coordinated by test-wayland-parent.py", "mode", "export"});
    args.process(app);
    const auto mode = args.value("mode");
    if (!QStringList{"export", "cancel", "destroy", "timeout", "destroy-owner", "invalidate"}.contains(mode)) return 2;
    if (!qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) return 2;
    Probe probe(mode);
    QTimer::singleShot(0, &probe, [&] { probe.beginParent(); });
    QTimer::singleShot(20000, &app, [&] { print({{"event", "probeDeadline"}, {"passed", false}}); app.exit(1); });
    return app.exec();
}
