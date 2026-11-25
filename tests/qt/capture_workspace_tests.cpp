#include "app/CaptureWorkspace.h"
#include "app/MediaWorkspace.h"
#include "app/RecordingWorkspace.h"
#include "capture/CaptureSession.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/localization/LanguageCatalog.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

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
    std::shared_ptr<media::MediaArchiveService> archive;
    // Observers and their catalog must outlive the workspaces. On an actual
    // native assertion failure, stack unwinding still closes active windows;
    // RecordingWorkspace::shutdown updates capture capabilities synchronously
    // and those views still invoke this translator while refreshing.
    HuhuCapture::Localization::LanguageCatalog language;
    unsigned completed{}, failed{};
    bool captureStopped{}, recordingStopped{}, clipboardStopped{};
    std::vector<QPointer<ui::EditorWindow>> editors;
    desktop::SystemClipboard clipboard;
    desktop::FileRevealer revealer;
    app::MediaWorkspace media;
    app::CaptureWorkspace capture;
    // Empty means the actual recording capability is unavailable. No fake
    // recorder or successful synthetic capture is installed for these checks.
    app::RecordingWorkspace recording;

    Journey() : archive(std::make_shared<media::MediaArchiveService>(directory.path() + QStringLiteral("/archive"))),
        language({HuhuCapture::Localization::ResourcePolicy::LinuxUserResources, {}, {}}),
        media(archive, clipboard, revealer), capture(media, clipboard),
        recording(archive, media, capture, {}, {})
    {
        require(directory.isValid(), "Create an isolated actual media directory");
        require(language.load(QStringLiteral("en-US")).usable, "Load the real shared language catalog");
        const auto translator = [this](QStringView key) { return language.text(key); };
        media.setTranslator(translator); capture.setTranslator(translator); recording.setTranslator(translator);
        QObject::connect(&capture, &app::CaptureWorkspace::captureFinished, &capture, [this] { ++completed; });
        QObject::connect(&capture, &app::CaptureWorkspace::operationFailed, &capture,
            [this](const QString& message) { ++failed; std::cerr << "CAPTURE " << message.toStdString() << '\n'; });
        QObject::connect(&capture, &app::CaptureWorkspace::shutdownFinished, &capture, [this] { captureStopped = true; });
        QObject::connect(&recording, &app::RecordingWorkspace::shutdownFinished, &recording, [this] { recordingStopped = true; });
        QObject::connect(&clipboard, &desktop::SystemClipboard::shutdownFinished, &clipboard, [this] { clipboardStopped = true; });
        QObject::connect(&media, &app::MediaWorkspace::editorOpened, &media,
            [this](ui::EditorWindow* editor) { editors.push_back(editor); });
    }
    void stop()
    {
        recording.shutdown(); waitFor([&] { return recordingStopped; }, "Recording closes admission before capture shutdown");
        capture.shutdown(); media.shutdown();
        waitFor([&] { return captureStopped && !media.busy(); }, "Capture and media finish their real pending work");
        clipboard.shutdown(); waitFor([&] { return clipboardStopped; }, "Clipboard waits for actual callbacks and workers");
        require(!capture.busy() && !recording.busy() && !clipboard.busy(), "A drained journey has no hidden active task");
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
    require(journey.capture.start(), "Admit an actual native desktop capture journey");
    waitFor([&] { return journey.capture.session().stage() == cap::Stage::Selecting || journey.failed != failures; },
        "The real desktop source reaches shared selection");
    require(journey.failed == failures, "Native capture must succeed rather than falling back to a placeholder");
    const auto& snapshot = journey.capture.session().snapshot();
    require(snapshot.coordinates == cap::CoordinateSpace::DesktopPixels &&
        journey.capture.sourceIsCurrent(snapshot.sourceId, snapshot.generation), "The source keeps actual verified desktop identity");
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
    auto& session = journey.capture.session();
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
    auto expected = journey.capture.session().snapshot().image.copy(selected);
    require(journey.capture.session().request(cap::Action::Clipboard) != 0, "Request shared capture-to-clipboard output");
    waitFor([&] { return journey.completed == 1 || journey.failed; }, "Actual archive and clipboard receipt complete the capture");
    require(!journey.failed && !journey.capture.busy(), "A successful receipt closes the captured selection journey");
    auto listed = journey.listing();
    require(!listed.failure && listed.entries.size() == 1 && listed.entries.front().kind == media::Kind::Png,
        "Direct copy creates exactly one completed managed PNG");
    const auto archivedPath = listed.entries.front().path;
    require(QImage(archivedPath).convertToFormat(QImage::Format_ARGB32) == expected.convertToFormat(QImage::Format_ARGB32),
        "The real archive preserves the exact selected physical pixels");
    require(QApplication::clipboard()->image().convertToFormat(QImage::Format_ARGB32) == expected.convertToFormat(QImage::Format_ARGB32),
        "Actual desktop clipboard publication receives the same selected pixels");

    selected = selectActual(journey, pattern);
    expected = journey.capture.session().snapshot().image.copy(selected);
    require(journey.capture.session().request(cap::Action::Edit) != 0, "Open selected pixels in the existing production editor");
    waitFor([&] { return journey.completed == 2 && journey.editors.size() == 1; }, "Editor admission ends the capture once");
    auto editor = journey.editors.back();
    require(editor && editor->isVisible() && editor->canvas()->originalImage() == expected,
        "Capture editing uses the shared editor and the actual selected image");
    editor->close(); waitFor([&] { return !editor; }, "Closing the editor releases its owned view");

    selected = selectActual(journey, pattern);
    expected = journey.capture.session().snapshot().image.copy(selected);
    require(journey.capture.session().request(cap::Action::Pin) != 0, "Pin the same actual native capture through the shared media owner");
    waitFor([&] { return journey.completed == 3 && journey.editors.size() == 2; }, "Pin admission completes exactly one capture");
    editor = journey.editors.back();
    require(editor && editor->isVisible() && editor->canvas()->originalImage() == expected,
        "Pin uses the same document/canvas implementation without replacing the source");
    editor->close(); waitFor([&] { return !editor; }, "Closing the pin releases its actual window");

    selected = selectActual(journey, pattern);
    expected = journey.capture.session().snapshot().image.copy(selected);
    const auto token = journey.capture.session().request(cap::Action::SaveAs);
    require(token, "Open the real shared Save As chooser");
    QPointer<QFileDialog> chooser;
    waitFor([&] {
        auto* parent = journey.capture.window().dialogParent();
        if (parent) chooser = parent->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
        return chooser && chooser->isVisible();
    }, "The capture owns an actual visible Save As dialog");
    chooser->reject();
    waitFor([&] { return !chooser && journey.capture.session().activeRequest() == 0; },
        "Cancelling the actual chooser settles its request and releases the dialog");
    require(journey.capture.session().stage() == cap::Stage::Adjusting && journey.capture.busy() && journey.completed == 3,
        "Cancelled Save As keeps the capture and selected pixels available for another action");
    require(journey.capture.session().snapshot().image.copy(selected) == expected,
        "Cancelling output never mutates the captured pixels");
    listed = journey.listing(); require(!listed.failure && listed.entries.size() == 1 && listed.entries.front().path == archivedPath,
        "Cancelled capture export leaves the earlier completed archive untouched");
    journey.capture.session().cancel();
    waitFor([&] { return journey.completed == 4 && !journey.capture.busy(); }, "Explicit cancellation finishes the visible capture once");
    journey.stop();
    require(QFileInfo::exists(archivedPath), "Normal application shutdown preserves completed media");
    std::cout << "PASS actual native capture, shared clipboard/archive, editor, pin, Save As cancellation and service drain\n";
}

void unavailableDesktop()
{
    Journey journey;
    require(journey.capture.start(), "Admit a real unavailable-desktop request for its asynchronous result");
    waitFor([&] { return journey.completed == 1 && journey.failed == 1 && !journey.capture.busy(); },
        "Unavailable desktop reports its real failure and ends the journey once");
    require(journey.capture.session().snapshot().image.isNull() && journey.editors.empty(),
        "No missing desktop is replaced by invented pixels or an editor");
    require(!journey.capture.session().capability(cap::Action::Record).canRequest(),
        "No absent recording provider is advertised as an available action");
    const auto listed = journey.listing(); require(!listed.failure && listed.entries.empty(), "A failed capture creates no fake history");
    journey.stop();
    require(!journey.capture.start(), "Closed capture admission stays closed after actual shutdown");
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
