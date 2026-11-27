#include "app/DesktopApplication.h"
#include "services/capture/DesktopCapture.h"
#include "app/MediaWorkspace.h"
#include "app/RecordingController.h"
#include "capture/CaptureSession.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/localization/LanguageCatalog.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#include "windows_clipboard_read.h"
#endif

namespace app = HuhuCapture::Application;
namespace cap = HuhuCapture::Capture;
namespace desktop = HuhuCapture::Desktop;
namespace media = HuhuCapture::Media;
namespace ui = HuhuCapture::UI::Qt;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void events()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void waitFor(const std::function<bool()>& ready, const char* message)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!ready() && elapsed.elapsed() < 15000) { events(); QThread::msleep(2); }
    require(ready(), message);
}
template<class Function> auto worker(Function function)
{ return std::async(std::launch::async, std::move(function)).get(); }

QImage receivedClipboardImage()
{
#ifdef Q_OS_WIN
    const HuhuCapture::Tests::ClipboardLock lock;
    const HuhuCapture::Tests::GlobalRead png(RegisterClipboardFormatW(L"PNG"));
    require(png.size <= static_cast<SIZE_T>((std::numeric_limits<int>::max)()), "Native PNG fits the clipboard decoder");
    return QImage::fromData(png.data, static_cast<int>(png.size), "png");
#else
    return QApplication::clipboard()->image();
#endif
}

class ColorWindow final : public QWidget {
public:
    QColor color{39, 118, 203}; unsigned paints{};
    ColorWindow() { resize(300, 220); setWindowTitle(QStringLiteral("huhucapture capture journey validation")); }
protected:
    void paintEvent(QPaintEvent*) override
    { QPainter painter(this); painter.fillRect(rect(), color); ++paints; }
};

struct Journey {
    QTemporaryDir directory;
    app::DesktopApplicationPaths paths;
    unsigned completed{}, presentationsClosed{}, failed{}, exits{};
    std::vector<QPointer<ui::EditorWindow>> editors;
    std::shared_ptr<media::MediaArchiveService> archive;
    std::unique_ptr<app::DesktopApplication> host;
    cap::CaptureSession* model{};
    cap::DesktopCapture* native{};
    app::MediaWorkspace* mediaOwner{};
    bool unavailable{};

