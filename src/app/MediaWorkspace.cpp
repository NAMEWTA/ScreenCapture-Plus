#include "app/MediaWorkspace.h"
#include "services/desktop/DesktopFiles.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/media/ArchiveExport.h"
#include "ui/qt/EditorWindow.h"
#include "ui/qt/HistoryWindow.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <filesystem>
#include <map>
#include <new>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Application {
namespace {
namespace ui = UI::Qt;
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toStdWString();
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
bool sameEntry(const Media::Entry& a, const Media::Entry& b)
{ return a.path == b.path && a.name == b.name && a.kind == b.kind && a.snapshot == b.snapshot; }
Media::Failure budgetFailure()
{ return {Media::Error::Budget, QStringLiteral("The bounded media worker is busy; retry after the current operation")}; }
ui::DesktopResult desktopResult(const Media::Failure& error)
{ return !error ? ui::DesktopResult::Succeeded : error.code == Media::Error::Cancelled ?
    ui::DesktopResult::Cancelled : ui::DesktopResult::Failed; }
ui::OutputResult outputResult(const Media::Failure& error)
{ return !error ? ui::OutputResult::Succeeded : error.code == Media::Error::Cancelled ?
    ui::OutputResult::Cancelled : ui::OutputResult::Failed; }
struct WorkResult {
    Media::Failure failure;
    Media::ListResult listing;
    Media::ArchiveResult archived;
    Media::ExportResult exported;
    Media::DestinationResult destination;
    std::shared_ptr<Desktop::PreparedClipboard> clipboard;
    std::shared_ptr<const Media::MediaLease> lease;
    QImage image;
    QVector<std::pair<Media::Entry, QImage>> covers;
    std::vector<Desktop::RemovalResult> removals;
};
Media::Entry completedEntry(const Media::ArchiveResult& archived)
{
    std::error_code error;
    const auto snapshot = Platform::readFileSnapshot(nativePath(archived.path), error);
    if (!snapshot) return {};
    return {archived.path, QFileInfo(archived.path).fileName(), archived.media.kind, *snapshot};
}
}

struct MediaWorkspace::State {
    MediaWorkspace* owner;
    std::shared_ptr<Media::MediaArchiveService> archive;
    std::shared_ptr<Media::ThumbnailCache> thumbnails;
    Desktop::SystemClipboard& clipboard;
    Desktop::FileRevealer& revealer;
    ui::DesktopTranslator translator;
    QThreadPool pool;
    QPointer<ui::HistoryWindow> history;
    std::vector<QPointer<ui::EditorWindow>> editors;
    bool stopping{}, sendAvailable{}, refreshNeeded{};
    struct Job {
        // Identity only; never dereferenced. QPointer is cleared before the
        // destroyed signal, which must still cancel this view's pending work.
        QObject* origin{};
        quint64 token{};
        std::uint64_t bytes{};
        bool thumbnail{};
        std::stop_source cancellation;
    };
    std::vector<std::shared_ptr<Job>> jobs;
    std::uint64_t queuedBytes{};
    struct ClipboardWait {
        QObject* origin{};
        quint64 originToken{};
        std::function<void(const Desktop::ClipboardResult&)> completed;
    };
    std::map<quint64, ClipboardWait> clipboardWaits;
    struct RevealWait {
        QObject* origin{};
        quint64 originToken{};
        std::function<void(const Desktop::RevealResult&)> completed;
    };
    std::map<quint64, RevealWait> revealWaits;
    struct DialogWait {
        QObject* origin{};
        quint64 token{};
        QPointer<QDialog> dialog;
        bool cancelled{}, finished{};
        std::function<void(int)> completed;
    };
    std::vector<std::shared_ptr<DialogWait>> dialogs;
    struct OutputTarget {
        QObject* identity{};
        QPointer<QObject> origin;
        QPointer<QWidget> parent;
        quint64 token{};
        std::uint64_t bytes{};
        bool cancelled{};
        OutputCompletion completed;
        std::optional<Media::Entry> archived;
        QMetaObject::Connection originDestroyed;
    };
    std::vector<std::shared_ptr<OutputTarget>> outputs;
    std::uint64_t outputBytes{};
    struct Removal {
        QPointer<ui::HistoryWindow> view;
        quint64 token{};
        std::vector<std::shared_ptr<const Desktop::PermanentRemovalPermit>> permits;
        bool failed{};
        QString detail;
    };
    std::shared_ptr<Removal> removal;

    State(MediaWorkspace* q, std::shared_ptr<Media::MediaArchiveService> service,
        Desktop::SystemClipboard& systemClipboard, Desktop::FileRevealer& fileRevealer,
        Media::VideoCoverReader videoCover)
        : owner(q), archive(std::move(service)),
          thumbnails(std::make_shared<Media::ThumbnailCache>(archive, std::move(videoCover))),
          clipboard(systemClipboard), revealer(fileRevealer)
    { pool.setMaxThreadCount(2); pool.setExpiryTimeout(30000); }

