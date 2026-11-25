#include "services/desktop/DesktopFiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <stdexcept>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Media::Entry saved(Media::MediaArchiveService& archive)
{
    QImage image(32, 24, QImage::Format_ARGB32); image.fill(qRgba(33, 88, 156, 192));
    const auto result = archive.archiveImage(image); require(result.durable(), "create an actual completed PNG archive");
    const auto list = archive.list(); require(!list.failure, "list the real isolated media archive");
    for (const auto& entry : list.entries) if (entry.path == result.path) return entry;
    throw std::runtime_error("the completed media is missing from its actual archive");
}
template<class Function> auto worker(Function function)
{ return std::async(std::launch::async, std::move(function)).get(); }

void recycleContracts(const QString& root)
{
    Media::MediaArchiveService archive(root + QStringLiteral("/archive"));
    auto entry = saved(archive);
    auto reader = archive.lease(entry); require(!reader.failure && reader.lease, "real read lease");
    auto result = worker([&] { return Desktop::recycle(archive, entry); });
    require(result.failure.code == Desktop::Error::Busy && !result.permanentPermit && QFile::exists(entry.path),
        "a busy original is kept and cannot produce a permanent-delete permit");
    reader.lease.reset();
    std::stop_source stop; stop.request_stop();
    result = worker([&] { return Desktop::recycle(archive, entry, stop.get_token()); });
    require(result.failure.code == Desktop::Error::Cancelled && QFile::exists(entry.path), "cancelled recycle keeps the actual file");
    result = worker([&] { return Desktop::recycle(archive, entry); });
    require(result.completed() && result.effect == Desktop::RemovalEffect::Recycled && !QFile::exists(entry.path),
        "the real platform recycle operation removes the exact confirmed original");
#ifndef Q_OS_WIN
    require(!result.recycledPath.isEmpty() && QFile::exists(result.recycledPath), "the real XDG trash retains a recoverable file");
    require(!QImage(result.recycledPath).isNull(), "the actual recycled PNG remains decodable");
#endif
    entry = saved(archive);
    { QFile modified(entry.path); require(modified.open(QIODevice::Append), "open the real archive for an external change");
      require(modified.write("x", 1) == 1 && modified.flush(), "make a real on-disk snapshot mismatch"); }
    result = worker([&] { return Desktop::recycle(archive, entry); });
    require(result.failure.code == Desktop::Error::ChangedOnDisk && !result.permanentPermit && QFile::exists(entry.path),
        "a stale confirmation cannot recycle a changed original");
    require(!Desktop::permanentlyRemove({}).completed(), "no permit means no permanent deletion");
    std::cout << "PASS real recycle, busy lease, cancellation and changed-file protection\n";
}

