#include "platform/windows/LegacyUpdateTask.h"

#include "app/Brand.h"
#include "core/UpdateManifest.h"
#include "core/UpdatePackageHeader.h"
#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <winver.h>
#include <io.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <format>
#include <limits>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace HuhuCapture::Platform::Windows::LegacyUpdates {
namespace {
namespace Policy = Core::UpdateManifest;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Filters;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Security::Cryptography::Core;
using winrt::Windows::System::Threading::ThreadPoolTimer;
using Kind = Core::UpdatePackageKind;
struct Failure { Error error; };

std::wstring wide(std::string_view value)
{
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw Failure{Error::Metadata};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw Failure{Error::Metadata};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) != count)
        throw Failure{Error::Metadata};
    return result;
}
std::string utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw Failure{Error::Metadata};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw Failure{Error::Metadata};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count)
        throw Failure{Error::Metadata};
    return result;
}
Policy::Identity identity()
{ return {utf8(Brand::name), utf8(Brand::repositoryLabel), utf8(Brand::releasesUrl), utf8(Brand::releaseAssetPrefix)}; }

bool safeUrl(const Uri& uri)
{ return uri && uri.UserName().empty() && uri.Password().empty() && uri.Fragment().empty(); }
bool validTestOrigin(const Uri& uri)
{
    return safeUrl(uri) && uri.SchemeName() == L"http" && uri.Port() > 0 && uri.Query().empty() &&
        (uri.Path().empty() || uri.Path() == L"/") &&
        (uri.Host() == L"127.0.0.1" || uri.Host() == L"[::1]" || uri.Host() == L"::1");
}
bool allowedRedirect(const Uri& next, const Uri& canonical, const Uri& testOrigin)
{
    if (!safeUrl(next)) return false;
    if (testOrigin) return validTestOrigin(testOrigin) && next.SchemeName() == testOrigin.SchemeName() &&
        next.Host() == testOrigin.Host() && next.Port() == testOrigin.Port();
    if (next.SchemeName() != L"https" || next.Port() != 443) return false;
    if (canonical.AbsoluteUri() == Brand::latestReleaseApiUrl || next.Host() == L"github.com")
        return next.AbsoluteUri() == canonical.AbsoluteUri();
    return next.Host() == L"release-assets.githubusercontent.com" || next.Host() == L"objects.githubusercontent.com";
}

class JsonView final {
public:
    explicit JsonView(IJsonValue value = nullptr) : value_(std::move(value)) {}
    bool isObject() const { return value_ && value_.ValueType() == JsonValueType::Object; }
    bool isArray() const { return value_ && value_.ValueType() == JsonValueType::Array; }
    bool isNumber() const { return value_ && value_.ValueType() == JsonValueType::Number; }
    bool isBoolean() const { return value_ && value_.ValueType() == JsonValueType::Boolean; }
    bool missing() const { return !value_; }
    double number() const { return isNumber() ? value_.GetNumber() : 0; }
    bool boolean() const { return isBoolean() && value_.GetBoolean(); }
    std::string string() const
    { return value_ && value_.ValueType() == JsonValueType::String ? utf8(value_.GetString()) : std::string{}; }
    std::size_t count() const { return isArray() ? value_.GetArray().Size() : 0; }
    JsonView value(const char* key) const
    {
        if (!isObject()) return JsonView{};
        const auto object = value_.GetObject(); const auto name = wide(key);
        return object.HasKey(name) ? JsonView(object.Lookup(name)) : JsonView{};
    }
    JsonView at(std::size_t index) const { return JsonView(value_.GetArray().GetAt(static_cast<std::uint32_t>(index))); }
    static bool validHttpsSource(const std::string& value)
    {
        try { const Uri uri(wide(value)); return safeUrl(uri) && uri.SchemeName() == L"https" && !uri.Host().empty(); }
        catch (...) { return false; }
    }
    static std::optional<std::string> comparisonPath(const std::string& value, bool windows)
    {
        if (!windows) return value;
        const auto source = wide(value);
        if (source.empty()) return std::string{};
        // Use the Windows invariant Unicode mapping for Windows path identity;
        // do not implement an ASCII-only or private Unicode case-fold table.
        const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, source.data(),
            static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
        if (!count) return {};
        std::wstring mapped(static_cast<std::size_t>(count), L'\0');
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, source.data(), static_cast<int>(source.size()),
            mapped.data(), count, nullptr, nullptr, 0) != count) return {};
        return utf8(mapped);
    }
