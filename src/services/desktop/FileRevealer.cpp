#include "services/desktop/FileRevealer.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <exception>
#include <filesystem>
#include <new>
#include <stop_token>
#include <utility>

#ifdef Q_OS_WIN
#include "platform/windows/DesktopFiles.h"
#else
#include "platform/linux/DesktopFiles.h"
#endif

namespace HuhuCapture::Desktop {
namespace {
Failure cancelled() { return {Error::Cancelled, 0, QStringLiteral("The file-manager request was cancelled")}; }
}

struct FileRevealer::State {
    std::atomic<quint64> sequence{};
    quint64 active{};
    std::stop_source stop;
    std::shared_ptr<const Media::MediaLease> lease;
    QString path;
    bool select{};
    QPointer<QFutureWatcher<RevealResult>> worker;
    QThreadPool pool;
    quint64 pendingResults{};
    bool stopping{}, shutdownReported{};
#ifndef Q_OS_WIN
    std::unique_ptr<Platform::Linux::DesktopFiles> desktop;
#endif
};
FileRevealer::FileRevealer(QObject* parent) : QObject(parent), state_(std::make_unique<State>())
{
    state_->pool.setMaxThreadCount(1);
#ifndef Q_OS_WIN
    state_->desktop = std::make_unique<Platform::Linux::DesktopFiles>();
    connect(state_->desktop.get(), &Platform::Linux::DesktopFiles::completed, this,
        [this](quint64 token, const Failure& failure, bool parentFallback) {
            if (state_->active != token) return;
            RevealResult result; result.failure = failure;
            result.path = parentFallback ? QFileInfo(state_->path).path() : state_->path;
            if (!failure) result.effect = parentFallback ? RevealEffect::OpenedParent :
                state_->select ? RevealEffect::SelectedFile : RevealEffect::OpenedDirectory;
            finish(token, std::move(result));
        });
#endif
}
FileRevealer::~FileRevealer() { state_->stop.request_stop(); state_->pool.waitForDone(); }
bool FileRevealer::busy() const noexcept { return state_->active != 0; }
quint64 FileRevealer::revealFile(std::shared_ptr<const Media::MediaLease> lease, QWindow* origin)
{
    const auto path = lease ? lease->entry().path : QString();
    return start(std::move(lease), path, true, false, origin);
}
quint64 FileRevealer::openDirectory(const QString& path, QWindow* origin, bool createIfMissing)
{ return start({}, path, false, createIfMissing, origin); }

quint64 FileRevealer::start(std::shared_ptr<const Media::MediaLease> lease, QString path,
    bool selectFile, bool createIfMissing, QWindow* origin)
{
    if (QThread::currentThread() == thread() && state_->stopping) return 0;
    auto token = state_->sequence.fetch_add(1) + 1;
    if (!token) token = state_->sequence.fetch_add(1) + 1;
    if (QThread::currentThread() != thread()) {
        QTimer::singleShot(0, this, [this, token, path = std::move(path)] {
            finish(token, {{Error::WrongThread, 0, QStringLiteral("File-manager requests require the GUI thread")}, RevealEffect::None, path});
        }); return token;
    }
    if (busy()) { finish(token, {{Error::Busy, 0, QStringLiteral("Another file-manager request is pending")}, RevealEffect::None, path}); return token; }
    if (!qGuiApp || thread() != qGuiApp->thread()) {
        finish(token, {{Error::Unavailable, 0, QStringLiteral("There is no desktop application context")}, RevealEffect::None, path}); return token;
    }
    if (path.isEmpty() || !QDir::isAbsolutePath(path) || path.contains(QChar::Null) ||
        (selectFile && (!lease || lease->removal()))) {
        finish(token, {{Error::InvalidArgument, 0, QStringLiteral("A confirmed local history file or absolute directory is required")}, RevealEffect::None, path}); return token;
    }
    const QPointer<FileRevealer> lifetime(this);
    try {
        state_->active = token; state_->path = QDir::cleanPath(path); state_->select = selectFile;
        state_->lease = std::move(lease); state_->stop = std::stop_source();
        const auto stop = state_->stop.get_token();
#ifdef Q_OS_WIN
        const HWND owner = origin ? reinterpret_cast<HWND>(origin->winId()) : nullptr;
        if (!lifetime || state_->active != token) return token;
#else
        Q_UNUSED(origin);
#endif
        auto* watcher = new QFutureWatcher<RevealResult>(this); state_->worker = watcher;
        connect(watcher, &QFutureWatcher<RevealResult>::finished, this, [this, token, watcher] {
            auto result = watcher->result();
            if (state_->worker == watcher) state_->worker.clear();
            watcher->deleteLater();
            if (state_->active != token) return;
#ifdef Q_OS_WIN
            finish(token, std::move(result));
#else
            if (result.failure) { finish(token, std::move(result)); return; }
            if (state_->stop.stop_requested()) { finish(token, {cancelled(), RevealEffect::None, state_->path}); return; }
            const auto failure = state_->desktop->start(token, state_->path, state_->select);
            if (failure) finish(token, {failure, RevealEffect::None, state_->path});
#endif
        });
        watcher->setFuture(QtConcurrent::run(&state_->pool, [readLease = state_->lease, path = state_->path,
            selectFile, createIfMissing, stop
#ifdef Q_OS_WIN
            , owner
#endif
            ]() mutable {
            auto reservation = std::move(readLease);
            RevealResult result; result.path = path;
            try {
                if (stop.stop_requested()) { result.failure = cancelled(); return result; }
                if (selectFile) {
                    if (!reservation->unchanged()) {
                        result.failure = {Error::ChangedOnDisk, 0, QStringLiteral("The confirmed file changed on disk; refresh history before locating it")}; return result;
                    }
                } else {
                    if (createIfMissing && !QDir().mkpath(path)) {
                        result.failure = {Error::FileOperation, 0, QStringLiteral("Cannot create the requested archive directory")}; return result;
                    }
                    if (!QFileInfo(path).isDir()) {
                        result.failure = {Error::InvalidArgument, 0, QStringLiteral("The requested directory does not exist")}; return result;
                    }
                }
                if (stop.stop_requested()) { result.failure = cancelled(); return result; }
#ifdef Q_OS_WIN
                const auto nativePath = std::filesystem::path(path.toStdWString());
                const auto native = selectFile ? Platform::Windows::revealDesktopFile(nativePath) :
                    Platform::Windows::openDesktopDirectory(nativePath, owner);
                if (!native.completed()) result.failure = {Error::FileOperation, native.nativeCode, QStringLiteral("Windows could not open this file-manager location")};
                else result.effect = selectFile ? RevealEffect::SelectedFile : RevealEffect::OpenedDirectory;
#endif
                return result;
            } catch (const std::bad_alloc&) { result.failure = {Error::Allocation, 0, QStringLiteral("Cannot allocate the file-manager operation")}; return result; }
            catch (const std::exception& error) { result.failure = {Error::FileOperation, 0, QString::fromUtf8(error.what())}; return result; }
        }));
    } catch (const std::bad_alloc&) {
        if (lifetime && state_->active == token)
            finish(token, {{Error::Allocation, 0, QStringLiteral("Cannot allocate a file-manager request")}, RevealEffect::None, path});
    }
    return token;
}

void FileRevealer::cancel(quint64 token)
{
    if (QThread::currentThread() != thread()) { QTimer::singleShot(0, this, [this, token] { cancel(token); }); return; }
    if (!token || state_->active != token) return;
    state_->stop.request_stop();
#ifndef Q_OS_WIN
    if (!state_->worker) state_->desktop->cancel();
#endif
    // A Windows Shell call already handed to the system cannot be undone. Its
    // actual result completes the request; closing a view never fabricates a
    // cancellation or makes the GUI wait for that worker.
}
void FileRevealer::finish(quint64 token, RevealResult result)
{
    if (state_->active == token) {
        if (state_->worker) {
            disconnect(state_->worker, nullptr, this, nullptr); state_->worker->deleteLater(); state_->worker.clear();
        }
        state_->lease.reset(); state_->path.clear(); state_->active = 0;
    }
    ++state_->pendingResults;
    QTimer::singleShot(0, this, [this, token, result = std::move(result)] {
        --state_->pendingResults;
        const QPointer<FileRevealer> life(this);
        Q_EMIT finished(token, result);
        if (life) finishShutdown();
    });
}

void FileRevealer::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (std::exchange(state_->stopping, true)) return;
    cancel(state_->active);
    QTimer::singleShot(0, this, [this] { finishShutdown(); });
}
void FileRevealer::finishShutdown()
{
    if (!state_->stopping || state_->shutdownReported || busy() || state_->pendingResults) return;
    state_->shutdownReported = true;
    Q_EMIT shutdownFinished();
}

} // namespace HuhuCapture::Desktop
