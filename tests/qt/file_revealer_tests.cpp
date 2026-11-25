#include "services/desktop/FileRevealer.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void diagnose(const char* operation, const Desktop::RevealResult& result)
{
    std::cout << "RESULT " << operation << " error=" << static_cast<int>(result.failure.code)
        << " native=" << result.failure.nativeCode << " (0x" << std::hex
        << static_cast<std::uint64_t>(result.failure.nativeCode) << std::dec << ") detail="
        << result.failure.detail.toStdString() << " effect=" << static_cast<int>(result.effect)
        << " path=" << result.path.toStdString() << " exists=" << QFile::exists(result.path) << '\n';
}
void events() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
template<class Function> auto worker(Function function) { return std::async(std::launch::async, std::move(function)).get(); }
void waitFor(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 20000) { events(); QThread::msleep(2); }
    require(predicate(), message);
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir isolated; require(isolated.isValid(), "isolate real file-manager test files");
        QWidget origin; origin.resize(320, 180); origin.setWindowTitle(QStringLiteral("huhucapture file manager validation")); origin.show(); events();
        Desktop::FileRevealer revealer;
        std::map<quint64, Desktop::RevealResult> results;
        QObject::connect(&revealer, &Desktop::FileRevealer::finished, &revealer, [&](quint64 token, const Desktop::RevealResult& result) {
            require(!results.contains(token), "a file-manager request completes exactly once"); results.emplace(token, result);
        });
        const auto completed = [&](quint64 token) {
            waitFor([&] { return results.contains(token); }, "the actual file-manager request completes asynchronously"); return results.at(token);
        };
        require(completed(revealer.openDirectory(QStringLiteral("relative/path"), origin.windowHandle())).failure.code == Desktop::Error::InvalidArgument,
            "relative locations never reach a system launcher");
        require(completed(revealer.openDirectory(isolated.path() + QStringLiteral("/missing"), origin.windowHandle())).failure.code == Desktop::Error::InvalidArgument,
            "missing directory reports its real validation failure");
        require(!QFile::exists(isolated.path() + QStringLiteral("/missing")), "validation never creates an unsolicited directory");
        Media::MediaArchiveService archive(isolated.path() + QStringLiteral("/历史 archive"));
        QImage image(20, 16, QImage::Format_ARGB32); image.fill(qRgba(45, 98, 160, 170));
        const auto saved = worker([&] { return archive.archiveImage(image); }); require(saved.durable(), "write a real archived image");
        const auto listed = worker([&] { return archive.list(); }); require(!listed.failure && listed.entries.size() == 1, "enumerate the confirmed actual history entry");
        const auto entry = listed.entries.front();
        auto stale = worker([&] { return archive.lease(entry); }); require(!stale.failure, "acquire a real history read lease");
        { QFile modified(entry.path); require(modified.open(QIODevice::Append), "modify real media outside the application"); require(modified.write("changed") == 7 && modified.flush(), "persist changed bytes before reveal"); }
        require(completed(revealer.revealFile(std::move(stale.lease), origin.windowHandle())).failure.code == Desktop::Error::ChangedOnDisk,
            "a changed file is rejected before the file manager sees its path");
        std::cout << "PASS asynchronous file-manager preconditions and actual stale-file protection\n";
        if (!app.arguments().contains(QStringLiteral("--real-desktop"))) {
            std::cout << "Real Explorer/FileManager1/GIO launch was not requested by this validation mode\n"; return 0;
        }
        // This opt-in mode calls the actual desktop file manager. No D-Bus
        // service, shell launch, response, or URI is simulated by the test.
        const auto directory = isolated.path() + QStringLiteral("/new archive folder");
        const auto opened = completed(revealer.openDirectory(directory, origin.windowHandle(), true));
        diagnose("open actual directory", opened);
        require(opened.completed() && opened.effect == Desktop::RevealEffect::OpenedDirectory && QDir(directory).exists(),
            "the real desktop accepts opening the explicitly requested created archive directory");
        const auto second = worker([&] { return archive.archiveImage(image); }); require(second.durable(), "write real media for native file selection");
        const auto current = worker([&] { return archive.list(); }); require(!current.failure, "read current actual history");
        Media::Entry selected;
        for (const auto& item : current.entries) if (item.path == second.path) selected = item;
        require(!selected.path.isEmpty(), "select the exact newly archived file");
        auto lease = worker([&] { return archive.lease(selected); }); require(!lease.failure, "reserve the selected original");
        const auto revealed = completed(revealer.revealFile(std::move(lease.lease), origin.windowHandle()));
        diagnose("select actual history file", revealed);
        require(revealed.completed() && (revealed.effect == Desktop::RevealEffect::SelectedFile || revealed.effect == Desktop::RevealEffect::OpenedParent),
            "the real desktop accepts file selection or reports the actual parent-folder fallback");
#ifdef Q_OS_WIN
        require(revealed.effect == Desktop::RevealEffect::SelectedFile, "Windows uses its preserved SHOpenFolderAndSelectItems path");
#endif
        require(!worker([&] { return archive.reserveRemoval(selected); }).failure, "native completion releases the history read reservation");
        std::cout << "PASS real desktop file-manager launch, effect=" << static_cast<int>(revealed.effect) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
