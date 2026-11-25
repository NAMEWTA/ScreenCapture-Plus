#include "platform/linux/X11Recording.h"
#include "services/media/MediaArchiveService.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace recording = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
namespace native = HuhuCapture::Platform::Linux;
namespace core = HuhuCapture::Core;
namespace {
void require(bool value, const char* detail)
{ if (!value) throw std::runtime_error(detail); }
std::size_t descriptors()
{
    std::size_t count{};
    for (const auto& item : std::filesystem::directory_iterator("/proc/self/fd")) { static_cast<void>(item); ++count; }
    return count;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(unsetenv("WAYLAND_DISPLAY") == 0 && unsetenv("XDG_SESSION_TYPE") == 0 &&
            setenv("DISPLAY", ":65432", 1) == 0, "Select an actual absent native display without touching a desktop session");
        QTemporaryDir directory;
        require(directory.isValid(), "Create an actual private native-failure archive");
        media::MediaArchiveService archive(directory.path());
        const auto before = descriptors();
        auto backend = native::makeX11RecordingBackend();
        recording::BackendRequest request;
        request.region = {0, 0, 64, 48}; request.systemAudio = false; request.microphone = false;
        request.firstFrameTimeout = std::chrono::milliseconds(500);
        request.maximumDuration = std::chrono::milliseconds(100);
        unsigned notifications{};
        const recording::BackendEvents events{[&](const auto&) { ++notifications; }, [&] { ++notifications; }};
        const auto run = [&](recording::BackendRequest value, std::stop_token stop) {
            auto staged = archive.createStaging(); require(!staged.failure, "Create an actual identity-checked output");
            std::error_code error; auto writer = staged.file.openWriter(error);
            require(bool(writer) && !error, "Open only the owned staging object");
            const auto started = core::MediaClock::now();
            auto result = backend(value, std::move(writer), events, stop);
            require(core::MediaClock::now() - started < std::chrono::seconds(3), "Missing native display must join its producer promptly");
            require(!notifications && !result.prepared && !result.media.framesWritten && !result.media.canArchive() &&
                QFileInfo(staged.file.path()).size() == 0, "Native failure cannot invent a frame, output bytes or a prepared notification");
            require(!staged.file.discard(), "The actual failed output releases its owned empty staging file");
            return result;
        };
        std::stop_source cancelled; cancelled.request_stop();
        const auto stopped = run(request, cancelled.get_token());
        require(stopped.stopRequested && !stopped.media.firstError &&
            stopped.media.completion(true) == core::RecordingCompletion::Cancelled,
            "Cancellation before native initialization remains a clean empty cancellation");
        const auto refused = [&](const auto& value) {
            const auto result = run(value, {});
            return result.media.firstError && result.media.firstError->stage == core::RecordingStage::Prepare;
        };
        auto wrong = request; wrong.coordinates = recording::CoordinateSpace::AuthorizedSourcePixels;
        require(refused(wrong),
            "An authorized-source region cannot be misinterpreted as a native X11 desktop rectangle");
        wrong = request; wrong.sourceId = wrong.generation = 1;
        require(refused(wrong),
            "Native desktop recording cannot claim an unrelated stream identity");
        wrong = request; wrong.framesPerSecond = 0;
        require(refused(wrong),
            "Invalid pacing is rejected before allocating the native polling source");
        require(setenv("WAYLAND_DISPLAY", "huhucapture-owned-unavailable-probe", 1) == 0, "Mark a Wayland environment for native refusal");
        const auto wayland = run(request, {});
        require(wayland.media.firstError && wayland.media.firstError->code == ENOTSUP,
            "Native X11 does not stand in for a Wayland compositor");
        require(unsetenv("WAYLAND_DISPLAY") == 0, "Restore the private native test environment");
        for (unsigned repeat = 0; repeat < 6; ++repeat) {
            const auto failed = run(request, {});
            require(failed.media.firstError && failed.media.firstError->stage == core::RecordingStage::Capture &&
                failed.sourceTerminal && failed.sourceTerminal->reason == core::FrameSourceEnd::PlatformFailure &&
                !failed.detail.empty(), "The actual XCB failure returns its source terminal and useful reason");
        }
        require(descriptors() == before, "Repeated native initialization failure releases every worker-owned descriptor");
        std::cout << "PASS real X11 unavailable-display failure, coordinate/currentness refusal, pre-cancel, worker join, owned output and descriptor cleanup\n";
        std::cout << "NOT RUN here: actual X server capture, cursor, monitor changes or audio devices\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
