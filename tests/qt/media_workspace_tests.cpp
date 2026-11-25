#include "app/MediaWorkspace.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/localization/LanguageCatalog.h"
#include "services/media/ArchiveExport.h"
#include "ui/qt/EditorWindow.h"
#include "ui/qt/HistoryWindow.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace media = HuhuCapture::Media;
namespace desktop = HuhuCapture::Desktop;
namespace application = HuhuCapture::Application;
namespace ui = HuhuCapture::UI::Qt;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void waitFor(Predicate predicate, const char* message)
{
    const auto until = std::chrono::steady_clock::now() + 20s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < until, message); settle(); std::this_thread::sleep_for(1ms);
    }
}
template<class Work> auto worker(Work work)
{
    auto future = std::async(std::launch::async, std::move(work));
    waitFor([&] { return future.wait_for(0ms) == std::future_status::ready; }, "Real media worker timed out");
    return future.get();
}
QByteArray read(const QString& path)
{ QFile file(path); require(file.open(QIODevice::ReadOnly), "Read real exported bytes"); return file.readAll(); }
media::Entry entryAt(const media::MediaArchiveService& archive, const QString& path)
{
    const auto listed = archive.list(); require(!listed.failure, "List real archive entries");
    const auto found = std::find_if(listed.entries.begin(), listed.entries.end(), [&](const auto& entry) { return entry.path == path; });
    require(found != listed.entries.end(), "Find published archive identity"); return *found;
}
void assertPixels(const QString& path, const QImage& original)
{
    QImage actual; const auto inspected = media::inspectPng(path, {}, {}, &actual);
    require(!inspected.failure && actual.convertToFormat(QImage::Format_ARGB32) == original.convertToFormat(QImage::Format_ARGB32),
        "The complete exported PNG preserves the exact original pixels");
}
media::ExportDestination selected(const QString& path)
{
    const auto inspected = media::inspectExportDestination(path);
    require(!inspected.failure, "Read the actual destination before confirming overwrite");
    return inspected.destination;
}
void exportAndCache(const QString& root)
{
    auto archive = std::make_shared<media::MediaArchiveService>(root + QStringLiteral("/archive"));
    QImage source(63, 47, QImage::Format_ARGB32); source.fill(QColor(15, 98, 209, 117));
    source.setPixelColor(0, 0, QColor(9, 8, 7, 0)); source.setPixelColor(62, 46, QColor(240, 160, 9, 255));
    const auto saved = archive->archiveImage(source); require(saved.durable(), "Publish real PNG for export");
    const auto entry = entryAt(*archive, saved.path);
    const auto destination = root + QStringLiteral("/selected.png");
    const auto exported = media::exportArchiveFile(*archive, entry, selected(destination));
    require(!exported.failure && exported.committed, "Export actual selected PNG without removing archive");
    require(read(destination) == read(saved.path), "Save As copies exact completed archive bytes");
    assertPixels(destination, source);
    const auto same = media::exportArchiveFile(*archive, entry, selected(saved.path));
    require(!same.failure && same.committed, "Selecting existing source needs no destructive write");
    QFile old(destination); require(old.open(QIODevice::WriteOnly | QIODevice::Truncate), "Prepare explicit overwrite target");
    require(old.write("old destination") == 15, "Write previous selected destination"); old.close();
    const auto replacement = media::exportArchiveFile(*archive, entry, selected(destination));
    require(!replacement.failure && replacement.committed && read(destination) == read(saved.path), "Confirmed replacement is an actual atomic file export");

    QImage other(30, 20, QImage::Format_ARGB32); other.fill(QColor(90, 60, 30));
    const auto second = archive->archiveImage(other); require(second.durable(), "Publish second managed file");
    const auto target = entryAt(*archive, second.path);
    auto inUse = archive->lease(target); require(!inUse.failure, "Reserve actual target for an ongoing reader");
    const auto before = read(second.path);
    auto busy = media::exportArchiveFile(*archive, entry, selected(second.path));
    require(busy.failure.code == media::Error::Busy && !busy.committed && read(second.path) == before,
        "Save As cannot replace a managed file held by an application reader");
    inUse.lease.reset();
    std::stop_source cancelled; cancelled.request_stop();
    const auto aborted = media::exportArchiveFile(*archive, entry, selected(root + QStringLiteral("/cancelled.png")), cancelled.get_token());
    require(aborted.failure.code == media::Error::Cancelled && !QFile::exists(root + QStringLiteral("/cancelled.png")),
        "Cancellation creates no destination and retains the source archive");
    const auto queuedPath = root + QStringLiteral("/appeared-while-queued.png");
    const auto noOverwrite = selected(queuedPath);
    require(!noOverwrite.existing, "User selected a destination which did not exist");
    QFile appeared(queuedPath); require(appeared.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Another writer creates the formerly absent destination");
    require(appeared.write("another writer") == 14, "Keep the independently created destination bytes"); appeared.close();
    const auto conflict = media::exportArchiveFile(*archive, entry, noOverwrite);
    require(conflict.failure.code == media::Error::ChangedOnDisk && !conflict.committed && read(queuedPath) == QByteArray("another writer"),
        "An absent-target selection never authorizes replacing a file created while work was queued");
    const auto oldConfirmation = selected(destination);
    require(other.save(destination, "png"), "Another writer changes a previously confirmed existing destination");
    const auto changedBytes = read(destination);
    const auto staleApproval = media::exportArchiveFile(*archive, entry, oldConfirmation);
    require(staleApproval.failure.code == media::Error::ChangedOnDisk && read(destination) == changedBytes,
        "Overwrite consent cannot silently transfer to a different destination snapshot");
#ifndef Q_OS_WIN
    const auto link = root + QStringLiteral("/destination-link.png");
    std::filesystem::create_symlink(destination.toStdString(), link.toStdString());
    const auto refused = media::inspectExportDestination(link);
    require(refused.failure && std::filesystem::is_symlink(link.toStdString()),
        "A selected symbolic-link destination is retained, not followed or replaced");
#endif

    media::ThumbnailCache cache(archive);
    const auto first = cache.load(entry, QSize(20, 20));
    require(!first.failure && first.image.width() == 20 && first.image.height() >= 14 && first.image.height() <= 15,
        "Decode and proportionally bound real PNG cover with integer pixel rounding");
    const auto again = cache.load(entry, QSize(20, 20));
    require(!again.failure && again.image == first.image, "A matching file identity reuses valid cover pixels");
    require(cache.load(entry, QSize(4096, 20)).failure.code == media::Error::Budget,
        "Oversized cover requests fail before allocating a full output");
    QImage changed(source.size(), QImage::Format_ARGB32); changed.fill(QColor(210, 22, 33, 180));
    require(changed.save(saved.path, "png"), "Actually replace a managed file outside the application");
    const auto stale = cache.load(entry, QSize(20, 20));
    require(stale.failure.code == media::Error::ChangedOnDisk && stale.image.isNull(), "Old file snapshots cannot return cached pixels after external modification");
    const auto current = entryAt(*archive, saved.path);
    const auto refreshed = cache.load(current, QSize(20, 20));
    require(!refreshed.failure && refreshed.image != first.image, "The fresh actual snapshot decodes new cover pixels");
    require(!archive->reserveRemoval(current).failure, "Thumbnail cache retains no source-file lease");
    std::cout << "PASS real archive export, active-file protection and identity-keyed bounded thumbnails\n";
}

void workspaceJourneys(const QString& root)
{
    auto archive = std::make_shared<media::MediaArchiveService>(root + QStringLiteral("/workspace-archive"));
    desktop::SystemClipboard clipboard;
    desktop::FileRevealer revealer;
    application::MediaWorkspace workspace(archive, clipboard, revealer);
    HuhuCapture::Localization::LanguageCatalog language({
#ifdef Q_OS_WIN
        HuhuCapture::Localization::ResourcePolicy::WindowsDirectoryFallback,
#else
        HuhuCapture::Localization::ResourcePolicy::LinuxUserResources,
#endif
        root, root});
    require(language.load(QStringLiteral("zh-CN")).usable, "Load actual shared Chinese resources");
    workspace.setTranslator([&](QStringView key) { return language.text(key); });
    QImage source(80, 60, QImage::Format_ARGB32); source.fill(QColor(7, 77, 177, 210));
    source.setPixelColor(1, 2, QColor(0, 0, 0, 0));
    QPointer<ui::EditorWindow> editor = workspace.openEditor(source, true);
    require(editor, "Create actual shared pinned editor");
    unsigned completions{}; ui::OutputResult last{ui::OutputResult::Failed};
    QObject::connect(editor, &ui::EditorWindow::outputFinished, &workspace,
        [&](ui::OutputIntent, ui::OutputResult value) { ++completions; last = value; });
    require(editor->requestOutput(ui::OutputIntent::Archive), "Request actual editor archive output");
    waitFor([&] { return completions == 1 && !workspace.busy(); }, "Editor archive completes through actual worker");
    require(last == ui::OutputResult::Succeeded && editor && editor->isVisible(), "Successful pinned archive keeps editing available");
    auto listing = worker([&] { return archive->list(); });
    require(!listing.failure && listing.entries.size() == 1, "Editor output actually enters shared history");
    worker([&] { assertPixels(listing.entries.front().path, source); return true; });
    QPointer<ui::HistoryWindow> history = workspace.openHistory();
    waitFor([&] { return history && !history->loading() && !workspace.busy(); }, "Actual history list and covers finish");
    require(history->entryCount() == 1, "History displays the actual archive result");
    unsigned opened{};
    QObject::connect(&workspace, &application::MediaWorkspace::editorOpened, &workspace, [&](auto*) { ++opened; });
    require(history->requestCommand(ui::HistoryCommand::Preview), "Request actual managed PNG preview");
    waitFor([&] { return opened == 1 && !history->commandPending() && !workspace.busy(); }, "History opens the decoded source in the shared editor");

    const auto beforeCancel = archive->list().entries.size();
    require(editor->requestOutput(ui::OutputIntent::SaveAs), "Open real Qt Save As dialog");
    auto* dialog = editor->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(dialog && editor->outputPending(), "A real destination choice keeps output pending");
    static_cast<QDialog*>(dialog)->reject();
    waitFor([&] { return completions == 2 && !editor->outputPending(); }, "Actual cancelled Save As completes");
    require(last == ui::OutputResult::Cancelled && archive->list().entries.size() == beforeCancel,
        "Cancelling the destination leaves the editor and archive intact");
    settle();
    // Close during each real Save As stage. No artificial filesystem provider
    // or worker delay is used: accepting the chooser then closing synchronously
    // guarantees the queued GUI completion has not run yet.
    const auto cancelledPath = root + QStringLiteral("/cancelled-after-selection.png");
    QPointer<ui::EditorWindow> cancellationEditor = workspace.openEditor(source, true);
    require(cancellationEditor && cancellationEditor->requestOutput(ui::OutputIntent::SaveAs), "Open actual cancellable destination chooser");
    cancellationEditor->close();
    waitFor([&] { return cancellationEditor && !cancellationEditor->outputPending() && !workspace.busy(); },
        "Closing the editor cancels its actual open chooser");
    require(archive->list().entries.size() == beforeCancel, "Closing an open chooser publishes no archive");
    settle();
    require(cancellationEditor->requestOutput(ui::OutputIntent::SaveAs), "Open chooser for queued-completion cancellation");
    auto* cancelDialog = cancellationEditor->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(cancelDialog, "Locate the actual cancellable chooser");
    cancelDialog->setDirectory(root); cancelDialog->selectFile(QFileInfo(cancelledPath).fileName());
    static_cast<QDialog*>(cancelDialog)->accept();
    cancellationEditor->close();
    waitFor([&] { return cancellationEditor && !cancellationEditor->outputPending() && !workspace.busy(); },
        "Cancellation suppresses a completed worker's queued continuation");
    require(!QFile::exists(cancelledPath) && archive->list().entries.size() == beforeCancel,
        "Cancelling after actual destination selection cannot restart archiving or export");
    settle();
    QFile overwriteTarget(cancelledPath);
    require(overwriteTarget.open(QIODevice::WriteOnly | QIODevice::NewOnly) && overwriteTarget.write("retain this file") == 16,
        "Create a real existing destination before requesting replacement");
    overwriteTarget.close();
    require(cancellationEditor->requestOutput(ui::OutputIntent::SaveAs), "Open chooser for actual overwrite confirmation cancellation");
    cancelDialog = cancellationEditor->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(cancelDialog, "Locate the overwrite destination chooser");
    cancelDialog->setDirectory(root); cancelDialog->selectFile(QFileInfo(cancelledPath).fileName());
    static_cast<QDialog*>(cancelDialog)->accept();
    waitFor([&] { return cancellationEditor->findChild<QMessageBox*>(QStringLiteral("mediaOverwriteConfirmation")); },
        "The actual existing file requires one explicit overwrite question");
    require(workspace.busy(), "The application retains the pending overwrite operation while no worker is running");
    cancellationEditor->close();
    waitFor([&] { return cancellationEditor && !cancellationEditor->outputPending() && !workspace.busy(); },
        "Closing the editor rejects the actual pending overwrite question");
    require(read(cancelledPath) == QByteArray("retain this file") && archive->list().entries.size() == beforeCancel,
        "Cancelling a real overwrite question preserves the independent destination and archive");
    cancellationEditor->close(); settle();
    require(!cancellationEditor, "A cancelled editor can then close normally");
    require(editor->requestOutput(ui::OutputIntent::SaveAs), "Request real Save As again after cancellation");
    dialog = editor->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(dialog, "Find actual second destination chooser");
    const auto selected = root + QStringLiteral("/user-selected.png");
    dialog->setDirectory(root); dialog->selectFile(QStringLiteral("user-selected.png"));
    static_cast<QDialog*>(dialog)->accept();
    waitFor([&] { return completions == 3 && !workspace.busy(); }, "Save As actually archives and writes the selected destination");
    require(last == ui::OutputResult::Succeeded && QFile::exists(selected), "Report success only for the real selected file");
    worker([&] { assertPixels(selected, source); return true; });
    waitFor([&] { return history && !history->loading() && !workspace.busy(); }, "History reload follows durable publication");
    require(history->entryCount() == 2, "Save As and direct archive share the same history service");

    require(editor->requestOutput(ui::OutputIntent::Clipboard), "Request actual editor clipboard path");
    waitFor([&] { return completions == 4 && !workspace.busy(); }, "Real system clipboard result reaches its originating editor");
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        require(last == ui::OutputResult::Failed && editor->canvas()->hasImage(), "Unavailable real clipboard retains the editing document");
    else require(last == ui::OutputResult::Succeeded, "The real supported desktop clipboard completes");
    require(archive->list().entries.size() == 3, "A published archive survives clipboard refusal or success");

    QPointer<ui::EditorWindow> disposable = workspace.openEditor(source, true);
    require(disposable && disposable->requestOutput(ui::OutputIntent::Archive), "Start actual work before destroying its editor");
    delete disposable.data();
    waitFor([&] { return !workspace.busy(); }, "Actual worker exits safely after originating window destruction");
    const auto afterClose = archive->list();
    require(!afterClose.failure, "List archive after cancelled or already-published work");
    for (const auto& item : afterClose.entries) worker([&] { assertPixels(item.path, source); return true; });
    history->close(); settle(); require(!history, "Closing history destroys only the view");
    history = workspace.openHistory();
    waitFor([&] { return history && !history->loading() && !workspace.busy(); }, "Reopening history reads durable current files");
    require(history->entryCount() == static_cast<qsizetype>(afterClose.entries.size()), "Reopened view reflects actual completed work");
    workspace.shutdown();
    require(!workspace.openEditor(source) && !workspace.openHistory(), "Shutdown refuses new media journeys");
    std::cout << "PASS actual editor, history, Save As, clipboard failure/success and destroyed-view lifetimes\n";
}

void directImageOutput(const QString& root)
{
    auto archive = std::make_shared<media::MediaArchiveService>(root + QStringLiteral("/capture-output-archive"));
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    application::MediaWorkspace workspace(archive, clipboard, revealer);
    QObject requestOwner; QWidget dialogParent; dialogParent.show();
    QImage source(71, 53, QImage::Format_ARGB32); source.fill(QColor(47, 99, 151, 173));
    unsigned editorWindows{}, completions{}; ui::OutputResult last{ui::OutputResult::Failed};
    QObject::connect(&workspace, &application::MediaWorkspace::editorOpened, &workspace, [&](auto*) { ++editorWindows; });
    const auto finished = [&](ui::OutputResult value, const QString&) { last = value; ++completions; };
    require(workspace.outputImage(&requestOwner, &dialogParent, 1, ui::OutputIntent::Archive, source, finished),
        "A capture owner uses the same actual archive pipeline without an editor");
    require(!workspace.outputImage(&requestOwner, &dialogParent, 1, ui::OutputIntent::Archive, source, finished),
        "The same live capture token cannot publish a second archive");
    waitFor([&] { return completions == 1 && !workspace.busy(); }, "Direct capture output reaches real durable completion");
    require(last == ui::OutputResult::Succeeded && editorWindows == 0, "Direct output does not construct a hidden editor");
    auto listing = worker([&] { return archive->list(); });
    require(!listing.failure && listing.entries.size() == 1, "Exactly one direct output is present in actual history");
    worker([&] { assertPixels(listing.entries.front().path, source); return true; });

    require(workspace.outputImage(&requestOwner, &dialogParent, 2, ui::OutputIntent::SaveAs, source, finished),
        "Direct capture Save As opens the real shared chooser");
    auto* dialog = dialogParent.findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(dialog, "The capture's actual parent owns its destination dialog");
    dialog->setDirectory(root); dialog->selectFile(QStringLiteral("direct-capture-cancelled.png"));
    static_cast<QDialog*>(dialog)->accept();
    workspace.cancelOutput(&requestOwner, 2);
    waitFor([&] { return completions == 2 && !workspace.busy(); }, "Direct capture cancellation suppresses queued continuations");
    require(last == ui::OutputResult::Cancelled && !QFile::exists(root + QStringLiteral("/direct-capture-cancelled.png")) &&
        archive->list().entries.size() == 1, "Direct capture cancellation creates neither an export nor an extra archive");

    auto separateParent = std::make_unique<QWidget>(); separateParent->show();
    require(workspace.outputImage(&requestOwner, separateParent.get(), 3, ui::OutputIntent::SaveAs, source, finished),
        "A real dialog parent may have a shorter lifetime than its capture session");
    separateParent.reset();
    waitFor([&] { return completions == 3 && !workspace.busy(); }, "Destroying the dialog parent settles its output without a finished signal");
    require(last == ui::OutputResult::Cancelled && archive->list().entries.size() == 1,
        "A destroyed dialog releases the original request and preserves media");

    auto disposable = std::make_unique<QObject>();
    require(workspace.outputImage(disposable.get(), &dialogParent, 4, ui::OutputIntent::SaveAs, source, finished),
        "Begin a real direct output before destroying its owner");
    disposable.reset();
    waitFor([&] { return !workspace.busy(); }, "Destroying the capture owner rejects its outstanding chooser");
    require(completions == 3 && editorWindows == 0 && archive->list().entries.size() == 1,
        "A destroyed capture owner receives no late completion or extra media");
    workspace.shutdown();
    std::cout << "PASS direct capture output shares real archive, chooser and cancellation without an editor\n";
}
}