private:
    IJsonValue value_;
};
JsonView document(const std::vector<std::uint8_t>& bytes, std::uint64_t maximum)
{
    if (bytes.empty() || bytes.size() > maximum) throw Failure{Error::Metadata};
    JsonObject object{nullptr};
    const auto text = wide({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    if (!JsonObject::TryParse(text, object)) throw Failure{Error::Metadata};
    return JsonView(object);
}

std::filesystem::path downloadsDirectory(const std::filesystem::path& requested)
{
    auto directory = requested;
    if (directory.empty()) {
        PWSTR raw{};
        const auto result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &raw);
        if (SUCCEEDED(result) && raw) directory = raw;
        if (raw) CoTaskMemFree(raw);
    }
    if (directory.empty() || !directory.is_absolute() || directory.native().find(L'\0') != std::wstring::npos)
        throw Failure{Error::Folder};
    std::error_code error;
    const auto resolved = std::filesystem::canonical(directory, error);
    if (error || resolved.empty() || !std::filesystem::is_directory(resolved, error)) throw Failure{Error::Folder};
    return resolved;
}

struct StagedFile final {
    std::filesystem::path path;
    std::optional<FileSnapshot> claimed;
    std::optional<FileHandle> writer;
    ~StagedFile() { cleanup(); }
    void cleanup() noexcept
    {
        writer.reset();
        if (!path.empty() && claimed) {
            std::error_code error;
            const auto actual = readFileSnapshot(path, error);
            if (actual && actual->identity == claimed->identity) std::filesystem::remove(path, error);
        }
        path.clear(); claimed.reset();
    }
    void create(const std::filesystem::path& directory)
    {
        std::error_code error;
        path = createExclusiveTemporaryFile(directory, error);
        if (path.empty()) throw Failure{Error::Save};
        claimed = readFileSnapshot(path, error);
        if (!claimed) throw Failure{Error::Save};
        writer.emplace(openOwnedEmptyFileForWrite(path, claimed->identity, error));
        if (!*writer) throw Failure{Error::Save};
    }
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> bytes)
    {
        if (!writer || _lseeki64(writer->get(), static_cast<__int64>(offset), SEEK_SET) < 0) return 0;
        const auto count = _read(writer->get(), bytes.data(), static_cast<unsigned>(bytes.size()));
        return count < 0 ? 0 : static_cast<std::size_t>(count);
    }
    void finish(std::uint64_t length, std::optional<Kind> kind, const std::string& version)
    {
        if (!writer || !claimed || _commit(writer->get()) != 0) throw Failure{Error::Save};
        const auto reader = [this](std::uint64_t offset, std::span<std::uint8_t> bytes) { return read(offset, bytes); };
        if (!Core::updatePackageHeader(kind.value_or(Kind::WindowsInstaller), reader)) throw Failure{Error::Integrity};
        if (!kind) {
            std::array<std::uint8_t, 64> dos{};
            if (read(0, dos) != dos.size()) throw Failure{Error::Integrity};
            const auto offset = std::uint32_t{dos[60]} | (std::uint32_t{dos[61]} << 8) |
                (std::uint32_t{dos[62]} << 16) | (std::uint32_t{dos[63]} << 24);
            std::array<std::uint8_t, 6> header{};
            if (read(offset, header) != header.size() || header[4] != 0x64 || header[5] != 0x86)
                throw Failure{Error::Integrity};
        }
        std::error_code error;
        const auto actual = readFileDescriptorSnapshot(writer->get(), error), named = readFileSnapshot(path, error);
        if (!actual || !named || actual->identity != claimed->identity || *actual != *named || actual->bytes != length)
            throw Failure{Error::Integrity};
        if (!writer->close(error)) throw Failure{Error::Save};
        writer.reset();
        const auto closed = readFileSnapshot(path, error);
        // Windows can finalize last-write timestamps when the writer closes.
        // Identity and exact length must survive; compare full revisions again
        // only after that legitimate close boundary.
        if (!closed || closed->identity != actual->identity || closed->bytes != actual->bytes)
            throw Failure{Error::Integrity};
        if (!kind) {
            const auto guard = openRegularFileReadOnly(path, error);
            if (!guard) throw Failure{Error::Integrity};
            const auto protectedFile = readFileDescriptorSnapshot(guard.get(), error);
            if (!protectedFile || *protectedFile != *closed) throw Failure{Error::Integrity};
            DWORD unused{};
            const auto size = GetFileVersionInfoSizeW(path.c_str(), &unused);
            if (!size || size > 1024 * 1024) throw Failure{Error::Integrity};
            std::vector<std::uint8_t> bytes(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) throw Failure{Error::Integrity};
            VS_FIXEDFILEINFO* value{}; UINT count{};
            if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&value), &count) ||
                count < sizeof(VS_FIXEDFILEINFO) || value->dwSignature != 0xfeef04bd || value->dwFileType != VFT_APP)
                throw Failure{Error::Integrity};
            const auto expected = Core::parseReleaseVersion(wide(version));
            if (!expected || HIWORD(value->dwFileVersionMS) != (*expected)[0] || LOWORD(value->dwFileVersionMS) != (*expected)[1] ||
                HIWORD(value->dwFileVersionLS) != (*expected)[2] || LOWORD(value->dwFileVersionLS) != 0)
                throw Failure{Error::Integrity};
        }
        const auto after = readFileSnapshot(path, error);
        if (!after || *after != *closed) throw Failure{Error::Integrity};
    }
};
struct ResponseScope final {
    HttpResponseMessage response{nullptr};
    IInputStream stream{nullptr};
    ~ResponseScope()
    {
        try { if (stream) stream.Close(); } catch (...) {}
        try { if (response) response.Close(); } catch (...) {}
    }
};
struct TimerScope final {
    ThreadPoolTimer timer{nullptr};
    void stop() noexcept
    { try { if (timer) std::exchange(timer, nullptr).Cancel(); } catch (...) {} }
    ~TimerScope() { stop(); }
};
struct ClientScope final {
    HttpClient client{nullptr};
    ~ClientScope() { try { if (client) client.Close(); } catch (...) {} }
};
} // namespace

