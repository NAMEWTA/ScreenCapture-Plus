#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"
#include "platform/windows/ComApartment.h"
#include "platform/windows/FileHandleStream.h"
#include "platform/windows/NativeMp4Recording.h"

#include <atlbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace platform = HuhuCapture::Platform;
namespace recording = HuhuCapture::Recording;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory()
    {
        GUID id{}; wchar_t name[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, name, 40), "Create isolated test identity");
        path = std::filesystem::temp_directory_path() / (L"huhucapture-stream-" + std::wstring(name));
        require(std::filesystem::create_directory(path), "Create isolated test directory exclusively");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
struct OwnedFile {
    std::filesystem::path path;
    platform::FileHandle writer;
    explicit OwnedFile(const std::filesystem::path& directory)
        : path([&] {
            std::error_code error;
            const auto value = platform::createExclusiveTemporaryFile(directory, error);
            require(!error && !value.empty(), "Claim actual empty test file"); return value;
        }()), writer([&] {
            std::error_code error;
            const auto snapshot = platform::readFileSnapshot(path, error);
            require(snapshot.has_value(), "Read claimed file identity");
            auto value = platform::openOwnedEmptyFileForWrite(path, snapshot->identity, error);
            require(static_cast<bool>(value), "Open same actual file without truncating"); return value;
        }()) {}
};
void checkStream(const std::filesystem::path& directory)
{
    OwnedFile file(directory);
    CComPtr<IStream> stream;
    require(SUCCEEDED(platform::Windows::createFileHandleStream(file.writer.get(), &stream)), "Wrap real file handle");
    ULONG count{};
    require(stream->Write("hello", 5, &count) == S_OK && count == 5, "Write actual stream bytes");
    STATSTG status{};
    require(stream->Stat(&status, STATFLAG_NONAME) == S_OK && status.type == STGTY_STREAM &&
        status.cbSize.QuadPart == 5 && (status.grfMode & STGM_READWRITE), "Expose initialized writable stream statistics");
    require(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr) == S_OK, "Seek actual file to start");
    std::array<char, 8> bytes{};
    require(stream->Read(bytes.data(), 8, &count) == S_FALSE && count == 5 &&
        std::string_view(bytes.data(), count) == "hello", "Short real read reports S_FALSE and exact byte count");
    require(stream->Write("!", 1, nullptr) == S_OK, "Optional count pointer is valid for a real write");
    require(stream->Read(bytes.data(), 0, nullptr) == S_OK, "Zero-byte read accepts an omitted count pointer");

    CComPtr<IStream> clone;
    LARGE_INTEGER one{}; one.QuadPart = 1;
    require(stream->Seek(one, STREAM_SEEK_SET, nullptr) == S_OK && stream->Clone(&clone) == S_OK,
        "Clone same actual file with its own stream position");
    require(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr) == S_OK, "Move only original cursor");
    require(clone->Read(bytes.data(), 1, &count) == S_OK && bytes[0] == 'e', "Clone cursor remains independent");
    require(stream->Read(bytes.data(), 1, &count) == S_OK && bytes[0] == 'h', "Original cursor remains independent");
    LARGE_INTEGER maximum{}; maximum.QuadPart = (std::numeric_limits<LONGLONG>::max)();
    ULARGE_INTEGER position{};
    require(stream->Seek(maximum, STREAM_SEEK_SET, &position) == S_OK && position.QuadPart == maximum.QuadPart,
        "64-bit seek does not truncate positions");
    require(stream->Seek(one, STREAM_SEEK_CUR, nullptr) == STG_E_SEEKERROR, "Seek overflow is rejected before arithmetic wraps");
    LARGE_INTEGER four{}; four.QuadPart = 4;
    require(stream->Seek(four, STREAM_SEEK_SET, nullptr) == S_OK, "Restore safe position");
    ULARGE_INTEGER size{}; size.QuadPart = 3;
    require(stream->SetSize(size) == S_OK && stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_CUR, &position) == S_OK &&
        position.QuadPart == 4, "SetSize changes actual file length without changing logical cursor");
    require(stream->Stat(&status, STATFLAG_NONAME) == S_OK && status.cbSize.QuadPart == 3, "Real file was truncated to requested size");

    // Replacing the pathname while this owned handle is live must never redirect
    // any MF write into that unrelated replacement.
    const auto moved = directory / L"moved-owned.part";
    require(MoveFileExW(file.path.c_str(), moved.c_str(), 0), "Rename actual open object using its permitted delete sharing");
    { std::ofstream unrelated(file.path, std::ios::binary); unrelated << "unrelated"; require(bool(unrelated), "Create unrelated replacement path"); }
    require(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr) == S_OK &&
        stream->Write("owned", 5, &count) == S_OK, "Continue writing the original open object");
    require(stream->Commit(STGC_DEFAULT) == S_OK, "Flush actual disk object");
    std::error_code error;
    require(file.writer.close(error), "Close original CRT handle while independent COM handle remains");
    require(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr) == S_OK &&
        stream->Read(bytes.data(), 5, &count) == S_OK && std::string_view(bytes.data(), count) == "owned",
        "DuplicateHandle retains the same object after original descriptor closes");
    std::ifstream replacement(file.path, std::ios::binary);
    const std::string untouched((std::istreambuf_iterator<char>(replacement)), {});
    require(untouched == "unrelated", "No stream operation reopens or changes the substituted pathname");
    clone.Release(); stream.Release();
    std::cout << "PASS real file identity, short IO, 64-bit seek/size, clone and substituted-path preservation\n";
}
void checkMfBridge(const std::filesystem::path& directory)
{
    require(SUCCEEDED(MFStartup(MF_VERSION)), "Initialize actual Media Foundation");
    struct Shutdown { ~Shutdown() { MFShutdown(); } } shutdown;
    OwnedFile file(directory);
    CComPtr<IStream> native;
    require(SUCCEEDED(platform::Windows::createFileHandleStream(file.writer.get(), &native)), "Create actual IStream for MF");
    CComPtr<IMFByteStream> media;
    require(SUCCEEDED(MFCreateMFByteStreamOnStream(native, &media)), "Use official Media Foundation stream adapter");
    ULONG written{};
    require(SUCCEEDED(media->Write(reinterpret_cast<const BYTE*>("media"), 5, &written)) && written == 5,
        "Actual MF byte stream writes through the same owned file");
    require(SUCCEEDED(media->SetCurrentPosition(0)), "Actual MF wrapper supports seeking");
    std::array<BYTE, 5> bytes{}; ULONG read{};
    require(SUCCEEDED(media->Read(bytes.data(), 5, &read)) && read == 5 &&
        std::string_view(reinterpret_cast<const char*>(bytes.data()), 5) == "media", "Actual MF wrapper reads back complete bytes");
    require(SUCCEEDED(media->Flush()) && SUCCEEDED(media->Close()), "Flush and close actual MF wrapper");
    media.Release(); native.Release();
    std::error_code error;
    require(file.writer.close(error), "Close final native writer before publication");
    std::cout << "PASS actual MFCreateMFByteStreamOnStream write/seek/read/flush/close\n";
}
void checkBackendPreflight(const std::filesystem::path& directory)
{
    unsigned prepared{}, finalizing{};
    const recording::BackendEvents events{[&](const auto&) { ++prepared; }, [&] { ++finalizing; }};
    recording::BackendRequest request; request.region = {0, 0, 32, 24}; request.framesPerSecond = 0;
    OwnedFile invalid(directory);
    const auto failed = platform::Windows::runNativeMp4(request, std::move(invalid.writer), events);
    require(failed.media.firstError && failed.media.firstError->stage == HuhuCapture::Core::RecordingStage::Prepare &&
        !failed.media.canArchive() && !prepared && !finalizing, "Invalid native request never reports prepared or complete media");
    request.framesPerSecond = 30;
    std::stop_source cancelled; cancelled.request_stop();
    OwnedFile stopped(directory);
    const auto aborted = platform::Windows::runNativeMp4(request, std::move(stopped.writer), events, cancelled.get_token());
    require(aborted.stopRequested && aborted.media.completion(true) == HuhuCapture::Core::RecordingCompletion::Cancelled &&
        !prepared && !finalizing, "Pre-start cancellation closes writer without touching devices or inventing success");
    require(std::filesystem::file_size(stopped.path) == 0, "Cancelled preparation leaves no encoded sample");
    std::cout << "PASS native worker invalid-request and real owned-file cancellation boundaries; no hardware capture claimed\n";
}
}
int main()
{
    try {
        platform::ComApartment apartment(COINIT_APARTMENTTHREADED);
        require(SUCCEEDED(apartment.result()), "Initialize test COM apartment");
        TemporaryDirectory directory;
        checkStream(directory.path); checkMfBridge(directory.path); checkBackendPreflight(directory.path);
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
