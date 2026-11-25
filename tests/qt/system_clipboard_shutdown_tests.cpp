#include "services/desktop/SystemClipboard.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFuture>
#include <QImage>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace desktop = HuhuCapture::Desktop;
namespace media = HuhuCapture::Media;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void events()
{
    QCoreApplication::sendPostedEvents();
    QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void waitFor(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 8000) { events(); QThread::msleep(1); }
    require(predicate(), message);
}

// Hold a real QtConcurrent task on the one available global-pool thread. The
// production identity check then remains an actual queued future, retaining a
// real archive lease. No file operation, clipboard result or service is mocked.
class PoolBarrier final {
public:
    PoolBarrier() : previous_(QThreadPool::globalInstance()->maxThreadCount()), gate_(std::make_shared<Gate>())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        task_ = QtConcurrent::run([gate = gate_] {
            gate->entered.release();
            gate->timedOut = !gate->released.tryAcquire(1, 10000);
        });
        waitFor([&] { return gate_->entered.available() == 1; }, "The actual Qt worker reaches its scheduling barrier");
    }
    ~PoolBarrier()
    {
        release(); task_.waitForFinished();
        QThreadPool::globalInstance()->setMaxThreadCount(previous_);
    }
    void release()
    { if (!released_) { released_ = true; gate_->released.release(); } }
    bool timedOut() const { return gate_->timedOut.load(); }
private:
    struct Gate { QSemaphore entered, released; std::atomic_bool timedOut{}; };
    int previous_;
    std::shared_ptr<Gate> gate_;
    QFuture<void> task_;
    bool released_{};
};

struct Fixture {
    QTemporaryDir directory;
    media::MediaArchiveService archive;
    media::Entry entry;
    Fixture() : archive(QDir(directory.path()).filePath(QStringLiteral("历史 archive")))
    {
        require(directory.isValid(), "Create a real isolated media directory");
        QImage image(24, 13, QImage::Format_ARGB32); image.fill(QColor(20, 40, 80, 170));
        const auto result = archive.archiveImage(image);
        require(result.durable(), "Create a fully encoded, durable actual PNG archive");
        const auto listed = archive.list();
        require(!listed.failure && listed.entries.size() == 1, "List exactly the real completed PNG");
        entry = listed.entries.front();
    }
    std::shared_ptr<desktop::PreparedClipboard> prepare()
    {
        auto lease = archive.lease(entry); require(!lease.failure && bool(lease.lease), "Acquire the actual history read reservation");
        auto prepared = desktop::prepareFile(std::move(lease.lease));
        require(!prepared.failure && bool(prepared.content), "Decode and encode actual media for clipboard publication");
        return std::move(prepared.content);
    }
    bool removalAvailable()
    {
        const auto reservation = archive.reserveRemoval(entry);
        return !reservation.failure && bool(reservation.lease);
    }
};

struct Receipts {
    std::map<quint64, desktop::ClipboardResult> results;
    std::map<quint64, int> counts;
    explicit Receipts(desktop::SystemClipboard& service)
    {
        QObject::connect(&service, &desktop::SystemClipboard::finished, &service,
            [this](quint64 token, const desktop::ClipboardResult& result) {
                ++counts[token]; results.emplace(token, result);
            });
    }
    void wait(quint64 token)
    { waitFor([&] { return results.contains(token); }, "Each admitted request produces its actual queued completion"); }
    void exactlyOnce() const
    { for (const auto& [token, count] : counts) require(token && count == 1, "Every nonzero request has exactly one terminal receipt"); }
};

void cancelledWorkerDrain()
{
    Fixture fixture;
    desktop::SystemClipboard service;
    Receipts receipts(service);
    std::vector<bool> busy;
    int shutdowns{};
    QObject::connect(&service, &desktop::SystemClipboard::busyChanged, &service, [&](bool value) { busy.push_back(value); });
    QObject::connect(&service, &desktop::SystemClipboard::shutdownFinished, &service, [&] { ++shutdowns; });
    auto prepared = fixture.prepare();
    PoolBarrier barrier;
    const auto token = service.publish(prepared, nullptr);
    require(token && service.busy(), "Publication owns a real queued identity validation");
    service.cancel(token); receipts.wait(token);
    require(receipts.results.at(token).failure.code == desktop::Error::Cancelled, "Cancellation reports its real terminal status");
    require(service.busy() && !fixture.removalAvailable(), "A cancellation receipt does not hide the unfinished worker or its media lease");
    require(busy == std::vector<bool>{true}, "No idle notification precedes actual worker completion");

    std::vector<quint64> rejected;
    for (int index = 0; index < 8; ++index) rejected.push_back(service.publish(fixture.prepare(), nullptr));
    for (const auto request : rejected) {
        receipts.wait(request);
        require(receipts.results.at(request).failure.code == desktop::Error::Busy, "Cancelled but undrained work prevents replacement validation tasks");
    }
    service.shutdown(); service.shutdown(); events();
    require(!shutdowns && service.busy(), "Shutdown remains pending while the actual worker cannot run");
    auto late = fixture.prepare();
    require(service.publish(late, nullptr) == 0, "A stopped service rejects new publication without an admitted token");
    require(!fixture.removalAvailable(), "Stopping still protects the original worker's real file identity");

    barrier.release();
    waitFor([&] { return shutdowns == 1; }, "Shutdown waits for the actual validation future and all receipts to complete");
    require(!barrier.timedOut(), "The test explicitly released the real scheduling barrier");
    require(!service.busy() && fixture.removalAvailable(), "Finished validation releases every read reservation, including retained preparations");
    require(QFileInfo::exists(fixture.entry.path), "Cancellation and shutdown never delete the completed user media");
    events(); receipts.exactlyOnce();
    require(receipts.results.size() == 9 && shutdowns == 1 && busy == std::vector<bool>({true, false}),
        "Only admitted tokens complete, shutdown is idempotent, and actual busy transitions are complete");
}

