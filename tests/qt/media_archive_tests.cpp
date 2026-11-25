#include "services/media/MediaArchiveService.h"
#include "services/files/QtPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef HUHUCAPTURE_TEST_WRAP_FSYNC
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
static std::atomic<bool> failDirectorySync{};
extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(int descriptor)
{
    struct stat info{};
    if (::fstat(descriptor, &info) == 0 && S_ISDIR(info.st_mode) && failDirectorySync.exchange(false)) {
        errno = EIO; return -1;
    }
    return __real_fsync(descriptor);
}
#endif

namespace media = HuhuCapture::Media;
namespace files = HuhuCapture::Files;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& content)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open isolated media fixture");
    require(file.write(content) == content.size() && file.flush(), "write entire media fixture");
}
QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "open completed media");
    const auto result = file.readAll();
    require(file.error() == QFileDevice::NoError, "read completed media");
    return result;
}
media::Entry find(const media::MediaArchiveService& archive, const QString& path)
{
    const auto result = archive.list();
    require(!result.failure, "enumerate actual archive");
    for (const auto& entry : result.entries) if (entry.path == path) return entry;
    throw std::runtime_error("Expected completed file is missing from archive");
}

QImage pixels()
{
    QImage image(33, 17, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, qRgba(x * 7, y * 13, 63, (x + y) % 2 ? 123 : 255));
    return image;
}