#ifdef Q_OS_WIN
void failedTrashContracts(const QString& root)
{
    Media::MediaArchiveService archive(root + QStringLiteral("/failed-recycle-archive"));
    const auto entry = saved(archive);
    const auto path = entry.path.toStdWString();
    const auto locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "hold an actual Windows file handle that denies deletion");
    const auto result = worker([&] { return Desktop::recycle(archive, entry); });
    CloseHandle(locked);
    require(result.failure.code == Desktop::Error::RecycleFailed && result.permanentPermit && QFile::exists(entry.path),
        "a real sharing violation must preserve the file and require a distinct permanent confirmation");
    require(archive.lease(entry).failure.code == Media::Error::Busy, "failed recycle permit keeps the exact exclusive reservation");
    std::stop_source stop; stop.request_stop();
    require(worker([&] { return Desktop::permanentlyRemove(result.permanentPermit, stop.get_token()); }).failure.code == Desktop::Error::Cancelled,
        "cancelling permanent removal does not consume its file");
    const auto removed = worker([&] { return Desktop::permanentlyRemove(result.permanentPermit); });
    require(removed.completed() && !QFile::exists(entry.path), "separately confirmed deletion works after the external lock is released");
    require(!worker([&] { return Desktop::permanentlyRemove(result.permanentPermit); }).completed(), "the same confirmation permit cannot be replayed");
    std::cout << "PASS actual Windows sharing violation and separate permanent-confirmation permit\n";
}
#else
void failedTrashContracts(const QString& root)
{
    const auto originalData = qgetenv("XDG_DATA_HOME");
    const auto blocked = root + QStringLiteral("/blocked-data");
    require(QDir().mkpath(blocked), "create isolated invalid trash parent");
    { QFile file(blocked + QStringLiteral("/Trash")); require(file.open(QIODevice::WriteOnly), "create a real nondirectory Trash obstruction");
      require(file.write("not a directory") > 0, "write the real trash obstruction"); }
    qputenv("XDG_DATA_HOME", QFile::encodeName(blocked));
    Media::MediaArchiveService archive(root + QStringLiteral("/failed-recycle-archive"));
    auto entry = saved(archive);
    auto result = worker([&] { return Desktop::recycle(archive, entry); });
    require(result.failure.code == Desktop::Error::RecycleFailed && result.permanentPermit && QFile::exists(entry.path),
        "a real unavailable trash keeps the file and returns only a separate confirmation permit");
    require(archive.lease(entry).failure.code == Media::Error::Busy, "the pending permanent question retains its exclusive reservation");
    std::stop_source stop; stop.request_stop();
    auto cancelled = worker([&] { return Desktop::permanentlyRemove(result.permanentPermit, stop.get_token()); });
    require(cancelled.failure.code == Desktop::Error::Cancelled && QFile::exists(entry.path), "cancelled permanent deletion keeps the file");
    const auto removed = worker([&] { return Desktop::permanentlyRemove(result.permanentPermit); });
    require(removed.completed() && removed.effect == Desktop::RemovalEffect::PermanentlyRemoved && !QFile::exists(entry.path),
        "a separately confirmed permit removes only its exact original");
    require(!worker([&] { return Desktop::permanentlyRemove(result.permanentPermit); }).completed(), "a consumed permit cannot be replayed");
    result = {}; entry = saved(archive);
    result = worker([&] { return Desktop::recycle(archive, entry); });
    require(result.permanentPermit != nullptr, "second real recycle failure obtains its own permit");
    { QFile changed(entry.path); require(changed.open(QIODevice::Append), "open after the second confirmation was prepared");
      require(changed.write("changed") > 0 && changed.flush(), "change actual bytes after preparing the permit"); }
    const auto rejected = worker([&] { return Desktop::permanentlyRemove(result.permanentPermit); });
    require(rejected.failure.code == Desktop::Error::ChangedOnDisk && QFile::exists(entry.path),
        "the second confirmation does not bypass the immediate identity recheck");
    qputenv("XDG_DATA_HOME", originalData);
    std::cout << "PASS real failed-trash path, explicit permanent permit, cancellation and changed-after-confirmation\n";
}

void failedOriginalUnlink(const QString& root)
{
    require(geteuid() != 0, "the original-unlink permission check needs an ordinary user, not a privileged runner");
    const auto directory = root + QStringLiteral("/readonly-archive");
    Media::MediaArchiveService archive(directory); const auto entry = saved(archive);
    const auto originalPermissions = QFile::permissions(directory);
    require(QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner), "remove directory write permission for a real unlink error");
    const auto result = worker([&] { return Desktop::recycle(archive, entry); });
    const bool retained = QFile::exists(entry.path);
    require(QFile::setPermissions(directory, originalPermissions), "restore the test's own directory permissions");
    require(!result.completed() && retained, "Qt's successful trash link must not hide a failed original unlink");
    std::cout << "PASS actual original-unlink failure is not reported as recycled\n";
}
#endif
}

int main(int argc, char** argv)
{
    QTemporaryDir isolated; if (!isolated.isValid()) return 2;
#ifndef Q_OS_WIN
    const auto data = isolated.path() + QStringLiteral("/xdg-data");
    if (!QDir().mkpath(data)) return 2;
    qputenv("XDG_DATA_HOME", QFile::encodeName(data));
#endif
    QCoreApplication app(argc, argv);
    try {
        recycleContracts(isolated.path());
        failedTrashContracts(isolated.path());
#ifndef Q_OS_WIN
        failedOriginalUnlink(isolated.path());
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