void receiptAndReentrancyDrain()
{
    desktop::SystemClipboard service;
    int receipts{}, shutdowns{};
    bool insideReceipt{};
    QObject::connect(&service, &desktop::SystemClipboard::shutdownFinished, &service, [&] {
        require(!insideReceipt, "Shutdown cannot complete inside an admitted receipt's listener"); ++shutdowns;
    });
    QObject::connect(&service, &desktop::SystemClipboard::finished, &service,
        [&](quint64 token, const desktop::ClipboardResult& result) {
            require(token && result.failure.code == desktop::Error::InvalidArgument, "An actual invalid request has its typed queued receipt");
            ++receipts; insideReceipt = true;
            service.shutdown(); events();
            require(!shutdowns, "A reentrant GUI event loop cannot outrun the current finished listener");
            insideReceipt = false;
        });
    require(service.publish({}, nullptr), "Invalid content still receives an admitted terminal token");
    waitFor([&] { return shutdowns == 1; }, "The receipt returns before shutdown's final signal");
    events(); require(receipts == 1 && shutdowns == 1 && !service.busy(), "Reentrant shutdown remains exactly once");
}

void reusableAfterCancelledWorker()
{
    Fixture fixture;
    desktop::SystemClipboard service;
    Receipts receipts(service);
    PoolBarrier barrier;
    const auto first = service.publish(fixture.prepare(), nullptr);
    service.cancel(first); receipts.wait(first);
    require(service.busy(), "Cancelled validation remains busy until its actual scheduling barrier is released");
    barrier.release();
    waitFor([&] { return !service.busy(); }, "A cancelled worker eventually returns the live service to idle");
    require(fixture.removalAvailable(), "Actual idle releases the first history reservation");
    const auto next = service.publish(fixture.prepare(), nullptr);
    service.cancel(next); receipts.wait(next);
    require(receipts.results.at(next).failure.code == desktop::Error::Cancelled,
        "The live service admits a fresh actual validation after the previous worker completes");
    int shutdowns{};
    QObject::connect(&service, &desktop::SystemClipboard::shutdownFinished, &service, [&] { ++shutdowns; });
    service.shutdown(); waitFor([&] { return shutdowns == 1; }, "The reused service drains its final actual worker");
    require(fixture.removalAvailable(), "The reused service retains no hidden media reservation");
    receipts.exactlyOnce();
}

void queuedWrongThreadDrain()
{
    Fixture fixture;
    desktop::SystemClipboard service;
    Receipts receipts(service);
    int shutdowns{};
    QObject::connect(&service, &desktop::SystemClipboard::shutdownFinished, &service, [&] { ++shutdowns; });
    auto prepared = fixture.prepare();
    auto submitting = std::async(std::launch::async, [&] { return service.publish(prepared, nullptr); });
    const auto token = submitting.get();
    require(token && receipts.results.empty() && !fixture.removalAvailable(), "A real non-GUI caller queues its admitted request and retains the preparation");
    service.shutdown();
    require(!shutdowns, "Shutdown accounts for admission before the queued GUI dispatch");
    waitFor([&] { return shutdowns == 1; }, "Queued wrong-thread completion drains before shutdown");
    require(receipts.results.at(token).failure.code == desktop::Error::WrongThread && fixture.removalAvailable(),
        "The actual wrong-thread path consumes its preparation and reports the correct failure");
    auto afterStop = fixture.prepare();
    auto rejected = std::async(std::launch::async, [&] { return service.publish(afterStop, nullptr); });
    require(rejected.get() == 0 && fixture.removalAvailable(), "Post-shutdown cross-thread rejection releases its own unused preparation");
    events(); receipts.exactlyOnce(); require(receipts.results.size() == 1 && shutdowns == 1, "Rejected zero tokens never invent extra receipts");
}
}

int main(int argc, char** argv)
{
    try {
        QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
        cancelledWorkerDrain(); reusableAfterCancelledWorker(); receiptAndReentrancyDrain(); queuedWrongThreadDrain();
        std::cout << "Actual Qt worker/receipt shutdown, bounded cancelled validation and real media lease release passed.\n"
                     "This lifecycle test does not claim a desktop clipboard receiver or process-exit persistence.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "system_clipboard_shutdown_tests: " << failure.what() << '\n'; return 1;
    }
}
