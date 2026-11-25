#include "services/capture/LiveCaptureSource.h"

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
}
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try { noNativeBackend(); reentrantShutdownAndDeletion(); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