    explicit Journey(bool unsupported = false) : unavailable(unsupported)
    {
        require(directory.isValid(), "Create isolated actual application paths");
        paths.settingsFile = directory.path() + QStringLiteral("/config/config.json");
        paths.runtimeDirectory = directory.path() + QStringLiteral("/runtime");
        paths.dataDirectory = directory.path() + QStringLiteral("/data");
        paths.archiveDirectory = directory.path() + QStringLiteral("/archive");
        paths.persistentExecutable = QApplication::applicationFilePath();
        require(QDir().mkpath(paths.runtimeDirectory), "Create the actual IPC owner directory");
        QFile::setPermissions(paths.runtimeDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        // This facade reads the host's actual durable files; it is not injected
        // into the application and cannot substitute capture/output services.
        archive = std::make_shared<media::MediaArchiveService>(paths.archiveDirectory);
        host = std::make_unique<app::DesktopApplication>(paths);
        QObject::connect(host.get(), &app::DesktopApplication::operationFailed, host.get(),
            [this](const QString& detail) { require(!detail.isEmpty(), "Actual failures retain a reason"); ++failed; });
        QObject::connect(host.get(), &app::DesktopApplication::exitReady, host.get(), [this](int code) {
            require(code == 0, "The completed application journey exits normally"); ++exits;
        });
        HuhuCapture::Core::StartupRequest request;
        request.background = !unavailable; request.oneShot = unavailable;
        require(host->start(request), "Admit the actual application's inherited startup mode");
        waitFor([&] { return host->ready(); }, "The one production host assembles its actual services");
        model = host->findChild<cap::CaptureSession*>();
        native = host->findChild<cap::DesktopCapture*>();
        mediaOwner = host->findChild<app::MediaWorkspace*>();
        require(model && native && mediaOwner, "The host owns one common capture/session/media pipeline");
        QObject::connect(model, &cap::CaptureSession::closed, host.get(), [this] { ++completed; });
        QObject::connect(&window(), &ui::CaptureWindow::closed, host.get(), [this] {
            ++presentationsClosed;
            require(presentationsClosed == completed, "The actual capture presentation closes once after its session completes");
        });
        QObject::connect(mediaOwner, &app::MediaWorkspace::editorOpened, host.get(),
            [this](ui::EditorWindow* view) { editors.push_back(view); });
    }
    cap::CaptureSession& session() { return *model; }
    ui::CaptureWindow& window()
    {
        auto* value = host->findChild<ui::CaptureWindow*>();
        require(value, "The production host owns its sole capture presentation"); return *value;
    }
    bool startCapture() { return host->activate({}); }
    bool captureBusy() const
    {
        // Session completion hides the surfaces immediately, but the host keeps
        // admission closed until CaptureWindow drains its queued destruction.
        return native->busy() || presentationsClosed != completed || model->stage() == cap::Stage::Selecting ||
            model->stage() == cap::Stage::Adjusting || model->stage() == cap::Stage::Submitting;
    }
    void stop()
    {
        host->requestExit();
        waitFor([&] {
            for (auto* view : QApplication::topLevelWidgets()) {
                if (view->objectName() == QStringLiteral("applicationClipboardOwner") && view->isVisible()) {
                    auto* exit = view->findChild<QPushButton*>(QStringLiteral("applicationExit"));
                    require(exit, "Retained native clipboard ownership has an explicit Exit button"); exit->click();
                }
                if (auto* message = qobject_cast<QMessageBox*>(view); message && message->isVisible()) message->accept();
            }
            return exits == 1;
        }, "All real application workers and native clipboard ownership drain");
        require(!native->busy() && !mediaOwner->busy(), "No capture or archive work remains after real exit");
    }
    media::ListResult listing()
    { return worker([archive = archive] { return archive->list(); }); }
};

QRect selectActual(Journey& journey, ColorWindow& pattern)
{
    pattern.show(); pattern.raise(); pattern.activateWindow(); pattern.repaint();
    waitFor([&] { return pattern.windowHandle() && pattern.windowHandle()->isExposed() && pattern.paints; },
        "Expose and paint the actual desktop source window");
    const auto failures = journey.failed;
    const bool admitted = journey.startCapture();
    if (!admitted) std::cerr << "ADMISSION completed=" << journey.completed
        << " presentationsClosed=" << journey.presentationsClosed << " nativeBusy=" << journey.native->busy()
        << " stage=" << static_cast<int>(journey.session().stage()) << '\n';
    require(admitted, "Admit an actual native desktop capture journey");
    waitFor([&] { return journey.session().stage() == cap::Stage::Selecting || journey.failed != failures; },
        "The real desktop source reaches shared selection");
    require(journey.failed == failures, "Native capture must succeed rather than falling back to a placeholder");
    const auto& snapshot = journey.session().snapshot();
    require(snapshot.coordinates == cap::CoordinateSpace::DesktopPixels &&
        snapshot.sourceId && snapshot.generation && !snapshot.screens.isEmpty(), "The source keeps actual verified desktop identity");
    const auto logical = pattern.mapToGlobal(pattern.rect().center());
    QPoint center;
    bool mapped{};
    for (const auto& binding : snapshot.screens) {
        if (binding.screen != pattern.windowHandle()->screen()) continue;
        const auto point = cap::sourceFromScreenLocal(binding, QPointF(logical) - binding.logicalGeometry.topLeft());
        center = {qFloor(point.x()), qFloor(point.y())}; mapped = true; break;
    }
    require(mapped && snapshot.image.rect().contains(center), "Map the actual widget through its own captured screen binding");
    require(snapshot.image.pixelColor(center) == pattern.color, "The capture contains pixels actually displayed by the native window");
    const QRect selected(center - QPoint(30, 20), QSize(60, 40));
    auto& session = journey.session();
    session.pointerPress(selected.topLeft());
    const auto end = selected.topLeft() + QPoint(selected.width(), selected.height());
    session.pointerMove(end); session.pointerRelease(end, false);
    require(session.stage() == cap::Stage::Adjusting, "The real source enters the shared adjustable selection state");
    return selected;
}

void actualDesktop()
{
    Journey journey;
    ColorWindow pattern;
    auto* screen = QGuiApplication::primaryScreen(); require(screen, "An actual desktop screen is required");
    pattern.move(screen->availableGeometry().center() - QPoint(pattern.width() / 2, pattern.height() / 2));
    auto selected = selectActual(journey, pattern);
    auto expected = journey.session().snapshot().image.copy(selected);
    require(journey.session().request(cap::Action::Clipboard) != 0, "Request shared capture-to-clipboard output");
    waitFor([&] { return (journey.completed == 1 && !journey.captureBusy()) || journey.failed; },
        "Actual archive/clipboard receipt and capture presentation drain complete the journey");
    require(!journey.failed && !journey.captureBusy(), "A successful receipt closes the captured selection journey");
    auto listed = journey.listing();
    require(!listed.failure && listed.entries.size() == 1 && listed.entries.front().kind == media::Kind::Png,
        "Direct copy creates exactly one completed managed PNG");
    const auto archivedPath = listed.entries.front().path;
    require(QImage(archivedPath).convertToFormat(QImage::Format_ARGB32) == expected.convertToFormat(QImage::Format_ARGB32),
        "The real archive preserves the exact selected physical pixels");
    require(receivedClipboardImage().convertToFormat(QImage::Format_ARGB32) == expected.convertToFormat(QImage::Format_ARGB32),
        "Actual desktop clipboard publication receives the same selected pixels");

    selected = selectActual(journey, pattern);
    expected = journey.session().snapshot().image.copy(selected);
    require(journey.session().request(cap::Action::Edit) != 0, "Open selected pixels in the existing production editor");
    waitFor([&] { return journey.completed == 2 && !journey.captureBusy() && journey.editors.size() == 1; },
        "Editor admission and presentation drain end the capture once");
    auto editor = journey.editors.back();
    require(editor && editor->isVisible() && editor->canvas()->originalImage() == expected,
        "Capture editing uses the shared editor and the actual selected image");
    editor->close(); waitFor([&] { return !editor; }, "Closing the editor releases its owned view");

    selected = selectActual(journey, pattern);
    expected = journey.session().snapshot().image.copy(selected);
    require(journey.session().request(cap::Action::Pin) != 0, "Pin the same actual native capture through the shared media owner");
    waitFor([&] { return journey.completed == 3 && !journey.captureBusy() && journey.editors.size() == 2; },
        "Pin admission and presentation drain complete exactly one capture");
    editor = journey.editors.back();
    require(editor && editor->isVisible() && editor->canvas()->originalImage() == expected,
        "Pin uses the same document/canvas implementation without replacing the source");
    editor->close(); waitFor([&] { return !editor; }, "Closing the pin releases its actual window");

    selected = selectActual(journey, pattern);
    expected = journey.session().snapshot().image.copy(selected);
    const auto token = journey.session().request(cap::Action::SaveAs);
    require(token, "Open the real shared Save As chooser");
    QPointer<QFileDialog> chooser;
    waitFor([&] {
        auto* parent = journey.window().dialogParent();
        if (parent) chooser = parent->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
        return chooser && chooser->isVisible();
    }, "The capture owns an actual visible Save As dialog");
    chooser->reject();
    waitFor([&] { return !chooser && journey.session().activeRequest() == 0; },
        "Cancelling the actual chooser settles its request and releases the dialog");
    require(journey.session().stage() == cap::Stage::Adjusting && journey.captureBusy() && journey.completed == 3,
        "Cancelled Save As keeps the capture and selected pixels available for another action");
    require(journey.session().snapshot().image.copy(selected) == expected,
        "Cancelling output never mutates the captured pixels");
    listed = journey.listing(); require(!listed.failure && listed.entries.size() == 1 && listed.entries.front().path == archivedPath,
        "Cancelled capture export leaves the earlier completed archive untouched");
    journey.session().cancel();
    waitFor([&] { return journey.completed == 4 && !journey.captureBusy(); }, "Explicit cancellation finishes the visible capture once");

    // Closing the selection hands the real journey to the recording controller.
    // A no-tray launcher must stay hidden until that controller settles, or it
    // can contaminate the next captured frame despite hiding the capture UI.
    selectActual(journey, pattern);
    auto* recording = journey.host->findChild<app::RecordingController*>();
    require(recording && journey.session().request(cap::Action::Record), "Hand the native selection to the actual recording options");
    QPointer<QWidget> options;
    QPointer<QWidget> launcher;
    waitFor([&] {
        for (auto* view : QApplication::topLevelWidgets()) {
            if (view->objectName() == QStringLiteral("recordingWindow")) options = view;
            if (view->objectName() == QStringLiteral("applicationLauncher")) launcher = view;
        }
        return journey.completed == 5 && !journey.captureBusy() && !journey.window().visible() &&
            recording->busy() && options && options->isVisible();
    }, "Actual recording options take ownership after the capture windows close");
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        require(launcher && !launcher->isVisible(), "No-tray launcher remains hidden for the complete recording journey");
    options->close();
    waitFor([&] { return !recording->busy(); }, "Cancelling actual recording options settles the controller without creating media");
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        waitFor([&] { return launcher && launcher->isVisible(); }, "The no-tray launcher returns only after the recording journey ends");
    listed = journey.listing();
    require(!listed.failure && listed.entries.size() == 1 && listed.entries.front().path == archivedPath,
        "Cancelling recording options leaves the completed archive untouched");
    journey.stop();
    require(QFileInfo::exists(archivedPath), "Normal application shutdown preserves completed media");
    std::cout << "PASS actual native capture, shared clipboard/archive, editor, pin, Save As cancellation and service drain\n";
}

void unavailableDesktop()
{
    Journey journey(true);
    waitFor([&] { return journey.failed == 1 && !journey.captureBusy(); },
        "Unavailable desktop reports one actual failure without creating a selection");
    require(journey.session().snapshot().image.isNull() && journey.editors.empty(),
        "No missing desktop is replaced by invented pixels or an editor");
    require(!journey.session().capability(cap::Action::Record).canRequest(),
        "No absent recording provider is advertised as an available action");
    const auto listed = journey.listing(); require(!listed.failure && listed.entries.empty(), "A failed capture creates no fake history");
    journey.stop();
    require(!journey.startCapture(), "Closed capture admission stays closed after actual shutdown");
    std::cout << "PASS actual unsupported desktop, shared failure propagation and ordered asynchronous shutdown\n";
}
}

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    try {
        if (application.arguments().contains(QStringLiteral("--unavailable"))) {
            require(QGuiApplication::platformName() == QStringLiteral("offscreen"), "Unavailable mode uses the real offscreen backend");
            unavailableDesktop();
        } else {
            require(QGuiApplication::platformName() == QStringLiteral("xcb") || QGuiApplication::platformName() == QStringLiteral("windows"),
                "Positive capture journeys require a real native desktop");
            actualDesktop();
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
