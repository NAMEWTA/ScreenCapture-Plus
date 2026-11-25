#include "ui/qt/RecordingWindow.h"
#include "services/localization/LanguageCatalog.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QToolButton>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace ui = HuhuCapture::UI::Qt;
namespace rec = HuhuCapture::Recording;
namespace cap = HuhuCapture::Capture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void events()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class T> T* child(QWidget& window, const char* name)
{
    auto* found = window.findChild<T*>(QString::fromLatin1(name)); require(found != nullptr, name); return found;
}
void save(ui::RecordingWindow& window, const QString& path, const QString& name)
{
    events();
    if (!path.isEmpty()) require(window.grab().save(QDir(path).filePath(name + QStringLiteral(".png"))),
        "Save actual recording QWidget rendering");
}
void key(QWidget& window, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(&window, &event); events();
}
ui::RecordingCapabilities available()
{
    const cap::Capability value{cap::CapabilityState::Available, {}};
    return {value, value, value, value};
}
void controlsAndIntent(HuhuCapture::Localization::LanguageCatalog& language, const QString& artifacts)
{
    ui::RecordingWindow window;
    window.setTranslator([&](QStringView key) { return language.text(key); });
    window.setCapabilities(available()); window.show(); events();
    require(window.options().format == rec::Format::Mp4 && window.options().systemAudio && !window.options().microphone,
        "The shared toolbar preserves actual MP4/system/microphone defaults");
    auto* speaker = child<QToolButton>(window, "video.recordSystem");
    require(speaker->size() == QSize(44, 44) && speaker->iconSize() == QSize(24, 24),
        "Recording uses the same original 44/24 toolbar controls as capture and editing");
    save(window, artifacts, QStringLiteral("recording-options"));
    child<QToolButton>(window, "recordingGif")->click(); events();
    require(window.options().format == rec::Format::Gif && !window.options().systemAudio && !window.options().microphone &&
        !speaker->isEnabled() && !child<QToolButton>(window, "video.recordMic")->isEnabled(),
        "Choosing GIF clears both sources and prevents nonexistent GIF audio");
    child<QToolButton>(window, "recordingMp4")->click(); events();
    require(!window.options().systemAudio && !window.options().microphone,
        "Returning from GIF does not silently restore previously cleared audio choices");
    speaker->click(); child<QToolButton>(window, "video.recordMic")->click();
    require(window.options().systemAudio && window.options().microphone, "Both MP4 sources are independently selectable");
    unsigned starts{};
    QObject::connect(&window, &ui::RecordingWindow::startRequested, &window, [&](const auto& selected) {
        ++starts; require(selected.format == rec::Format::Mp4 && selected.systemAudio && selected.microphone,
            "Start hands the actual selected values to the session");
    });
    std::vector<rec::StopIntent> stops;
    QObject::connect(&window, &ui::RecordingWindow::stopRequested, &window, [&](auto intent) { stops.push_back(intent); });
    child<QToolButton>(window, "video.startRecord")->click(); events();
    require(starts == 1 && child<QLabel>(window, "recordingStatus")->text() == language.text(u"video.preparing") &&
        child<QLabel>(window, "recordingTimer")->text().startsWith(QStringLiteral("00:00")),
        "A start click shows preparation rather than inventing an already running encoder");
    window.setState(rec::SessionState::Recording);
    window.setElapsed(std::chrono::duration_cast<HuhuCapture::Core::MediaTime>(std::chrono::seconds(67)));
    require(child<QLabel>(window, "recordingTimer")->text() == QStringLiteral("01:07 / 120:00"),
        "Elapsed display comes from the service's monotonic duration, not counted GUI timer ticks");
    save(window, artifacts, QStringLiteral("recording-active"));
    key(window, Qt::Key_S, Qt::ControlModifier);
    child<QToolButton>(window, "video.stopClipboard")->click(); key(window, Qt::Key_Escape);
    require(stops == std::vector<rec::StopIntent>{rec::StopIntent::Save} && window.isVisible(),
        "Only the first stop intent is emitted; closing cannot destroy a finalizing native worker");
    window.setState(rec::SessionState::Finalizing);
    require(child<QLabel>(window, "recordingStatus")->text() == language.text(u"video.finalizing") &&
        !child<QToolButton>(window, "video.stopFile")->isEnabled(), "Finalization remains distinct from a completed archive");
    window.setState(rec::SessionState::Completed); window.setOutputPending(true);
    key(window, Qt::Key_Escape); require(window.isVisible(), "A pending real clipboard/file result retains its visible origin");
    unsigned retries{}, closed{};
    QObject::connect(&window, &ui::RecordingWindow::retryOutputRequested, &window, [&] { ++retries; });
    QObject::connect(&window, &ui::RecordingWindow::cancelRequested, &window, [&] { ++closed; });
    const auto retained = QStringLiteral("/实际文档/huhucapture/20261125-121314.mp4");
    window.showFailure(language.text(u"clipboard.copyFailed"), retained, true); events();
    require(child<QLabel>(window, "recordingStatus")->text().contains(retained) &&
        child<QToolButton>(window, "video.retryOutput")->isEnabled(), "An output error shows the full preserved path and an actual retry action");
    const auto retryIcon = child<QToolButton>(window, "video.retryOutput")->icon().pixmap(24, 24).toImage();
    bool paintedRetry{};
    for (int y = 0; y < retryIcon.height(); ++y) for (int x = 0; x < retryIcon.width(); ++x)
        if (qAlpha(retryIcon.pixel(x, y))) paintedRetry = true;
    require(paintedRetry, "The retry action renders a glyph from the shipped font, not an empty button");
    save(window, artifacts, QStringLiteral("recording-output-failed"));
    child<QToolButton>(window, "video.retryOutput")->click();
    child<QToolButton>(window, "video.retryOutput")->click();
    require(retries == 1, "A retry is admitted once while its actual output receipt is pending");
    window.setOutputPending(false); window.showFailure(language.text(u"clipboard.saveFailed"), retained, false); events();
    const auto* completionClose = child<QToolButton>(window, "video.retryOutput")->parentWidget()->
        findChild<QToolButton*>(QStringLiteral("video.exit"));
    require(child<QToolButton>(window, "video.retryOutput")->isHidden() &&
        completionClose && completionClose->x() == 1,
        "A hidden retry action does not leave an empty hit target in the shared toolbar");
    window.setOutputPending(false); window.close(); events();
    require(closed == 1 && !window.isVisible(), "Closing a terminal view ends its journey without a new discard request");
    std::cout << "PASS real recording options, 44/24 controls, monotonic display, first stop and output retry lifetime\n";
}
void capabilitiesAndLanguage(HuhuCapture::Localization::LanguageCatalog& language, const QString& artifacts)
{
    ui::RecordingWindow window;
    window.setTranslator([&](QStringView key) { return language.text(key); });
    auto capabilities = available();
    capabilities.gif = {cap::CapabilityState::Unavailable, QStringLiteral("Actual GIF encoder is not available")};
    capabilities.microphone = {cap::CapabilityState::Unavailable, QStringLiteral("No microphone source")};
    window.setCapabilities(capabilities); window.show(); events();
    require(child<QWidget>(window, "recordingDragHandle")->isVisible(),
        "Setup has an explicit native-move grip before a timer exists");
    window.setCaptureExcluded(false); events();
    require(child<QLabel>(window, "recordingCaptureNotice")->isVisible(),
        "A desktop that cannot exclude controls exposes that real limitation before recording");
    window.setCaptureExcluded(true); events();
    require(!child<QLabel>(window, "recordingCaptureNotice")->isVisible(),
        "An actual exclusion capability removes the inapplicable capture notice");
    require(!child<QToolButton>(window, "recordingGif")->isEnabled() &&
        child<QToolButton>(window, "recordingGif")->toolTip() == capabilities.gif.reason &&
        !child<QToolButton>(window, "video.recordMic")->isEnabled(), "The same UI exposes actual unavailable native capabilities");
    window.move(73, 81); const auto position = window.pos();
    for (const auto& entry : language.available()) {
        require(language.load(entry.code).usable, "Load each real embedded translation");
        window.setTranslator([&](QStringView key) { return language.text(key); }); events();
        require(window.pos() == position, "Translation refresh does not recenter a user-positioned recording control");
        require(!window.windowTitle().isEmpty() && window.windowTitle() != QStringLiteral("video.recordingTitle"),
            "Every shipped language has an actual recording title");
    }
    save(window, artifacts, QStringLiteral("recording-capabilities"));
    window.close();
    std::cout << "PASS unavailable capability reasons and all real language catalogs without repositioning\n";
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    try {
        const auto artifacts = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        if (!artifacts.isEmpty()) require(QDir().mkpath(artifacts), "Create owned recording rendering evidence directory");
        HuhuCapture::Localization::LanguageCatalog language({HuhuCapture::Localization::ResourcePolicy::LinuxUserResources, {}, {}});
        require(language.load(QStringLiteral("en-US")).usable, "Load actual shared English catalog");
        controlsAndIntent(language, artifacts); capabilitiesAndLanguage(language, artifacts);
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