void archiveAndLeases(const QTemporaryDir& root)
{
    const auto directory = QDir(root.path()).filePath(QString::fromUtf8("文档媒体😀/huhucapture"));
    media::MediaArchiveService archive(directory);
    require(!QFileInfo::exists(directory) && archive.list().entries.empty(), "read-only setup never creates user folders");
    const auto original = pixels();
    const auto first = archive.archiveImage(original);
    require(first.durable(), "encode and durably archive a real PNG");
    require(QImage(first.path).convertToFormat(original.format()) == original, "source pixels and alpha survive actual PNG roundtrip");
    const auto bytes = read(first.path);
    std::vector<media::ArchiveResult> parallel(8);
    {
        std::vector<std::jthread> workers;
        for (std::size_t i = 0; i < parallel.size(); ++i)
            workers.emplace_back([&, i] { parallel[i] = archive.archiveImage(original); });
    }
    std::set<QString> paths{first.path};
    for (const auto& result : parallel) {
        require(result.durable() && paths.insert(result.path).second, "concurrent completions have exclusive names");
        require(read(result.path) == bytes, "name collision handling never alters earlier media");
    }

    write(QDir(directory).filePath("unrelated.png"), bytes);
    write(QDir(directory).filePath("20260230-120000.png"), bytes);
    write(QDir(directory).filePath("20260101-120000.png"), {});
    require(QDir(directory).mkdir("20260101-120001.png"), "create directory with a media-looking name");
    const auto alias = QDir(directory).filePath("20260101-120002.png");
    fs::create_hard_link(files::nativePath(first.path), files::nativePath(alias));
#ifndef _WIN32
    const auto link = QDir(directory).filePath("20260101-120003.png");
    fs::create_symlink(files::nativePath(first.path), files::nativePath(link));
#endif
    auto listed = archive.list();
    require(!listed.failure && listed.entries.size() == 10, "only nonempty ordinary top-level managed names are enumerated");
    auto one = find(archive, first.path);
    auto other = find(archive, alias);
    require(one.snapshot.identity == other.snapshot.identity, "hard links share actual OS identity");
    auto leased = archive.lease(one);
    require(!leased.failure && leased.lease && leased.lease->unchanged(), "history lease pins the selected identity");
    {
        const auto validated = archive.validatedLease(one);
        require(!validated.failure && validated.lease && validated.lease->unchanged(),
            "send preparation validates the actual complete media while holding its history lease");
    }
    require(archive.reserveRemoval(other).failure.code == media::Error::Busy, "sending also prevents removal through a hard-link alias");
    {
        std::jthread release([token = std::move(leased.lease)]() mutable { token.reset(); });
    }
    auto removal = archive.reserveRemoval(other);
    require(!removal.failure && removal.lease->removal(), "released reader permits an exclusive removal reservation");
    require(archive.lease(one).failure.code == media::Error::Busy, "new sends cannot race an admitted recycle operation");
    removal.lease.reset();

    const auto stale = find(archive, alias);
    const auto saved = alias + ".retained-original";
    require(QFile::rename(alias, saved), "retain original identity while replacing selected name");
    write(alias, bytes);
    require(archive.lease(stale).failure.code == media::Error::ChangedOnDisk,
        "same bytes and same name cannot substitute a different file identity");
    auto outside = one;
    outside.path = QDir(root.path()).filePath(one.name);
    write(outside.path, bytes);
    require(archive.reserveRemoval(outside).failure.code == media::Error::InvalidManagedFile,
        "a plausible timestamp outside the archive is never admitted for cleanup");

    std::stop_source cancelled;
    cancelled.request_stop();
    require(archive.archiveImage(original, cancelled.get_token()).failure.code == media::Error::Cancelled,
        "pre-cancelled image output never creates a successful archive");
    const auto source = QDir(root.path()).filePath(QString::fromUtf8("完整原件.png"));
    write(source, bytes);
    const auto imported = archive.importFile(source, media::Kind::Png);
    require(imported.durable() && read(source) == bytes && read(imported.path) == bytes,
        "streamed import retains its source and validates its complete bytes");
    require(archive.importFile(source, media::Kind::Png, cancelled.get_token()).failure.code == media::Error::Cancelled,
        "cancelled import does not discard its source");
#ifndef _WIN32
    const auto fifo = QDir(root.path()).filePath("untrusted-source.fifo");
    require(::mkfifo(files::nativePath(fifo).c_str(), 0600) == 0, "create an actual FIFO with no writer");
    require(archive.importFile(fifo, media::Kind::Png).failure.code == media::Error::Read,
        "ordinary-file admission never blocks opening a FIFO without a writer");
    require(archive.importFile(link, media::Kind::Png).failure.code == media::Error::Read,
        "external import does not follow a selected symbolic link");
#endif

#ifdef HUHUCAPTURE_TEST_WRAP_FSYNC
    failDirectorySync = true;
    const auto uncertain = archive.archiveImage(original);
    require(!uncertain.durable() && uncertain.failure.code == media::Error::DurabilityUnknown &&
        uncertain.state == HuhuCapture::Platform::CommitState::CommittedDurabilityUnknown,
        "real directory fsync failure remains a distinct publication result");
    require(!uncertain.path.isEmpty() && QImage(uncertain.path).convertToFormat(original.format()) == original,
        "an uncertain published file survives temporary cleanup and is never retried");
#endif
}