struct Task::State final : std::enable_shared_from_this<State> {
    Options options;
    Observer observer;
    std::atomic_bool cancelled{}, done{};
    std::mutex mutex;
    std::mutex watcherMutex;
    bool watcherStopped{};
    IAsyncInfo operation{nullptr};
    std::uint64_t deadline{}, idleDeadline{};
    bool timedOut{};
    Status status;
    Error failure{Error::Network};

    State(Options value, Observer callback) : options(std::move(value)), observer(std::move(callback)) {}
    void notify() noexcept { try { if (observer) observer(status); } catch (...) {} }
    void cancelPending() noexcept
    {
        try {
            IAsyncInfo pending{nullptr};
            { std::lock_guard lock(mutex); pending = operation; }
            if (pending) pending.Cancel();
        } catch (...) {}
    }
    void check()
    {
        if (cancelled.load(std::memory_order_acquire)) throw Failure{Error::None};
        std::lock_guard lock(mutex);
        if (timedOut) throw Failure{Error::Network};
    }
    void track(const IAsyncInfo& next)
    {
        bool cancel{};
        { std::lock_guard lock(mutex); operation = next; cancel = timedOut || cancelled.load(std::memory_order_acquire); }
        if (cancel && next) next.Cancel();
    }
    template<class Operation>
    auto waitResult(Operation next)
    {
        track(next);
        try {
            auto result = next.get(); // This method runs only on our MTA worker.
            track(nullptr); check();
            { std::lock_guard lock(mutex); idleDeadline = GetTickCount64() + 30000; }
            return result;
        } catch (...) { track(nullptr); throw; }
    }
    void watch() noexcept
    {
        try {
            // Timer cancellation prevents future scheduling, but an already
            // running callback must leave Cancel() before worker teardown.
            std::lock_guard watchLock(watcherMutex);
            if (watcherStopped) return;
            IAsyncInfo pending{nullptr};
            {
                std::lock_guard lock(mutex);
                const auto now = GetTickCount64();
                if (!deadline || (now < deadline && now < idleDeadline)) return;
                timedOut = true; pending = operation;
            }
            if (pending) pending.Cancel();
        } catch (...) {}
    }
    std::vector<std::uint8_t> fetch(HttpClient& client, const std::string& canonicalText,
        std::uint64_t expected, const std::string& expectedHash, std::uint64_t limit, StagedFile* file = nullptr)
    {
        check();
        const Uri canonical(wide(canonicalText));
        Uri test{nullptr};
        if (options.loopbackTestOrigin) {
            test = Uri(*options.loopbackTestOrigin);
            if (!validTestOrigin(test)) throw Failure{Error::Metadata};
        }
        auto requestUri = test ? test.CombineUri(canonical.Path() + canonical.Query()) : canonical;
        {
            std::lock_guard lock(mutex);
            const auto now = GetTickCount64();
            deadline = now + (file ? 60ull * 60 * 1000 : 30000); idleDeadline = now + 30000; timedOut = false;
        }
        for (unsigned redirects = 0;; ++redirects) {
            check(); failure = Error::Network;
            HttpRequestMessage request(HttpMethod::Get(), requestUri);
            request.Headers().TryAppendWithoutValidation(L"Accept-Encoding", L"identity");
            request.Headers().TryAppendWithoutValidation(L"Accept", canonicalText == utf8(Brand::latestReleaseApiUrl) ?
                L"application/vnd.github+json" : L"application/octet-stream");
            ResponseScope response;
            response.response = waitResult(client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead));
            const auto statusCode = static_cast<unsigned>(response.response.StatusCode());
            if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
                failure = Error::Metadata;
                if (redirects >= 5 || !response.response.Headers().HasKey(L"Location")) throw Failure{failure};
                const auto next = requestUri.CombineUri(response.response.Headers().Lookup(L"Location"));
                if (!allowedRedirect(next, canonical, test)) throw Failure{failure};
                requestUri = next; continue;
            }
            if (statusCode != 200 || !response.response.Content()) throw Failure{Error::Network};
            failure = Error::Integrity;
            const auto headers = response.response.Content().Headers();
            if (headers.HasKey(L"Content-Encoding")) {
                auto encoding = utf8(headers.Lookup(L"Content-Encoding"));
                std::transform(encoding.begin(), encoding.end(), encoding.begin(), [](unsigned char c) {
                    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                });
                if (!encoding.empty() && encoding != "identity") throw Failure{failure};
            }
            if (headers.HasKey(L"Content-Length")) {
                const auto text = utf8(headers.Lookup(L"Content-Length"));
                std::uint64_t declared{};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), declared);
                if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                    declared > limit || (expected && declared != expected)) throw Failure{failure};
            }
            failure = Error::Network;
            response.stream = waitResult(response.response.Content().ReadAsInputStreamAsync());
            const auto hash = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256()).CreateHash();
            Buffer buffer(64 * 1024);
            std::vector<std::uint8_t> bytes;
            if (!file && expected) bytes.reserve(static_cast<std::size_t>(expected));
            std::uint64_t received{}, lastProgress{};
            for (;;) {
                check(); buffer.Length(0);
                const auto chunk = waitResult(response.stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial));
                const auto amount = chunk.Length();
                if (!amount) break;
                if (amount > limit - received || (expected && amount > expected - received)) throw Failure{Error::Integrity};
                if (file) {
                    if (!file->writer || _write(file->writer->get(), chunk.data(), amount) != static_cast<int>(amount))
                        throw Failure{Error::Save};
                } else bytes.insert(bytes.end(), chunk.data(), chunk.data() + amount);
                hash.Append(chunk); received += amount;
                if (file) {
                    status.received = received;
                    const auto now = GetTickCount64();
                    if (!lastProgress || now - lastProgress >= 100) { lastProgress = now; notify(); }
                }
            }
            if ((expected && received != expected) || (!expectedHash.empty() &&
                !CryptographicBuffer::Compare(hash.GetValueAndReset(), CryptographicBuffer::DecodeFromHexString(wide(expectedHash)))))
                throw Failure{Error::Integrity};
            { std::lock_guard lock(mutex); deadline = idleDeadline = 0; }
            return bytes;
        }
    }
    Status perform()
    {
        failure = Error::Metadata;
        if (!Core::canonicalUpdateVersion(options.currentVersion)) throw Failure{Error::Metadata};
        if (options.loopbackTestOrigin && !validTestOrigin(Uri(*options.loopbackTestOrigin))) throw Failure{Error::Metadata};
        HttpBaseProtocolFilter filter;
        filter.AllowAutoRedirect(false); filter.AutomaticDecompression(false); filter.AllowUI(false);
        filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
        filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
        ClientScope client{HttpClient(filter)};
        client.client.DefaultRequestHeaders().TryAppendWithoutValidation(L"User-Agent", Brand::name);
        const auto product = identity();
        Policy::Release release;
        auto bytes = fetch(client.client, utf8(Brand::latestReleaseApiUrl), 0, {}, Core::updateMetadataLimit);
        failure = Error::Metadata;
        if (!Policy::parseRelease(document(bytes, Core::updateMetadataLimit), product, release, Policy::ReleaseMode::LegacyBridge))
            throw Failure{Error::Metadata};
        Policy::Asset package;
        const auto manifest = release.assets.find("release-manifest.json");
        if (manifest == release.assets.end()) {
            if (!Policy::selectLegacyExecutable(product, release, package)) throw Failure{Error::Metadata};
        } else {
            bytes = fetch(client.client, manifest->second.url, manifest->second.size, manifest->second.sha256, Core::updateMetadataLimit);
            failure = Error::Metadata;
            Policy::Selection selection;
            // A standalone old installation is portable. Do not silently turn
            // it into a managed installer installation or download a bare Qt EXE.
            if (!Policy::selectPackage(document(bytes, Core::updateMetadataLimit), product, release,
                Kind::WindowsPortableZip, selection)) throw Failure{Error::Metadata};
            bytes = fetch(client.client, selection.buildInfo.url, selection.buildInfo.size, selection.buildInfo.sha256, Core::updateBuildInfoLimit);
            failure = Error::Metadata;
            if (!Policy::validateBuildInfo(document(bytes, Core::updateBuildInfoLimit), product, selection)) throw Failure{Error::Metadata};
            package = selection.package; status.packageKind = selection.kind; status.minimumSystem = wide(selection.minimumSystem);
        }
        status.version = wide(release.version); status.total = package.size;
        if (!options.download) {
            const auto local = Core::parseReleaseVersion(options.currentVersion), offered = Core::parseReleaseVersion(status.version);
            status.stage = local && offered && *offered > *local ? Stage::Available : Stage::Idle;
            return status;
        }
        check();
        const auto directory = downloadsDirectory(options.downloadsDirectory);
        StagedFile file; file.create(directory);
        status.stage = Stage::Downloading; notify();
        fetch(client.client, package.url, package.size, package.sha256,
            status.packageKind ? Core::updatePackagePolicy(*status.packageKind).maximumBytes : Policy::legacyExecutableLimit, &file);
        check(); file.finish(package.size, status.packageKind, release.version);
        const auto name = status.packageKind ? wide(package.name) :
            std::wstring(Brand::name) + L"-" + wide(release.tag) + L"-windows-x64.exe";
        const auto dot = name.find_last_of(L'.');
        for (unsigned collision = 0; collision < 10000; ++collision) {
            check();
            const auto candidate = collision ? name.substr(0, dot) + std::format(L"-{:03}", collision) + name.substr(dot) : name;
            const auto committed = commitFileNoReplace(file.path, directory / candidate);
            if (committed.committed()) {
                status.file = committed.destination; status.received = package.size;
                status.stage = committed.durable() ? Stage::Downloaded : Stage::Failed;
                status.error = committed.durable() ? Error::None : Error::DurabilityUnknown;
                // Publication transfers the final name to the user. Cleanup
                // can only inspect/remove the still-owned staging identity.
                file.cleanup(); return status;
            }
            if (committed.error != std::errc::file_exists) throw Failure{Error::Save};
        }
        throw Failure{Error::Save};
    }
    void run() noexcept
    {
        bool initialized{};
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded); initialized = true;
            {
                const std::weak_ptr<State> weak = shared_from_this();
                TimerScope watchdog{ThreadPoolTimer::CreatePeriodicTimer([weak](const ThreadPoolTimer&) {
                    if (const auto state = weak.lock()) state->watch();
                }, std::chrono::milliseconds(250))};
                try { status = perform(); }
                catch (const Failure& error) {
                    status.stage = cancelled.load(std::memory_order_acquire) ? Stage::Cancelled : Stage::Failed;
                    status.error = status.stage == Stage::Cancelled ? Error::None : error.error;
                } catch (const winrt::hresult_error& error) {
                    status.stage = cancelled.load(std::memory_order_acquire) ? Stage::Cancelled : Stage::Failed;
                    status.error = status.stage == Stage::Cancelled ? Error::None : failure;
                    status.nativeError = error.code().value;
                } catch (...) {
                    status.stage = cancelled.load(std::memory_order_acquire) ? Stage::Cancelled : Stage::Failed;
                    status.error = status.stage == Stage::Cancelled ? Error::None : failure;
                }
                watchdog.stop();
                {
                    std::lock_guard watchLock(watcherMutex);
                    watcherStopped = true;
                    std::lock_guard lock(mutex); operation = nullptr; deadline = idleDeadline = 0;
                }
            }
            // File/network scopes are already destroyed; a completion observer
            // never sees a terminal status while its temporary writer is live.
            notify();
        } catch (...) {
            status.stage = cancelled.load(std::memory_order_acquire) ? Stage::Cancelled : Stage::Failed;
            status.error = status.stage == Stage::Cancelled ? Error::None : Error::Network;
            notify();
        }
        if (initialized) winrt::uninit_apartment();
        done.store(true, std::memory_order_release);
    }
};

Task::Task(Options options, Observer observer) : state_(std::make_shared<State>(std::move(options), std::move(observer))),
    worker_([state = state_] { state->run(); }) {}
Task::~Task() { cancel(); join(); }
void Task::cancel() noexcept
{ state_->cancelled.store(true, std::memory_order_release); state_->cancelPending(); }
bool Task::finished() const noexcept { return state_->done.load(std::memory_order_acquire); }
void Task::join() { if (worker_.joinable()) worker_.join(); }

} // namespace HuhuCapture::Platform::Windows::LegacyUpdates