    QString text(QStringView key) const
    { return translator ? translator(key) : key.toString(); }
    bool busy() const
    { return !jobs.empty() || !clipboardWaits.empty() || !revealWaits.empty() || !dialogs.empty() || !outputs.empty() || bool(removal); }
    void notifyBusy()
    {
        // Admission cannot call arbitrary listeners before its state is fully
        // registered; in particular a listener may close the application.
        QTimer::singleShot(0, owner, [view = QPointer(owner)] {
            if (view) Q_EMIT view->busyChanged(view->busy());
        });
    }

    bool submit(QObject* origin, quint64 token, std::uint64_t bytes,
        std::function<WorkResult(std::stop_token)> work, std::function<void(WorkResult)> done,
        bool thumbnail = false)
    {
        constexpr std::uint64_t maximumBytes = 512ull * 1024 * 1024;
        if (stopping || !archive || jobs.size() >= 8 || bytes > maximumBytes - queuedBytes) return false;
        auto job = std::make_shared<Job>(); job->origin = origin; job->token = token; job->bytes = bytes; job->thumbnail = thumbnail;
        auto* watcher = new QFutureWatcher<WorkResult>(owner);
        jobs.push_back(job); queuedBytes += bytes;
        QObject::connect(watcher, &QFutureWatcher<WorkResult>::finished, owner,
            [this, job, watcher, done = std::move(done)]() mutable {
                auto future = watcher->future();
                auto result = future.takeResult(); watcher->deleteLater();
                const QPointer<MediaWorkspace> alive(owner);
                // An archive published before cancellation remains visible even
                // when its originating editor has since been destroyed.
                if (!stopping && !result.archived.path.isEmpty()) owner->notifyArchiveChanged();
                if (!alive) return;
                const auto found = std::find(jobs.begin(), jobs.end(), job);
                if (found != jobs.end()) { queuedBytes -= job->bytes; jobs.erase(found); }
                if (stopping) { notifyBusy(); return; }
                // Cancellation also covers work which has returned while its
                // GUI completion is still queued. Never start a new stage or
                // show a confirmation for that cancelled operation.
                if (job->cancellation.stop_requested() && !result.failure)
                    result.failure = {Media::Error::Cancelled, {}};
                done(std::move(result));
                if (alive) notifyBusy();
            });
        try {
            watcher->setFuture(QtConcurrent::run(&pool,
                [job, work = std::move(work)]() mutable {
                    try { return work(job->cancellation.get_token()); }
                    catch (const std::bad_alloc&) { WorkResult result; result.failure = budgetFailure(); return result; }
                    catch (...) {
                        WorkResult result;
                        result.failure = {Media::Error::Read, QStringLiteral("The media operation failed before completion")};
                        return result;
                    }
                }));
        } catch (...) {
            jobs.erase(std::find(jobs.begin(), jobs.end(), job)); queuedBytes -= bytes; delete watcher; return false;
        }
        notifyBusy();
        return true;
    }

