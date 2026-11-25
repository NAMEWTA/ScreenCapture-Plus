#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QApplication::processEvents(); QThread::msleep(1);
    }
    require(condition(), "Actual asynchronous desktop owners finish shutdown");
}
template<class Work> auto worker(Work work)
{ return std::async(std::launch::async, std::move(work)).get(); }
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir directory; require(directory.isValid(), "Create owned desktop shutdown fixture");
        QWidget origin; origin.resize(240, 120); origin.show(); QApplication::processEvents();
        for (int iteration = 0; iteration < 16; ++iteration) {
            Media::MediaArchiveService archive(directory.path() + QStringLiteral("/archive-%1").arg(iteration));
            QImage pixels(64, 48, QImage::Format_ARGB32); pixels.fill(qRgba(20, 40, 60, 128));
            require(worker([&] { return archive.archiveImage(pixels); }).durable(), "Archive real pixels before shutdown race");
            const auto entries = worker([&] { return archive.list(); });
            require(!entries.failure && entries.entries.size() == 1, "Enumerate exact archived media");
            auto lease = worker([&] { return archive.lease(entries.entries.front()); });
            require(!lease.failure, "Acquire actual file reservation");
            auto prepared = worker([lease = std::move(lease.lease)]() mutable {
                return Desktop::prepareFile(std::move(lease));
            });
            require(!prepared.failure, "Prepare real image clipboard bytes");
            Desktop::SystemClipboard clipboard;
            unsigned completions{}, shutdowns{};
            QObject::connect(&clipboard, &Desktop::SystemClipboard::finished, &clipboard,
                [&](quint64, const Desktop::ClipboardResult&) { ++completions; });
            QObject::connect(&clipboard, &Desktop::SystemClipboard::shutdownFinished, &clipboard, [&] {
                require(completions == 1, "Clipboard shutdown follows its actual terminal receipt");
                require(!clipboard.busy(), "No actual validation worker remains at clipboard shutdown");
                ++shutdowns;
            });
            const auto token = clipboard.publish(std::move(prepared.content), origin.windowHandle());
            require(token, "Admit actual clipboard work");
            clipboard.shutdown(); clipboard.shutdown();
            waitFor([&] { return shutdowns == 1; });
            require(!clipboard.publish({}, origin.windowHandle()), "A stopped clipboard owner rejects new work");
            require(!worker([&] { return archive.reserveRemoval(entries.entries.front()); }).failure,
                "Shutdown releases the final actual media reservation");
        }
        Desktop::FileRevealer revealer;
        unsigned completed{}, stopped{};
        QObject::connect(&revealer, &Desktop::FileRevealer::finished, &revealer,
            [&](quint64, const Desktop::RevealResult& value) {
                require(!value.completed(), "A nonexistent directory never opens a file manager"); ++completed;
            });
        QObject::connect(&revealer, &Desktop::FileRevealer::shutdownFinished, &revealer, [&] {
            require(completed == 1 && !revealer.busy(), "File-manager shutdown follows real worker completion"); ++stopped;
        });
        require(revealer.openDirectory(directory.path() + QStringLiteral("/does-not-exist")), "Admit real directory validation");
        revealer.shutdown(); revealer.shutdown(); waitFor([&] { return stopped == 1; });
        require(!revealer.openDirectory(directory.path()), "A stopped file manager never launches another request");
        std::cout << "PASS actual clipboard/media reservations and file-manager workers drain before shutdown\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
