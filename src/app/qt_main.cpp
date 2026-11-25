#include "app/QtApplication.h"
#include "app/Brand.h"
#include "app/Version.h"
#ifdef Q_OS_WIN
#include "platform/windows/NativeGifRecording.h"
#include "platform/windows/NativeMp4Recording.h"
#elif defined(Q_OS_LINUX)
#include "platform/linux/LiveRecording.h"
#include "platform/linux/X11Recording.h"
#endif

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageBox>
#include <QResource>
#include <QTimer>
#include <exception>

// Retain the existing editor/brand qrc object in the static UI library even
// when background startup has not constructed an editor yet.
static void initializeUiResources() { Q_INIT_RESOURCE(huhucapture_ui); }

namespace {
HuhuCapture::Application::ApplicationOptions applicationOptions(const QString& portable)
{
    using namespace HuhuCapture;
    Application::ApplicationOptions options; options.portableDirectory = portable;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        options.recordingBackend = [](const Recording::BackendRequest& request, Platform::FileHandle writer,
            const Recording::BackendEvents& events, std::stop_token stop) {
            return request.format == Recording::Format::Gif
                ? Platform::Windows::runNativeGif(request, std::move(writer), events, stop)
                : Platform::Windows::runNativeMp4(request, std::move(writer), events, stop);
        };
        const Capture::Capability usable{Capture::CapabilityState::Available, {}};
        options.recordingCapabilities = {usable, usable, usable, usable};
        // Device and encoder availability is checked by the real backend on
        // preparation, including microphone privacy and unavailable defaults.
    }
#elif defined(Q_OS_LINUX)
    const auto platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("xcb") || platform.startsWith(QStringLiteral("wayland"))) {
        if (platform == QStringLiteral("xcb")) {
            options.recordingBackend = Platform::Linux::makeX11RecordingBackend();
            options.recordingCapabilities.guardedOutsideControls = true;
        }
        options.liveRecordingBackend = Platform::Linux::makeLiveRecordingBackend;
        const Capture::Capability usable{Capture::CapabilityState::Available, {}};
        options.recordingCapabilities.mp4 = usable;
        options.recordingCapabilities.gif = usable;
        options.recordingCapabilities.systemAudio = usable;
        options.recordingCapabilities.microphone = usable;
        // LiveSource supplies actual Portal admission/permission state before
        // exposing capture actions. A recording worker then verifies its exact
        // claimed pixels, codec and requested real audio devices on preparation.
    }
#endif
    return options;
}

int startupError(QApplication& application, const QString& message)
{
    QMessageBox box(QMessageBox::Critical, QStringLiteral("huhucapture"), message, QMessageBox::Ok);
    box.setTextFormat(Qt::PlainText);
    QObject::connect(&box, &QDialog::finished, &application, [&application] { application.exit(2); });
    box.open(); return application.exec();
}
}

int main(int argc, char** argv)
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication application(argc, argv);
    application.setApplicationName(QString::fromWCharArray(HuhuCapture::Brand::name.data()));
    application.setApplicationDisplayName(application.applicationName());
    // An organization component here would silently move the existing Windows
    // RoamingAppData/huhucapture profile into a different directory.
    application.setOrganizationName({});
    application.setOrganizationDomain(QStringLiteral("github.com"));
    application.setApplicationVersion(QString::fromUtf8(HuhuCapture::Version::text.data(),
        static_cast<qsizetype>(HuhuCapture::Version::text.size())));
    application.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    application.setQuitOnLastWindowClosed(false);
    initializeUiResources();
    QIcon icon;
    for (const int edge : {16, 24, 32, 48, 64, 128, 256, 512})
        icon.addFile(QStringLiteral(":/huhucapture/brand/icon-%1.png").arg(edge), QSize(edge, edge));
    application.setWindowIcon(icon);

    const auto arguments = HuhuCapture::Application::parseApplicationArguments(application.arguments().mid(1));
    if (!arguments.failure.isEmpty()) return startupError(application, arguments.failure);
    try {
        HuhuCapture::Application::QtApplication host(application, applicationOptions(arguments.portableDirectory));
        QObject::connect(&host, &HuhuCapture::Application::QtApplication::exitReady, &application,
            [&application](int code) { application.exit(code); });
        if (!host.start(arguments.startup)) return startupError(application, QStringLiteral("The application could not accept its startup request"));
        return application.exec();
    } catch (const std::exception& error) {
        return startupError(application, QStringLiteral("The application could not initialize: ") + QString::fromUtf8(error.what()));
    }
}