void completedArchiveOutput(const QString& root)
{
    auto archive = std::make_shared<media::MediaArchiveService>(root + QStringLiteral("/completed-recording-output"));
    QImage source(37, 29, QImage::Format_ARGB32); source.fill(QColor(35, 75, 169, 203));
    const auto archived = worker([&] { return archive->archiveImage(source); });
    require(archived.durable(), "Create an actual completed archive before asking for output");
    const auto entry = worker([&] { return entryAt(*archive, archived.path); });
    const auto original = read(entry.path);
    desktop::SystemClipboard clipboard;
    desktop::FileRevealer revealer;
    application::MediaWorkspace workspace(archive, clipboard, revealer);
    QObject requestOwner; QWidget parent; parent.show();
    unsigned completions{}; ui::OutputResult last = ui::OutputResult::Failed;
    const auto finished = [&](ui::OutputResult result, const QString&) { ++completions; last = result; };
    require(workspace.outputArchived(&requestOwner, &parent, 1, ui::OutputIntent::SaveAs, entry, finished),
        "Admit already archived media through the shared output controller");
    auto* dialog = parent.findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(dialog && dialog->defaultSuffix() == QStringLiteral("png"), "Use the actual archived type in the shared chooser");
    const QPointer<QFileDialog> cancelledChooser(dialog);
    dialog->reject();
    waitFor([&] { return completions == 1 && !workspace.busy() && !cancelledChooser; },
        "Settle actual completed-media Save As cancellation and deferred dialog destruction");
    require(last == ui::OutputResult::Cancelled && read(entry.path) == original && archive->list().entries.size() == 1,
        "Cancelling completed-media output retains its one durable archive byte for byte");
    require(workspace.outputArchived(&requestOwner, &parent, 2, ui::OutputIntent::SaveAs, entry, finished),
        "Retry Save As of the same completed entry without re-encoding");
    dialog = parent.findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(dialog, "Find the real completed-media destination chooser");
    const auto destination = root + QStringLiteral("/completed-export.png");
    dialog->setDirectory(root); dialog->selectFile(QFileInfo(destination).fileName());
    static_cast<QDialog*>(dialog)->accept();
    waitFor([&] { return completions == 2 && !workspace.busy(); }, "Complete the real archived-file export worker");
    require(last == ui::OutputResult::Succeeded && read(destination) == original && read(entry.path) == original &&
        archive->list().entries.size() == 1, "Save As exports exact bytes and creates no duplicate history file");
    QFile changed(entry.path);
    require(changed.open(QIODevice::WriteOnly | QIODevice::Truncate) && changed.write("externally replaced") == 19,
        "Change the actual archived bytes after the output identity was recorded");
    changed.close();
    require(workspace.outputArchived(&requestOwner, &parent, 3, ui::OutputIntent::Clipboard, entry, finished),
        "Queue actual immutable-identity validation before clipboard publication");
    waitFor([&] { return completions == 3 && !workspace.busy(); }, "Reject changed archived media without hanging a receipt");
    require(last == ui::OutputResult::Failed && read(entry.path) == QByteArray("externally replaced") &&
        read(destination) == original, "Changed archive identity reports failure and preserves both remaining files");
    require(!workspace.outputArchived(&requestOwner, &parent, 4, ui::OutputIntent::Pin, entry, finished),
        "Completed video output does not infer an image-editor action");
    workspace.shutdown();
    std::cout << "PASS completed archive output reuses real chooser/export/identity checks without duplicate publication\n";
}

int main(int argc, char** argv)
{
    // These checks drive actual Qt dialog widgets. Native OS file pickers and
    // physical pointer/IME interaction remain separate desktop acceptance.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir temporary; require(temporary.isValid(), "Create isolated real media workspace");
        worker([&] { exportAndCache(temporary.path()); return true; });
        workspaceJourneys(temporary.path());
        directImageOutput(temporary.path());
        completedArchiveOutput(temporary.path());
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