    void cancel(QObject* origin, std::optional<quint64> token = {})
    {
        const QPointer<MediaWorkspace> alive(owner);
        for (const auto& output : outputs)
            if (output->identity == origin && (!token || output->token == *token)) output->cancelled = true;
        std::vector<std::shared_ptr<DialogWait>> pendingDialogs;
        for (const auto& dialog : dialogs)
            if (dialog->origin == origin && (!token || dialog->token == *token)) {
                dialog->cancelled = true; pendingDialogs.push_back(dialog);
            }
        for (const auto& job : jobs)
            if (job->origin == origin && (!token || job->token == *token)) job->cancellation.request_stop();
        std::vector<quint64> pendingClipboard, pendingReveal;
        for (const auto& [id, waiting] : clipboardWaits)
            if (waiting.origin == origin && (!token || waiting.originToken == *token)) pendingClipboard.push_back(id);
        for (const auto& [id, waiting] : revealWaits)
            if (waiting.origin == origin && (!token || waiting.originToken == *token)) pendingReveal.push_back(id);
        for (const auto id : pendingClipboard) { clipboard.cancel(id); if (!alive) return; }
        for (const auto id : pendingReveal) { revealer.cancel(id); if (!alive) return; }
        for (const auto& pending : pendingDialogs) {
            if (pending->dialog && !pending->finished) pending->dialog->reject();
            if (!alive) return;
        }
    }
    void finishOutput(const std::shared_ptr<OutputTarget>& target, ui::OutputResult result, const QString& detail = {})
    {
        const auto found = std::find(outputs.begin(), outputs.end(), target);
        if (found == outputs.end()) return;
        outputBytes -= target->bytes; outputs.erase(found);
        QObject::disconnect(target->originDestroyed);
        notifyBusy();
        if (!stopping && target->origin)
            target->completed(target->cancelled ? ui::OutputResult::Cancelled : result, detail);
    }
    std::shared_ptr<OutputTarget> admitOutput(QObject* origin, QWidget* parent, quint64 token,
        std::uint64_t bytes, OutputCompletion completion)
    {
        constexpr std::uint64_t maximumBytes = 512ull * 1024 * 1024;
        if (QThread::currentThread() != owner->thread() || stopping || !archive || !origin ||
            origin->thread() != owner->thread() || (parent && parent->thread() != owner->thread()) ||
            !token || !completion || outputs.size() >= 8 || bytes > maximumBytes - outputBytes ||
            std::any_of(outputs.begin(), outputs.end(), [&](const auto& existing) {
                return existing->identity == origin && existing->token == token;
            })) return {};
        auto target = std::make_shared<OutputTarget>();
        target->identity = origin; target->origin = origin;
        target->parent = parent ? parent->window() : nullptr;
        target->token = token; target->bytes = bytes; target->completed = std::move(completion);
        target->originDestroyed = QObject::connect(origin, &QObject::destroyed, owner,
            [this, origin, token] { cancel(origin, token); });
        outputs.push_back(target); outputBytes += bytes;
        return target;
    }
    void finishDialog(const std::shared_ptr<DialogWait>& pending, int status)
    {
        if (pending->finished) return;
        pending->finished = true;
        dialogs.erase(std::remove(dialogs.begin(), dialogs.end(), pending), dialogs.end());
        auto done = std::move(pending->completed);
        const QPointer<MediaWorkspace> alive(owner);
        if (!stopping) done(pending->cancelled ? QDialog::Rejected : status);
        if (alive) notifyBusy();
    }
    void showDialog(QDialog* dialog, QObject* origin, quint64 token, std::function<void(int)> done)
    {
        auto pending = std::make_shared<DialogWait>();
        pending->origin = origin; pending->token = token; pending->dialog = dialog; pending->completed = std::move(done);
        dialogs.push_back(pending);
        QObject::connect(dialog, &QDialog::finished, owner,
            [this, pending](int status) { finishDialog(pending, status); });
        QObject::connect(dialog, &QObject::destroyed, owner, [this, pending] {
            // Parent destruction/direct deletion does not emit finished(). It
            // still settles the same owned output and releases its image budget.
            finishDialog(pending, QDialog::Rejected);
        });
        const QPointer<MediaWorkspace> alive(owner);
        dialog->open(); if (alive) notifyBusy();
    }
    void capabilities()
    {
        if (history) history->setCapabilities({!stopping, !stopping, !stopping, !stopping,
            !stopping, !stopping && sendAvailable, !stopping});
    }
    void refresh()
    {
        if (!history || !history->isVisible() || stopping) return;
        if (history->loading() || history->commandPending()) { refreshNeeded = true; return; }
        const QPointer<MediaWorkspace> alive(owner);
        const bool accepted = history->requestReload();
        if (alive) refreshNeeded = !accepted;
    }
    void afterCommand()
    { if (refreshNeeded) { refreshNeeded = false; refresh(); } }
    void publish(std::shared_ptr<Desktop::PreparedClipboard> content, QObject* origin,
        quint64 originToken, QWindow* window, std::function<void(const Desktop::ClipboardResult&)> done)
    {
        const auto token = clipboard.publish(std::move(content), window);
        if (!token) {
            done({{Desktop::Error::Busy, 0, QStringLiteral("The system clipboard is busy")}, {}});
            return;
        }
        clipboardWaits.emplace(token, ClipboardWait{origin, originToken, std::move(done)});
        notifyBusy();
    }
    void reveal(std::shared_ptr<const Media::MediaLease> lease, QObject* origin, quint64 originToken, QWindow* window,
        std::function<void(const Desktop::RevealResult&)> done)
    {
        const auto token = revealer.revealFile(std::move(lease), window);
        if (!token) { done({{Desktop::Error::Busy, 0, QStringLiteral("The file manager service is busy")}, {}, {}}); return; }
        revealWaits.emplace(token, RevealWait{origin, originToken, std::move(done)}); notifyBusy();
    }
    void openDirectory(QObject* origin, quint64 originToken, QWindow* window,
        std::function<void(const Desktop::RevealResult&)> done)
    {
        const auto token = revealer.openDirectory(archive->directory(), window, true);
        if (!token) { done({{Desktop::Error::Busy, 0, QStringLiteral("The file manager service is busy")}, {}, {}}); return; }
        revealWaits.emplace(token, RevealWait{origin, originToken, std::move(done)}); notifyBusy();
    }