void completeValidationAndOwnership(const QTemporaryDir& root)
{
    const auto directory = QDir(root.path()).filePath("validation-archive");
    media::MediaArchiveService archive(directory);
    const auto good = archive.archiveImage(pixels());
    require(good.durable(), "create complete decoder reference");
    const auto png = read(good.path);
    std::vector<qsizetype> cuts{0, 8, png.size() / 2};
    for (qsizetype missing = 1; missing <= 12; ++missing) cuts.push_back(png.size() - missing);
    for (const auto cut : cuts) {
        auto temporary = archive.createStaging();
        require(!temporary.failure, "create isolated incomplete receive");
        write(temporary.file.path(), png.first(cut));
        const auto result = archive.commitStaged(temporary.file, media::Kind::Png);
        require(result.failure && result.state == HuhuCapture::Platform::CommitState::NotCommitted && result.path.isEmpty(),
            "truncated PNG cannot be acknowledged or published");
        require(QFileInfo::exists(temporary.file.path()), "failed validation preserves caller-owned staging");
    }
    {
        auto temporary = archive.createStaging();
        write(temporary.file.path(), png + "trailing bytes");
        require(archive.commitStaged(temporary.file, media::Kind::Png).failure.code == media::Error::InvalidMedia,
            "PNG trailer does not authorize unparsed trailing content");
    }
    {
        auto temporary = archive.createStaging();
        write(temporary.file.path(), png);
        require(archive.commitStaged(temporary.file, media::Kind::Gif).failure.code == media::Error::Unsupported,
            "a missing complete codec is explicit and cannot authorize media publication");
        media::MediaArchiveService differentOwner(directory);
        require(differentOwner.commitStaged(temporary.file, media::Kind::Png).failure.code == media::Error::InvalidManagedFile,
            "a staging token cannot be committed by an unrelated service owner");
    }
    QString retained;
    {
        auto temporary = archive.createStaging();
        write(temporary.file.path(), png.first(png.size() / 2));
        retained = temporary.file.retainForRecovery();
    }
    require(!retained.isEmpty() && QFileInfo::exists(retained), "recording recovery path survives explicit ownership release");
    QString replaced;
    {
        auto temporary = archive.createStaging();
        replaced = temporary.file.path();
        require(QFile::rename(replaced, replaced + ".original"), "move the owned empty inode aside");
        write(replaced, "another file");
        std::error_code writerError;
        const auto refusedWriter = temporary.file.openWriter(writerError);
        require(!refusedWriter && writerError && read(replaced) == "another file",
            "a substituted staging file is refused before any writer can truncate its contents");
        require(archive.commitStaged(temporary.file, media::Kind::Png).failure.code == media::Error::ChangedOnDisk,
            "staging identity is checked before decode");
    }
    require(read(replaced) == "another file", "staging destructor never deletes a substituted inode");
    {
        auto temporary = archive.createStaging();
        std::error_code error;
        auto handle = temporary.file.openWriter(error);
        require(handle && !error, "open the actual owned empty staging inode without truncation");
        QFile writer;
        require(writer.open(handle.get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle),
            "adopt the checked writer without reopening its path");
        const auto originalPath = temporary.file.path() + ".retained-inode";
        require(QFile::rename(temporary.file.path(), originalPath), "retain the actual writer's inode under another name");
        write(temporary.file.path(), "preserved substitute");
        require(writer.write(png) == png.size() && writer.flush(), "continue writing the same owned open file");
        writer.close();
        require(handle.close(error), "close the owned writer before attempting publication");
        require(read(originalPath) == png && read(temporary.file.path()) == "preserved substitute",
            "a renamed writer never redirects bytes into the replacement path");
        require(archive.commitStaged(temporary.file, media::Kind::Png).failure.code == media::Error::ChangedOnDisk,
            "a substituted staging pathname cannot be published even after the original inode was written");
    }
    {
        const auto invalidPath = QDir(directory).filePath("20260101-010203.png");
        write(invalidPath, png.first(png.size() - 1));
        const auto invalid = find(archive, invalidPath);
        require(archive.validatedLease(invalid).failure.code == media::Error::InvalidMedia,
            "an externally inserted managed filename is not proof of complete sendable media");
        require(QFile::remove(invalidPath), "remove only the explicitly owned invalid fixture");
    }
    media::MediaArchiveService bounded(QDir(root.path()).filePath("bounded"), {}, {64, 64});
    require(bounded.archiveImage(pixels()).failure.code == media::Error::Budget, "image byte budget is enforced before writing");
    require(!QFileInfo::exists(bounded.directory()), "rejected input does not create an archive directory");
    require(archive.list().entries.size() == 1, "failed receive and recovery temporaries never appear as completed history");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "create private test root");
        archiveAndLeases(temporary);
        completeValidationAndOwnership(temporary);
        std::cout << "PASS real PNG archive/import, exclusive publication, identity leases, cancellation and recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
