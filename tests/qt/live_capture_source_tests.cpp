#include "services/capture/LiveCaptureSource.h"
#ifdef Q_OS_LINUX
#include "platform/linux/portal/WaylandRequestBarrier.h"
#endif

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QWindow>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture::Capture;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 8000) {
        QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(); QThread::msleep(1);
    }
    require(condition(), "the actual unavailable source drains all queued notifications");
}
void noNativeBackend()
{
    const auto qpa = QGuiApplication::platformName();
    require(qpa == QStringLiteral("offscreen") || qpa == QStringLiteral("minimal"),
        "this negative test requires an actual non-desktop Qt backend");
    LiveCaptureSource source;
    QWindow parent; parent.resize(240, 180); parent.show();
    bool initialized = false, stopped = false, shutdown = false;
    std::vector<QString> order;
    unsigned preparations{};
    QObject::connect(&source, &LiveCaptureSource::initialized, &source, [&](const Capability& capability, const Failure& failure) {
        require(capability.state == CapabilityState::Unavailable && failure.code == Failure::Code::Unavailable,
            "the actual unsupported QPA is not advertised as live capture");
        initialized = true;
    });
    QObject::connect(&source, &LiveCaptureSource::prepared, &source, [&](quint64 token, const Snapshot& frame, const Failure& failure) {
        require(token && !frame.sourceId && frame.image.isNull() && failure.code == Failure::Code::Unavailable,
            "unavailable authorization produces no fabricated stream pixels");
        ++preparations; order.push_back(QStringLiteral("prepared"));
    });
    QObject::connect(&source, &LiveCaptureSource::stopped, &source, [&](quint64 token, const Failure&) {
        require(token != 0, "a stopped admission retains its exact request identity");
        stopped = true; order.push_back(QStringLiteral("stopped"));
    });
    QObject::connect(&source, &LiveCaptureSource::shutdownFinished, &source, [&] {
        shutdown = true; order.push_back(QStringLiteral("shutdown"));
    });
    source.initialize();
    waitFor([&] { return initialized; });
    const auto token = source.request(&parent);
    require(token != 0, "a valid live-source request receives one asynchronous rejection");
    waitFor([&] { return stopped; });
    require(preparations == 1 && !source.claim(1, 1, {0, 0, 1, 1}) && !source.isCurrent(1, 1),
        "unavailable capture cannot expose a queue or a valid source identity");
    source.shutdown();
    require(!shutdown && source.request(&parent) == 0, "shutdown closes admission before its queued drain notification");
    waitFor([&] { return shutdown; });
    require(order == std::vector<QString>({QStringLiteral("prepared"), QStringLiteral("stopped"), QStringLiteral("shutdown")}),
        "preparation and stopped notifications precede the final drain");
    std::cout << "PASS real unavailable QPA, no stream fabrication, closed admission and asynchronous drain\n";
}
void reentrantShutdownAndDeletion()
{
    auto* source = new LiveCaptureSource;
    QPointer<LiveCaptureSource> alive(source);
    QWindow parent; parent.show();
    bool shutdown{};
    QObject::connect(source, &LiveCaptureSource::prepared, source, [source](quint64, const Snapshot&, const Failure&) { source->shutdown(); });
    QObject::connect(source, &LiveCaptureSource::shutdownFinished, source, [&] { shutdown = true; delete source; });
    const auto token = source->request(&parent);
    require(token != 0, "a reentrant owner observes an asynchronous source admission");
    waitFor([&] { return shutdown; });
    require(!alive, "the real shutdown signal may destroy its source owner");

    source = new LiveCaptureSource; alive = source;
    QObject::connect(source, &LiveCaptureSource::prepared, source, [source](quint64, const Snapshot&, const Failure&) { delete source; });
    require(source->request(nullptr) != 0, "invalid parent rejection is also delivered asynchronously");
    waitFor([&] { return !alive; });
    std::cout << "PASS reentrant shutdown and source deletion from real terminal signals\n";
}
void refreshAdmission()
{
    LiveCaptureSource source;
    quint64 expected{};
    unsigned completions{};
    bool drained{};
    QObject::connect(&source, &LiveCaptureSource::captureReady, &source,
        [&](quint64 request, quint64 id, quint64 generation, const Failure& failure) {
            require(request == expected && id == 73 && generation == 5 && ++completions == 1,
                "refresh preserves the requested source and has exactly one terminal");
            require(failure.code == Failure::Code::SourceChanged && !source.claim(id, generation, {0, 0, 1, 1}),
                "refresh cannot fabricate authorization or a replacement frame");
            source.cancelRefresh(request); // a completed rejection is harmless
            source.shutdown();
        });
    QObject::connect(&source, &LiveCaptureSource::shutdownFinished, &source, [&] { drained = true; });
    expected = source.refreshForCapture(73, 5);
    require(expected != 0 && !completions, "invalid refresh admission still reports asynchronously");
    waitFor([&] { return drained; });
    require(source.refreshForCapture(73, 5) == 0 && completions == 1,
        "shutdown closes refresh admission before reporting its drain");
    std::cout << "PASS actual unauthorized refresh rejection, no stale claim, terminal reentry and closed admission\n";
}
#ifdef Q_OS_LINUX
void requestBarrierWithoutWayland()
{
    using HuhuCapture::Platform::Linux::WaylandRequestBarrier;
    using HuhuCapture::Platform::Linux::PortalFailure;
    for (const bool cancel : {false, true}) {
        auto* barrier = new WaylandRequestBarrier;
        QPointer<WaylandRequestBarrier> alive(barrier);
        unsigned terminals{};
        QObject::connect(barrier, &WaylandRequestBarrier::finished, barrier, [&](const PortalFailure& failure) {
            require(++terminals == 1 && failure.code == (cancel ? PortalFailure::Code::Cancelled : PortalFailure::Code::Unsupported),
                "The actual unsupported Qt display or prior cancellation has one truthful asynchronous terminal");
            delete barrier;
        });
        if (cancel) barrier->cancel();
        barrier->start();
        require(terminals == 0 && alive, "No Wayland completion runs inside start/cancel");
        waitFor([&] { return !alive; });
        require(terminals == 1, "The terminal may destroy its request barrier without a late callback");
    }
    std::cout << "PASS actual non-Wayland request ordering failure, prior cancellation and terminal destruction\n";
}
#endif
}
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        noNativeBackend(); reentrantShutdownAndDeletion(); refreshAdmission();
#ifdef Q_OS_LINUX
        requestBarrierWithoutWayland();
#endif
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