    void selectDestination(const std::shared_ptr<OutputTarget>& target, ui::OutputIntent intent,
        QImage image, QString selectedPath)
    {
        if (!target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
        const auto bytes = image.sizeInBytes();
        if (bytes < 0 || !submit(target->identity, target->token, static_cast<std::uint64_t>(bytes),
            [selectedPath = std::move(selectedPath)](std::stop_token stop) {
                WorkResult result;
                if (stop.stop_requested()) result.failure = {Media::Error::Cancelled, {}};
                else { result.destination = Media::inspectExportDestination(selectedPath); result.failure = result.destination.failure; }
                return result;
            }, [this, target, intent, image = std::move(image)](WorkResult result) {
                if (!target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
                if (result.failure) { finishOutput(target, outputResult(result.failure), result.failure.detail); return; }
                const auto destination = result.destination.destination;
                if (!destination.existing) { output(target, intent, image, destination); return; }
                auto* question = new QMessageBox(QMessageBox::Question, text(u"tool.save"),
                    text(u"clipboard.overwriteConfirm").arg(destination.path), QMessageBox::Yes | QMessageBox::No, target->parent);
                question->setObjectName(QStringLiteral("mediaOverwriteConfirmation"));
                question->setTextFormat(Qt::PlainText); question->setDefaultButton(QMessageBox::No);
                question->setEscapeButton(QMessageBox::No); question->setAttribute(Qt::WA_DeleteOnClose);
                showDialog(question, target->identity, target->token,
                    [this, target, intent, image, destination](int answer) {
                        if (!target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
                        if (answer != QMessageBox::Yes) finishOutput(target, ui::OutputResult::Cancelled);
                        else output(target, intent, image, destination);
                    });
            })) finishOutput(target, ui::OutputResult::Failed, budgetFailure().detail);
    }

    void output(const std::shared_ptr<OutputTarget>& target, ui::OutputIntent intent, QImage image,
        std::optional<Media::ExportDestination> destination = {})
    {
        if (stopping || !target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
        if (image.isNull() && !target->archived) { finishOutput(target, ui::OutputResult::Failed); return; }
        if (intent == ui::OutputIntent::Pin) {
            const QPointer<MediaWorkspace> alive(owner);
            const auto* opened = owner->openEditor(image, true);
            if (alive) finishOutput(target, opened ? ui::OutputResult::Succeeded : ui::OutputResult::Failed);
            return;
        }
        if (intent == ui::OutputIntent::SaveAs && !destination) {
            const auto extension = target->archived && target->archived->kind == Media::Kind::Gif ? QStringLiteral("gif") :
                target->archived && target->archived->kind == Media::Kind::Mp4 ? QStringLiteral("mp4") : QStringLiteral("png");
            auto* dialog = new QFileDialog(target->parent, text(u"tool.save"), archive->directory(),
                QStringLiteral("%1 (*.%2)").arg(extension.toUpper(), extension));
            dialog->setObjectName(QStringLiteral("mediaSaveAsDialog"));
            dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setAcceptMode(QFileDialog::AcceptSave);
            dialog->setFileMode(QFileDialog::AnyFile); dialog->setDefaultSuffix(extension);
            // The worker first records the exact existing file. Only our one
            // subsequent question can authorize replacement of that snapshot.
            dialog->setOption(QFileDialog::DontConfirmOverwrite);
            dialog->selectFile(target->archived ? target->archived->name :
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QChar(u'.') + extension);
            showDialog(dialog, target->identity, target->token, [this, dialog, target, intent, image](int status) {
                if (!target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
                if (status != QDialog::Accepted) { finishOutput(target, ui::OutputResult::Cancelled); return; }
                const auto paths = dialog->selectedFiles();
                if (paths.size() != 1 || paths.front().isEmpty())
                    finishOutput(target, ui::OutputResult::Cancelled);
                else selectDestination(target, intent, image, paths.front());
            });
            return;
        }
        const auto bytes = image.sizeInBytes();
        const auto archiveService = archive;
        const auto archivedEntry = target->archived;
        const bool accepted = bytes >= 0 && submit(target->identity, target->token, static_cast<std::uint64_t>(bytes),
            [archiveService, archivedEntry, image = std::move(image), intent, destination](std::stop_token stop) {
                WorkResult result;
                if (stop.stop_requested()) { result.failure = {Media::Error::Cancelled, {}}; return result; }
                if (!archivedEntry) {
                    result.archived = archiveService->archiveImage(image, stop);
                    if (!result.archived.durable()) {
                        result.failure = result.archived.failure;
                        if (!result.failure) result.failure = {Media::Error::Commit, QStringLiteral("The archive did not confirm durable completion")};
                        return result;
                    }
                }
                try {
                const auto entry = archivedEntry ? *archivedEntry : completedEntry(result.archived);
                auto reserved = archiveService->lease(entry);
                if (reserved.failure) { result.failure = reserved.failure; return result; }
                result.lease = std::move(reserved.lease);
                if (intent == ui::OutputIntent::Clipboard) {
                    auto prepared = archivedEntry ? Desktop::prepareFile(result.lease, {}, stop) : Desktop::prepareImage(image, {}, stop);
                    if (prepared.failure) {
                        result.failure = {prepared.failure.code == Desktop::Error::Cancelled ? Media::Error::Cancelled : Media::Error::Write,
                            prepared.failure.detail}; return result;
                    }
                    result.clipboard = std::move(prepared.content);
                } else if (intent == ui::OutputIntent::SaveAs) {
                    result.exported = Media::exportArchiveFile(*archiveService, entry, *destination, stop);
                    result.failure = result.exported.failure;
                }
                } catch (const std::bad_alloc&) { result.failure = budgetFailure(); }
                catch (...) { result.failure = {Media::Error::Write, QStringLiteral("The archive is preserved, but output did not complete")}; }
                return result;
            }, [this, target, intent](WorkResult result) {
                if (!target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
                if (result.failure) {
                    auto message = result.failure.detail;
                    if (!result.archived.path.isEmpty()) message += QChar(u'\n') + result.archived.path;
                    else if (target->archived) message += QChar(u'\n') + target->archived->path;
                    finishOutput(target, outputResult(result.failure), message); return;
                }
                if (intent != ui::OutputIntent::Clipboard) {
                    finishOutput(target, ui::OutputResult::Succeeded); return;
                }
                publish(std::move(result.clipboard), target->identity, target->token, target->parent ? target->parent->windowHandle() : nullptr, [this, target](const auto& completion) {
                    if (!target->origin || target->cancelled) { finishOutput(target, ui::OutputResult::Cancelled); return; }
                    const auto status = !completion.failure ? ui::OutputResult::Succeeded :
                        completion.failure.code == Desktop::Error::Cancelled ? ui::OutputResult::Cancelled : ui::OutputResult::Failed;
                    finishOutput(target, status, completion.failure.detail);
                });
            });
        if (!accepted) finishOutput(target, ui::OutputResult::Failed, budgetFailure().detail);
    }

    void historyCommand(ui::HistoryWindow* window, quint64 token, ui::HistoryCommand command, const Media::Entry& entry)
    {
        const QPointer<ui::HistoryWindow> view(window);
        if (command == ui::HistoryCommand::Send) {
            const QPointer<MediaWorkspace> alive(owner);
            if (sendAvailable) Q_EMIT owner->sendRequested({entry});
            if (alive && view) view->completeCommand(token, sendAvailable ? ui::DesktopResult::Succeeded : ui::DesktopResult::Failed);
            return;
        }
        const auto completion = [this, view, token](const Desktop::RevealResult& result) {
            const QPointer<MediaWorkspace> alive(owner);
            if (view) view->completeCommand(token, !result.failure ? ui::DesktopResult::Succeeded :
                result.failure.code == Desktop::Error::Cancelled ? ui::DesktopResult::Cancelled : ui::DesktopResult::Failed,
                result.failure.detail);
            if (alive) afterCommand();
        };
        if (command == ui::HistoryCommand::OpenFolder) { openDirectory(view, token, view->windowHandle(), completion); return; }
        const auto archiveService = archive;
        const bool accepted = submit(window, token, 0, [archiveService, entry, command](std::stop_token stop) {
            WorkResult result;
            if (stop.stop_requested()) { result.failure = {Media::Error::Cancelled, {}}; return result; }
            auto lease = archiveService->lease(entry);
            if (lease.failure) { result.failure = lease.failure; return result; }
            result.lease = std::move(lease.lease);
            if (command == ui::HistoryCommand::Copy) {
                auto prepared = Desktop::prepareFile(result.lease, {}, stop);
                if (prepared.failure) result.failure = {
                    prepared.failure.code == Desktop::Error::Cancelled ? Media::Error::Cancelled : Media::Error::Read,
                    prepared.failure.detail};
                else result.clipboard = std::move(prepared.content);
            } else if (command == ui::HistoryCommand::Preview && entry.kind == Media::Kind::Png) {
                result.failure = Media::inspectPng(entry.path, {}, stop, &result.image).failure;
            }
            if (!result.failure && !result.lease->unchanged())
                result.failure = {Media::Error::ChangedOnDisk, QStringLiteral("The selected media changed")};
            return result;
        }, [this, view, token, command, entry, completion](WorkResult result) {
            if (!view) return;
            if (result.failure) {
                const QPointer<MediaWorkspace> alive(owner);
                view->completeCommand(token, desktopResult(result.failure), result.failure.detail);
                if (alive) afterCommand(); return;
            }
            if (command == ui::HistoryCommand::Reveal) { reveal(std::move(result.lease), view, token, view->windowHandle(), completion); return; }
            if (command == ui::HistoryCommand::Copy) {
                publish(std::move(result.clipboard), view, token, view->windowHandle(), [this, view, token](const auto& status) {
                    const QPointer<MediaWorkspace> alive(owner);
                    if (view) view->completeCommand(token, !status.failure ? ui::DesktopResult::Succeeded :
                        status.failure.code == Desktop::Error::Cancelled ? ui::DesktopResult::Cancelled : ui::DesktopResult::Failed,
                        status.failure.detail);
                    if (alive) afterCommand();
                }); return;
            }
            const QPointer<MediaWorkspace> alive(owner);
            const bool opened = entry.kind == Media::Kind::Png ? owner->openEditor(result.image, true) != nullptr :
                QDesktopServices::openUrl(QUrl::fromLocalFile(entry.path));
            if (alive && view) { view->completeCommand(token, opened ? ui::DesktopResult::Succeeded : ui::DesktopResult::Failed); afterCommand(); }
        });
        if (!accepted && view) view->completeCommand(token, ui::DesktopResult::Failed, budgetFailure().detail);
    }

    void finishRemoval(const std::shared_ptr<Removal>& operation, ui::DesktopResult status)
    {
        if (removal != operation) return;
        const QPointer<MediaWorkspace> alive(owner);
        removal.reset(); operation->permits.clear();
        thumbnails->clear();
        if (operation->view) operation->view->completeRemoval(operation->token, status, operation->detail);
        if (!alive) return;
        Q_EMIT owner->archiveChanged();
        if (alive) notifyBusy();
    }
    void remove(ui::HistoryWindow* window, quint64 token, QVector<Media::Entry> entries)
    {
        if (removal) { window->completeRemoval(token, ui::DesktopResult::Failed, budgetFailure().detail); return; }
        auto operation = std::make_shared<Removal>(); operation->view = window; operation->token = token;
        removal = operation;
        const auto archiveService = archive;
        std::uint64_t bytes{};
        for (const auto& entry : entries) bytes += static_cast<std::uint64_t>(entry.path.size() + entry.name.size()) * 2 + sizeof(Media::Entry);
        if (bytes > 16 * 1024 * 1024 || !submit(window, token, bytes,
            [archiveService, entries = std::move(entries)](std::stop_token stop) {
                WorkResult result; result.removals.reserve(static_cast<std::size_t>(entries.size()));
                for (const auto& entry : entries) {
                    if (stop.stop_requested()) { result.failure = {Media::Error::Cancelled, {}}; break; }
                    result.removals.push_back(Desktop::recycle(*archiveService, entry, stop));
                }
                return result;
            }, [this, operation](WorkResult result) {
                if (removal != operation) return;
                if (result.failure) { operation->failed = true; operation->detail = result.failure.detail; }
                QVector<Media::Entry> ask;
                for (const auto& item : result.removals) {
                    if (item.permanentPermit) { operation->permits.push_back(item.permanentPermit); ask.push_back(item.entry); }
                    else if (!item.completed()) { operation->failed = true; operation->detail = item.failure.detail; }
                }
                if (result.failure.code != Media::Error::Cancelled && !ask.isEmpty() && operation->view && operation->view->isVisible() &&
                    operation->view->confirmPermanentRemoval(operation->token, ask)) return;
                const auto status = result.failure.code == Media::Error::Cancelled ? ui::DesktopResult::Cancelled :
                    operation->failed || !ask.isEmpty() ? ui::DesktopResult::Failed : ui::DesktopResult::Succeeded;
                finishRemoval(operation, status);
            })) {
            removal.reset();
            window->completeRemoval(token, ui::DesktopResult::Failed, budgetFailure().detail); return;
        }
    }
    void permanent(ui::HistoryWindow* view, quint64 token, const QVector<Media::Entry>& entries, bool approved)
    {
        const auto operation = removal;
        if (!operation || operation->view != view || operation->token != token) return;
        if (!approved) { finishRemoval(operation, ui::DesktopResult::Cancelled); return; }
        if (entries.size() != static_cast<qsizetype>(operation->permits.size())) {
            operation->detail = QStringLiteral("The permanent removal selection changed");
            finishRemoval(operation, ui::DesktopResult::Failed); return;
        }
        for (const auto& permit : operation->permits)
            if (std::none_of(entries.begin(), entries.end(), [&](const auto& entry) { return sameEntry(entry, permit->entry()); })) {
                operation->detail = QStringLiteral("The permanent removal selection changed");
                finishRemoval(operation, ui::DesktopResult::Failed); return;
            }
        if (!submit(view, token, 0, [permits = operation->permits](std::stop_token stop) {
            WorkResult result;
            for (const auto& permit : permits) result.removals.push_back(Desktop::permanentlyRemove(permit, stop));
            return result;
        }, [this, operation](WorkResult result) {
            if (result.failure) { operation->failed = true; operation->detail = result.failure.detail; }
            for (const auto& item : result.removals)
                if (!item.completed()) { operation->failed = true; operation->detail = item.failure.detail; }
            finishRemoval(operation, operation->failed ? ui::DesktopResult::Failed : ui::DesktopResult::Succeeded);
        })) { operation->detail = budgetFailure().detail; finishRemoval(operation, ui::DesktopResult::Failed); }
    }
};

MediaWorkspace::MediaWorkspace(std::shared_ptr<Media::MediaArchiveService> archive,
    Desktop::SystemClipboard& clipboard, Desktop::FileRevealer& revealer,
    Media::VideoCoverReader videoCover, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), clipboard, revealer, std::move(videoCover)))
{
    connect(&clipboard, &Desktop::SystemClipboard::finished, this, [this](quint64 token, const auto& result) {
        auto found = state_->clipboardWaits.find(token); if (found == state_->clipboardWaits.end()) return;
        auto callback = std::move(found->second.completed); state_->clipboardWaits.erase(found);
        const QPointer<MediaWorkspace> alive(this); if (!state_->stopping) callback(result);
        if (alive) Q_EMIT busyChanged(busy());
    });
    connect(&revealer, &Desktop::FileRevealer::finished, this, [this](quint64 token, const auto& result) {
        auto found = state_->revealWaits.find(token); if (found == state_->revealWaits.end()) return;
        auto callback = std::move(found->second.completed); state_->revealWaits.erase(found);
        const QPointer<MediaWorkspace> alive(this); if (!state_->stopping) callback(result);
        if (alive) Q_EMIT busyChanged(busy());
    });
}
MediaWorkspace::~MediaWorkspace()
{
    shutdown(); state_->pool.waitForDone();
    // These top-level windows have no QObject parent; their lifetime belongs to
    // this workspace, not to the window which originally opened them.
    if (state_->history) delete state_->history;
    const auto editors = state_->editors;
    for (const auto& editor : editors) if (editor) delete editor;
}
void MediaWorkspace::setTranslator(ui::DesktopTranslator translator)
{
    state_->translator = std::move(translator);
    if (state_->history) state_->history->setTranslator(state_->translator);
    for (const auto& editor : state_->editors) if (editor) editor->setTranslator(state_->translator);
}
void MediaWorkspace::setSendAvailable(bool available) { state_->sendAvailable = available; state_->capabilities(); }
bool MediaWorkspace::busy() const noexcept { return state_->busy(); }
void MediaWorkspace::shutdown()
{
    if (state_->stopping) return;
    state_->stopping = true;
    for (const auto& job : state_->jobs) job->cancellation.request_stop();
    std::vector<quint64> clipboardTokens, revealTokens;
    for (const auto& [token, ignored] : state_->clipboardWaits) { Q_UNUSED(ignored); clipboardTokens.push_back(token); }
    for (const auto& [token, ignored] : state_->revealWaits) { Q_UNUSED(ignored); revealTokens.push_back(token); }
    const QPointer<MediaWorkspace> alive(this);
    for (const auto token : clipboardTokens) { state_->clipboard.cancel(token); if (!alive) return; }
    for (const auto token : revealTokens) { state_->revealer.cancel(token); if (!alive) return; }
    const auto dialogs = state_->dialogs;
    for (const auto& dialog : dialogs) {
        dialog->cancelled = true;
        if (dialog->dialog && !dialog->finished) dialog->dialog->reject();
        if (!alive) return;
    }
    for (const auto& output : state_->outputs) QObject::disconnect(output->originDestroyed);
    state_->outputs.clear(); state_->outputBytes = 0;
    state_->removal.reset(); state_->capabilities();
}
bool MediaWorkspace::outputImage(QObject* origin, QWidget* dialogParent, quint64 token,
    ui::OutputIntent intent, QImage image, OutputCompletion completion)
{
    const auto bytes = image.sizeInBytes();
    if (image.isNull() || bytes < 0) return false;
    auto target = state_->admitOutput(origin, dialogParent, token, static_cast<std::uint64_t>(bytes), std::move(completion));
    if (!target) return false;
    state_->output(target, intent, std::move(image));
    return true;
}
bool MediaWorkspace::outputArchived(QObject* origin, QWidget* dialogParent, quint64 token,
    ui::OutputIntent intent, Media::Entry entry, OutputCompletion completion)
{
    if ((intent != ui::OutputIntent::SaveAs && intent != ui::OutputIntent::Clipboard) ||
        entry.path.isEmpty() || entry.name.isEmpty()) return false;
    auto target = state_->admitOutput(origin, dialogParent, token, 0, std::move(completion));
    if (!target) return false;
    target->archived = std::move(entry);
    state_->output(target, intent, {});
    return true;
}
void MediaWorkspace::cancelOutput(QObject* origin, quint64 token)
{ if (origin && token) state_->cancel(origin, token); }
ui::EditorWindow* MediaWorkspace::openEditor(const QImage& image, bool pinned)
{
    if (state_->stopping || image.isNull() || !state_->archive) return nullptr;
    auto* window = new ui::EditorWindow();
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowFlag(Qt::WindowStaysOnTopHint, pinned);
    window->setTranslator(state_->translator);
    if (const auto error = window->loadImage(image)) { delete window; return nullptr; }
    window->setCloseOnSuccessfulOutput(!pinned);
    for (const auto intent : {ui::OutputIntent::Archive, ui::OutputIntent::SaveAs, ui::OutputIntent::Clipboard, ui::OutputIntent::Pin})
        window->setOutputEnabled(intent, true);
    state_->editors.erase(std::remove_if(state_->editors.begin(), state_->editors.end(), [](const auto& value) { return value.isNull(); }), state_->editors.end());
    state_->editors.push_back(window);
    connect(window, &ui::EditorWindow::outputRequested, this, [this, window](quint64 token, ui::OutputIntent intent, const QImage& image) {
        const QPointer<ui::EditorWindow> view(window);
        if (!outputImage(window, window, token, intent, image, [view, token](ui::OutputResult result, const QString& detail) {
            if (view) view->completeOutput(token, result, detail);
        })) if (view) view->completeOutput(token, ui::OutputResult::Failed, budgetFailure().detail);
    });
    connect(window, &ui::EditorWindow::outputCancellationRequested, this, [this, window](quint64 token) { state_->cancel(window, token); });
    connect(window, &QObject::destroyed, this, [this, window] { state_->cancel(window); });
    const QPointer<ui::EditorWindow> view(window);
    Q_EMIT editorOpened(window);
    if (!view) return nullptr;
    view->show(); view->raise(); view->activateWindow(); return view;
}
ui::HistoryWindow* MediaWorkspace::openHistory()
{
    if (state_->stopping || !state_->archive) return nullptr;
    if (state_->history) { state_->history->show(); state_->history->raise(); state_->history->activateWindow(); state_->refresh(); return state_->history; }
    auto* window = new ui::HistoryWindow(); state_->history = window;
    window->setAttribute(Qt::WA_DeleteOnClose); window->setTranslator(state_->translator);
    window->setArchiveDirectory(state_->archive->directory()); state_->capabilities();
    connect(window, &ui::HistoryWindow::reloadRequested, this, [this, view = QPointer(window)](quint64 token) {
        const auto archive = state_->archive;
        if (!state_->submit(view, token, 0, [archive](std::stop_token stop) {
            WorkResult result; result.listing = archive->list(stop); result.failure = result.listing.failure; return result;
        }, [this, view, token](WorkResult result) {
            if (!view) return;
            const QPointer<MediaWorkspace> alive(this);
            if (result.failure) result.listing.failure = result.failure;
            view->completeReload(token, result.listing); if (alive) state_->afterCommand();
        })) if (view) view->completeReload(token, {budgetFailure(), {}});
    });
    connect(window, &ui::HistoryWindow::thumbnailsRequested, this,
        [this, view = QPointer(window)](quint64 generation, const QVector<Media::Entry>& entries, QSize size) {
            for (const auto& job : state_->jobs)
                if (job->origin == view.data() && job->thumbnail) job->cancellation.request_stop();
            const auto cache = state_->thumbnails;
            state_->submit(view, generation, 0, [cache, entries, size](std::stop_token stop) {
                WorkResult result;
                for (const auto& entry : entries) {
                    if (stop.stop_requested()) break;
                    auto preview = cache->load(entry, size, stop); result.covers.push_back({entry, std::move(preview.image)});
                }
                return result;
            }, [view, generation](WorkResult result) {
                if (view) for (const auto& cover : result.covers) view->completeThumbnail(generation, cover.first, cover.second);
            }, true);
        });
    connect(window, &ui::HistoryWindow::commandRequested, this,
        [this, window](quint64 token, ui::HistoryCommand command, const Media::Entry& entry) { state_->historyCommand(window, token, command, entry); });
    connect(window, &ui::HistoryWindow::removalRequested, this,
        [this, window](quint64 token, const QVector<Media::Entry>& entries) { state_->remove(window, token, entries); });
    connect(window, &ui::HistoryWindow::permanentRemovalConfirmed, this,
        [this, window](quint64 token, const QVector<Media::Entry>& entries, bool approved) { state_->permanent(window, token, entries, approved); });
    connect(window, &ui::HistoryWindow::viewClosed, this, [this, window] {
        state_->cancel(window);
        if (state_->removal && state_->removal->view == window) state_->removal.reset();
    });
    connect(window, &QObject::destroyed, this, [this, window] { state_->cancel(window); });
    window->show(); window->raise(); window->activateWindow(); window->requestReload(); return window;
}
void MediaWorkspace::notifyArchiveChanged()
{
    state_->thumbnails->clear();
    const QPointer<MediaWorkspace> alive(this);
    Q_EMIT archiveChanged(); if (alive) state_->refresh();
}
void MediaWorkspace::revealArchivedFile(const QString& path)
{
    if (state_->stopping) return;
    const auto archive = state_->archive;
    if (!state_->submit(this, 0, 0, [archive, path](std::stop_token stop) {
        WorkResult result; const auto listing = archive->list(stop);
        if (listing.failure) { result.failure = listing.failure; return result; }
        const auto found = std::find_if(listing.entries.begin(), listing.entries.end(), [&](const auto& entry) { return entry.path == path; });
        if (found == listing.entries.end()) { result.failure = {Media::Error::InvalidManagedFile, QStringLiteral("The received file is no longer in the archive")}; return result; }
        auto reserved = archive->lease(*found); result.failure = reserved.failure; result.lease = std::move(reserved.lease); return result;
    }, [this](WorkResult result) {
        if (result.failure) { Q_EMIT operationFailed(result.failure.detail); return; }
        state_->reveal(std::move(result.lease), this, 0, nullptr,
            [this](const auto& result) { if (result.failure) Q_EMIT operationFailed(result.failure.detail); });
    })) Q_EMIT operationFailed(budgetFailure().detail);
}

} // namespace HuhuCapture::Application
