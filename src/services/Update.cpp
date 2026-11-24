#include "pch.h"
#include "app/Brand.h"
#include "core/ReleaseVersion.h"
#include "services/Lang.h"
#include "services/Setting.h"
#include "services/Update.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <vector>
#include <shlobj.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>

namespace {
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Data::Json;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Web::Http::Filters;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Security::Cryptography;
    using namespace winrt::Windows::Security::Cryptography::Core;
    using winrt::Windows::System::Threading::ThreadPoolTimer;
    using Dispatcher = winrt::Windows::System::DispatcherQueue;
    using Stage = Update::Stage;
    using Error = Update::Error;

    constexpr std::uint64_t maximumDownload = 64ull * 1024 * 1024;
    struct UpdateFailure { Error error; };
    struct UpdateSession {
        std::atomic<bool> active{ true };
        std::mutex mutex;
        IAsyncInfo operation{ nullptr };
        void track(const IAsyncInfo& next) {
            {
                std::lock_guard lock(mutex);
                operation = next;
            }
            if (!active && next) next.Cancel();
        }
        void stop() noexcept {
            active = false;
            try {
                IAsyncInfo pending{ nullptr };
                {
                    std::lock_guard lock(mutex);
                    pending = std::move(operation);
                    operation = nullptr;
                }
                // Cancellation may complete a coroutine immediately. Do not
                // hold the mutex when its cleanup calls track(nullptr).
                if (pending) pending.Cancel();
            } catch (...) {}
        }
    };
    struct RequestTimeout {
        ThreadPoolTimer timer{ nullptr };
        ~RequestTimeout() noexcept { try { if (timer) timer.Cancel(); } catch (...) {} }
    };
    struct StagedFile {
        std::filesystem::path path;
        HANDLE handle{ INVALID_HANDLE_VALUE };
        ~StagedFile() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
        }
        void close() {
            if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
        }
    };

    auto session = std::make_shared<UpdateSession>();
    Update::Status current;
    winrt::event<winrt::delegate<>> changed;
    bool disposed{}, checked{};
    UINT_PTR checkTimer{}, promptTimer{};
    std::uint64_t requestId{};

    bool busy() { return current.stage == Stage::Checking || current.stage == Stage::Downloading; }
    void publish(Update::Status next) { current = std::move(next); changed(); }
    bool post(const std::shared_ptr<UpdateSession>& lifetime, const Dispatcher& dispatcher,
        std::uint64_t id, Update::Status next, bool progress = false)
    {
        if (!lifetime->active) return false;
        try {
            return dispatcher.TryEnqueue([lifetime, id, next = std::move(next), progress]() mutable {
                if (!lifetime->active || id != requestId || (progress && current.stage != Stage::Downloading)) return;
                publish(std::move(next));
            });
        } catch (...) { return false; }
    }

    long long today()
    {
        return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()).time_since_epoch().count();
    }
    BOOL CALLBACK onEnumWin(HWND hwnd, LPARAM count)
    {
        if (!IsWindowVisible(hwnd)) return TRUE;
        ++*reinterpret_cast<int*>(count);
        return FALSE;
    }
    bool isIdle()
    {
        int count{};
        EnumThreadWindows(GetCurrentThreadId(), onEnumWin, reinterpret_cast<LPARAM>(&count));
        return count == 0;
    }

    std::filesystem::path downloadsDirectory()
    {
        PWSTR raw{};
        const auto result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &raw);
        std::filesystem::path path;
        if (SUCCEEDED(result) && raw) path = raw;
        if (raw) CoTaskMemFree(raw);
        if (path.empty()) throw UpdateFailure{ Error::Folder };
        return path;
    }

    // Only this request's exclusively created .part file is removed on failure.
    // The final rename never replaces an existing file, including the active EXE.
    std::filesystem::path retainExecutable(const std::filesystem::path& directory,
        const std::vector<BYTE>& bytes, const std::array<int, 3>& version,
        const std::wstring& versionText, const std::shared_ptr<UpdateSession>& lifetime)
    {
        StagedFile staged;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            staged.path = directory / std::format(L"{}-v{}-{}-{}-{}.part", HuhuCapture::Brand::name, versionText,
                GetCurrentProcessId(), GetTickCount64(), attempt);
            staged.handle = CreateFileW(staged.path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (staged.handle != INVALID_HANDLE_VALUE) break;
            const auto error = GetLastError();
            // A name collision is not ours to delete.
            staged.path.clear();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) throw UpdateFailure{ Error::Save };
        }
        if (staged.handle == INVALID_HANDLE_VALUE) throw UpdateFailure{ Error::Save };
        DWORD written{};
        if (!WriteFile(staged.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size() || !FlushFileBuffers(staged.handle)) throw UpdateFailure{ Error::Save };
        staged.close();
        if (Ling::Util::getVerNum(staged.path.wstring()) != version) throw UpdateFailure{ Error::Integrity };
        if (!lifetime->active) throw UpdateFailure{ Error::Save };
        for (unsigned collision = 0; collision < 10000; ++collision) {
            if (!lifetime->active) throw UpdateFailure{ Error::Save };
            const auto suffix = collision ? std::format(L"-{:03}", collision) : std::wstring{};
            const auto target = directory / std::format(L"{}-v{}-windows-x64{}.exe", HuhuCapture::Brand::name, versionText, suffix);
            if (MoveFileExW(staged.path.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                staged.path.clear();
                return target;
            }
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS &&
                GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) throw UpdateFailure{ Error::Save };
        }
        throw UpdateFailure{ Error::Save };
    }

    void promptLater();
    void CALLBACK onPromptTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (promptTimer == id) promptTimer = 0;
        if (disposed || current.stage != Stage::Available) return;
        if (!isIdle()) { promptLater(); return; }
        const auto text = std::format(L"{} {}\n\n{}", Lang::get(L"update.found"), current.version,
            Lang::get(L"update.promptDownload"));
        const auto answer = MessageBoxW(nullptr, text.c_str(), HuhuCapture::Brand::name.data(),
            MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
        if (answer == IDYES) Update::downloadLatest();
    }
    void promptLater()
    {
        if (!disposed && current.stage == Stage::Available && !promptTimer)
            promptTimer = SetTimer(nullptr, 0, 5000, onPromptTimer);
    }

    winrt::fire_and_forget request(std::shared_ptr<UpdateSession> lifetime, Dispatcher dispatcher,
        std::uint64_t id, std::array<int, 3> localVersion, bool downloadRequested)
    {
        co_await winrt::resume_background();
        Error failure = Error::Network;
        try {
            if (!lifetime->active) co_return;
            HttpBaseProtocolFilter filter;
            filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
            filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
            HttpClient client{ filter };
            client.DefaultRequestHeaders().TryAppendWithoutValidation(L"User-Agent", HuhuCapture::Brand::name);
            client.DefaultRequestHeaders().TryAppendWithoutValidation(L"Accept", L"application/vnd.github+json");
            auto metadata = client.GetStringAsync(Uri{ HuhuCapture::Brand::latestReleaseApiUrl });
            lifetime->track(metadata);
            RequestTimeout metadataTimeout{ ThreadPoolTimer::CreateTimer(
                [metadata](const ThreadPoolTimer&) { metadata.Cancel(); }, std::chrono::seconds(10)) };
            const std::wstring body{ co_await metadata };
            metadataTimeout.timer.Cancel();
            if (!lifetime->active) co_return;
            failure = Error::Metadata;
            JsonObject release{ nullptr };
            if (!JsonObject::TryParse(body, release) || release.GetNamedBoolean(L"draft", false) ||
                release.GetNamedBoolean(L"prerelease", false)) throw UpdateFailure{ failure };
            const std::wstring tag{ release.GetNamedString(L"tag_name", L"") };
            const auto version = HuhuCapture::Core::parseReleaseVersion(tag);
            if (!version) throw UpdateFailure{ failure };
            const auto versionText = std::format(L"{}.{}.{}", (*version)[0], (*version)[1], (*version)[2]);
            if (tag != L"v" + versionText) throw UpdateFailure{ failure };
            if (!downloadRequested && *version <= localVersion) {
                lifetime->track(nullptr);
                post(lifetime, dispatcher, id, {});
                co_return;
            }
            const auto expectedUrl = std::wstring{ HuhuCapture::Brand::releaseAssetPrefix } + tag + L"/" +
                std::wstring{ HuhuCapture::Brand::executableName };
            std::wstring downloadUrl, digest;
            std::uint64_t size{};
            const auto assets = release.GetNamedArray(L"assets", JsonArray{});
            for (const auto& value : assets) {
                const auto asset = value.GetObject();
                if (asset.GetNamedString(L"name", L"") != HuhuCapture::Brand::executableName) continue;
                downloadUrl = asset.GetNamedString(L"browser_download_url", L"");
                digest = asset.GetNamedString(L"digest", L"");
                const auto rawSize = asset.GetNamedNumber(L"size", 0);
                if (!std::isfinite(rawSize) || rawSize < 2 || rawSize > maximumDownload || std::floor(rawSize) != rawSize)
                    throw UpdateFailure{ failure };
                size = static_cast<std::uint64_t>(rawSize);
                break;
            }
            if (downloadUrl != expectedUrl || !digest.starts_with(L"sha256:") || digest.size() != 71 ||
                !std::all_of(digest.begin() + 7, digest.end(), [](wchar_t c) {
                    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
                })) throw UpdateFailure{ failure };
            if (!downloadRequested) {
                lifetime->track(nullptr);
                dispatcher.TryEnqueue([lifetime, id, versionText]() {
                    if (!lifetime->active || id != requestId) return;
                    publish({ Stage::Available, versionText });
                    promptLater();
                });
                co_return;
            }

            failure = Error::Folder;
            const auto directory = downloadsDirectory();
            if (!lifetime->active) co_return;
            post(lifetime, dispatcher, id, { Stage::Downloading, versionText, {}, 0, size });
            failure = Error::Network;
            auto download = client.GetBufferAsync(Uri{ downloadUrl });
            lifetime->track(download);
            auto reported = std::make_shared<std::atomic<unsigned>>(101);
            download.Progress([lifetime, dispatcher, id, versionText, size, reported](const auto& operation, const HttpProgress& progress) {
                if (!lifetime->active) { operation.Cancel(); return; }
                // HttpProgress includes response headers; allow bounded header
                // overhead and check the exact body length after completion.
                if (progress.BytesReceived > size + 1024 * 1024) { operation.Cancel(); return; }
                const auto received = (std::min)(progress.BytesReceived, size);
                const auto percent = static_cast<unsigned>(received * 100 / size);
                if (reported->exchange(percent) == percent) return;
                post(lifetime, dispatcher, id, { Stage::Downloading, versionText, {}, received, size }, true);
            });
            RequestTimeout downloadTimeout{ ThreadPoolTimer::CreateTimer(
                [download](const ThreadPoolTimer&) { download.Cancel(); }, std::chrono::seconds(60)) };
            const auto buffer = co_await download;
            downloadTimeout.timer.Cancel();
            if (!lifetime->active) co_return;
            failure = Error::Integrity;
            if (buffer.Length() != size) throw UpdateFailure{ failure };
            const auto algorithm = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
            const auto expectedHash = CryptographicBuffer::DecodeFromHexString(digest.substr(7));
            if (!CryptographicBuffer::Compare(algorithm.HashData(buffer), expectedHash)) throw UpdateFailure{ failure };
            std::vector<BYTE> bytes(buffer.Length());
            DataReader::FromBuffer(buffer).ReadBytes(bytes);
            if (bytes.size() < 2 || bytes[0] != 'M' || bytes[1] != 'Z') throw UpdateFailure{ failure };
            failure = Error::Save;
            const auto target = retainExecutable(directory, bytes, *version, versionText, lifetime);
            // A completed verified file belongs to the user, even if settings
            // closes or the UI dispatcher shuts down before receiving this event.
            lifetime->track(nullptr);
            post(lifetime, dispatcher, id, { Stage::Downloaded, versionText, target, size, size });
        } catch (const UpdateFailure& error) {
            lifetime->track(nullptr);
            post(lifetime, dispatcher, id, { Stage::Failed, {}, {}, 0, 0, error.error });
        } catch (...) {
            lifetime->track(nullptr);
            post(lifetime, dispatcher, id, { Stage::Failed, {}, {}, 0, 0, failure });
        }
    }

    void begin(bool downloadRequested)
    {
        if (disposed || busy()) return;
        auto* app = Ling::App::get();
        if (!app) return;
        if (checkTimer) KillTimer(nullptr, checkTimer);
        if (promptTimer) KillTimer(nullptr, promptTimer);
        checkTimer = promptTimer = 0;
        if (downloadRequested) {
            checked = true;
            Setting::get()->setUpdateCheckDay(today());
        }
        ++requestId;
        publish({ Stage::Checking });
        // Capture UI-owned state once; the worker never accesses settings,
        // language objects, windows, or application singletons.
        request(session, app->dq, requestId, Ling::Util::getVerNum(), downloadRequested);
    }
    void CALLBACK onCheckTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (checkTimer == id) checkTimer = 0;
        if (disposed || checked || !isIdle() || busy()) return;
        checked = true;
        Setting::get()->setUpdateCheckDay(today());
        begin(false);
    }
}

void Update::checkLater()
{
    if (disposed || busy() || checked || checkTimer) return;
    auto* app = Ling::App::get();
    if (!app || app->args[L"--auto-quit"] == L"true" || Setting::get()->getUpdateCheckDay() >= today()) return;
    checkTimer = SetTimer(nullptr, 0, 15000, onCheckTimer);
}

void Update::downloadLatest() { begin(true); }
Update::Status Update::status() { return current; }
winrt::event_token Update::subscribe(std::function<void()> handler)
{
    return changed.add([handler = std::move(handler)] { handler(); });
}
void Update::unsubscribe(winrt::event_token token) { changed.remove(token); }
void Update::dispose()
{
    disposed = true;
    session->stop();
    if (checkTimer) KillTimer(nullptr, checkTimer);
    if (promptTimer) KillTimer(nullptr, promptTimer);
    checkTimer = promptTimer = 0;
}
