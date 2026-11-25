#include "services/update/WindowsUpdateDownload.h"
#include "app/Brand.h"
#include "core/ReleasePackage.h"
#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>
#include <shlobj.h>
#include <io.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace HuhuCapture::Updates::WindowsBridge {
namespace {
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Security::Cryptography::Core;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Filters;
using winrt::Windows::System::Threading::ThreadPoolTimer;
namespace Contract = Core::ReleasePackage;
namespace fs = std::filesystem;

struct Failure { Error error; };
void require(bool condition, Error error = Error::Metadata) { if (!condition) throw Failure{error}; }

IJsonValue field(const JsonObject& object, std::wstring_view key)
{ return object.GetNamedValue(winrt::hstring{key}, JsonValue::CreateNullValue()); }
JsonObject object(const IJsonValue& value)
{ require(value.ValueType() == JsonValueType::Object); return value.GetObject(); }
std::wstring string(const JsonObject& object, std::wstring_view key)
{
    const auto value = field(object, key);
    require(value.ValueType() == JsonValueType::String);
    return std::wstring{value.GetString()};
}
bool bounded(std::wstring_view value, std::size_t maximum)
{ return !value.empty() && value.find(L'\0') == value.npos && winrt::to_string(value).size() <= maximum; }
JsonArray array(const JsonObject& object, std::wstring_view key, std::uint32_t maximum)
{
    const auto value = field(object, key);
    require(value.ValueType() == JsonValueType::Array);
    const auto result = value.GetArray();
    require(result.Size() && result.Size() <= maximum);
    return result;
}
std::uint64_t bytes(const JsonObject& object, std::wstring_view key, std::uint64_t maximum, bool empty = false)
{
    const auto value = field(object, key);
    std::uint64_t result{};
    require(value.ValueType() == JsonValueType::Number && Contract::byteCount(value.GetNumber(), maximum, result, empty));
    return result;
}
void falseBoolean(const JsonObject& object, std::wstring_view key, bool optional = false)
{
    if (optional && !object.HasKey(winrt::hstring{key})) return;
    const auto value = field(object, key);
    require(value.ValueType() == JsonValueType::Boolean && !value.GetBoolean());
}
void identity(const JsonObject& object, std::wstring_view tag, std::wstring_view version, std::wstring_view commit)
{
    falseBoolean(object, L"developmentOnly", true);
    falseBoolean(object, L"sourceDirty", true);
    require(bytes(object, L"schemaVersion", 1) == 1 && string(object, L"product") == Brand::name &&
        string(object, L"repository") == Brand::repositoryLabel && string(object, L"tag") == tag &&
        string(object, L"version") == version && string(object, L"sourceCommit") == commit && Contract::lowerHex(commit, 40));
}
std::wstring assetUrl(std::wstring_view tag, std::wstring_view name)
{ return std::wstring{Brand::releaseAssetPrefix} + std::wstring{tag} + L"/" + std::wstring{name}; }
std::wstring kindName(PackageKind kind)
{ return kind == PackageKind::WindowsInstaller ? L"installer" : L"portable-zip"; }
std::wstring packageName(std::wstring_view tag, PackageKind kind)
{ return std::wstring{Brand::name} + L"-" + std::wstring{tag} +
    (kind == PackageKind::WindowsInstaller ? L"-windows-x64-setup.exe" : L"-windows-x64.zip"); }
std::uint64_t budget(PackageKind kind)
{
    if (kind == PackageKind::LegacyExecutable) return Contract::legacyBudget;
    return kind == PackageKind::WindowsInstaller ? Contract::installerBudget : Contract::archiveBudget;
}
bool safeUri(const Uri& uri)
{ return uri.UserName().empty() && uri.Password().empty() && uri.Fragment().empty(); }
bool validFixture(const LoopbackFixture& fixture)
{
    const Uri uri{fixture.origin};
    return safeUri(uri) && uri.SchemeName() == L"http" && uri.Port() > 0 &&
        uri.Query().empty() && uri.Path() == L"/" &&
        (uri.Host() == L"127.0.0.1" || uri.Host() == L"[::1]" || uri.Host() == L"::1") &&
        fixture.downloadsDirectory.is_absolute();
}
Uri requestUri(std::wstring_view canonical, const std::optional<LoopbackFixture>& fixture)
{
    if (!fixture) return Uri{canonical};
    const Uri url{canonical};
    return Uri{fixture->origin, url.Path() + url.Query()};
}
bool allowedRedirect(const Uri& next, std::wstring_view canonical, const std::optional<LoopbackFixture>& fixture)
{
    if (!safeUri(next)) return false;
    if (fixture) {
        const Uri origin{fixture->origin};
        return next.SchemeName() == origin.SchemeName() && next.Host() == origin.Host() && next.Port() == origin.Port();
    }
    if (next.SchemeName() != L"https" || next.Port() != 443) return false;
    if (canonical == Brand::latestReleaseApiUrl || next.Host() == L"github.com") return next.AbsoluteUri() == canonical;
    return next.Host() == L"release-assets.githubusercontent.com" || next.Host() == L"objects.githubusercontent.com";
}

struct Asset { std::wstring name, sha256, url; std::uint64_t size{}; };
struct Release { std::wstring tag, version; std::map<std::wstring, Asset> assets; };
struct Selection { Asset package, buildInfo; std::wstring commit; PackageKind kind{PackageKind::LegacyExecutable}; };

Release release(const JsonObject& json)
{
    falseBoolean(json, L"draft"); falseBoolean(json, L"prerelease");
    Release result;
    result.tag = string(json, L"tag_name");
    require(Contract::canonicalVersion(result.tag, true) &&
        string(json, L"html_url") == std::wstring{Brand::releasesUrl} + L"/tag/" + result.tag);
    result.version = result.tag.substr(1);
    for (const auto& value : array(json, L"assets", 128)) {
        const auto entry = object(value);
        Asset asset;
        asset.name = string(entry, L"name");
        require(Contract::assetName(asset.name));
        const auto digest = string(entry, L"digest");
        require(digest.starts_with(L"sha256:") && Contract::lowerHex(std::wstring_view{digest}.substr(7), 64));
        asset.sha256 = digest.substr(7);
        asset.size = bytes(entry, L"size", Contract::archiveBudget);
        asset.url = string(entry, L"browser_download_url");
        require(asset.url == assetUrl(result.tag, asset.name));
        const auto name = asset.name;
        require(result.assets.emplace(name, std::move(asset)).second);
    }
    return result;
}
Asset descriptor(const JsonObject& json, const Release& release, std::uint64_t maximum)
{
    const auto name = string(json, L"name");
    const auto found = release.assets.find(name);
    require(found != release.assets.end() && found->second.sha256 == string(json, L"sha256") &&
        found->second.size == bytes(json, L"size", maximum));
    return found->second;
}
Selection selection(const JsonObject& json, const Release& release)
{
    const auto commit = string(json, L"sourceCommit");
    identity(json, release.tag, release.version, commit);
    std::optional<Selection> installer, portable;
    for (const auto& value : array(json, L"assets", 16)) {
        const auto entry = object(value);
        if (string(entry, L"platform") != L"windows" || string(entry, L"architecture") != L"x64") continue;
        const auto kind = string(entry, L"packageKind");
        if (kind != L"installer" && kind != L"portable-zip") continue;
        auto& chosen = kind == L"installer" ? installer : portable;
        require(!chosen && bounded(string(entry, L"minimumSystem"), 256));
        Selection candidate;
        candidate.kind = kind == L"installer" ? PackageKind::WindowsInstaller : PackageKind::WindowsPortableZip;
        candidate.commit = commit;
        candidate.package = descriptor(entry, release, budget(candidate.kind));
        candidate.buildInfo = descriptor(object(field(entry, L"buildInfo")), release, Contract::buildInfoBudget);
        require(candidate.package.name == packageName(release.tag, candidate.kind) &&
            candidate.buildInfo.name == L"build-info-windows-x64.json");
        chosen = std::move(candidate);
    }
    require(installer.has_value() || portable.has_value());
    return installer ? *installer : *portable;
}
struct WindowsPathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
void buildInfo(const JsonObject& json, const Release& release, const Selection& selected)
{
    identity(json, release.tag, release.version, selected.commit);
    require(string(json, L"platform") == L"windows" && string(json, L"architecture") == L"x64");
    std::set<std::wstring> dependencies;
    for (const auto& value : array(json, L"dependencies", 128)) {
        const auto entry = object(value);
        const auto name = string(entry, L"name"), source = string(entry, L"source");
        require(Contract::assetName(name) && dependencies.insert(name).second &&
            bounded(string(entry, L"version"), 128) && bounded(string(entry, L"license"), 1024) && bounded(source, 2048));
        const Uri uri{source};
        require(safeUri(uri) && uri.SchemeName() == L"https" && !uri.Host().empty());
    }
    for (const auto* name : {L"qt", L"gifski", L"localsend", L"ffmpeg"}) require(dependencies.contains(name));
    unsigned matches{};
    std::wstring contentRoot;
    std::set<std::wstring> packages;
    for (const auto& value : array(json, L"packages", 16)) {
        const auto entry = object(value);
        const auto name = string(entry, L"name");
        require(Contract::assetName(name) && packages.insert(name).second);
        if (name != selected.package.name) continue;
        require(string(entry, L"packageKind") == kindName(selected.kind) &&
            bytes(entry, L"size", budget(selected.kind)) == selected.package.size && string(entry, L"sha256") == selected.package.sha256);
        if (entry.HasKey(L"contentRoot")) {
            contentRoot = string(entry, L"contentRoot");
            require(bounded(contentRoot, 4096) && Contract::relativeFile(contentRoot));
            contentRoot += L'/';
        }
        ++matches;
    }
    require(matches == 1);
    bool executable{}, qt{}, platform{};
    std::set<std::wstring, WindowsPathLess> paths;
    for (const auto& value : array(json, L"files", 8192)) {
        const auto entry = object(value);
        const auto path = string(entry, L"path");
        const auto length = bytes(entry, L"size", Contract::archiveBudget, true);
        require(bounded(path, 4096) && Contract::relativeFile(path) && paths.insert(path).second &&
            Contract::lowerHex(string(entry, L"sha256"), 64));
        if (!length || !path.starts_with(contentRoot)) continue;
        const auto leaf = path.substr(path.find_last_of(L'/') + 1);
        executable = executable || leaf == L"huhucapture.exe";
        qt = qt || leaf == L"Qt6Core.dll";
        platform = platform || (leaf == L"qwindows.dll" &&
            (path.starts_with(L"platforms/") || path.find(L"/platforms/") != path.npos));
    }
    require(executable && qt && platform);
}

fs::path downloadsDirectory()
{
    PWSTR raw{};
    const auto result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &raw);
    fs::path path;
    if (SUCCEEDED(result) && raw) path = raw;
    if (raw) CoTaskMemFree(raw);
    require(!path.empty(), Error::Folder);
    return path;
}

struct StagedFile {
    fs::path path;
    std::optional<Platform::FileIdentity> identity;
    std::unique_ptr<Platform::FileHandle> handle;
    explicit StagedFile(const fs::path& directory) {
        std::error_code error;
        path = Platform::createExclusiveTemporaryFile(directory, error);
        require(!path.empty(), Error::Save);
        // Construct before initialize() so a later open failure still reaches
        // identity-aware cleanup; no destructor removes an unobserved identity.
    }
    void initialize() {
        std::error_code error;
        const auto snapshot = Platform::readFileSnapshot(path, error);
        require(snapshot.has_value() && !snapshot->bytes, Error::Save);
        identity = snapshot->identity;
        handle = std::make_unique<Platform::FileHandle>(Platform::openOwnedEmptyFileForWrite(path, *identity, error));
        require(static_cast<bool>(*handle), Error::Save);
    }
    void write(const std::vector<std::uint8_t>& buffer) {
        const auto written = _write(handle->get(), buffer.data(), static_cast<unsigned>(buffer.size()));
        require(written >= 0 && static_cast<std::size_t>(written) == buffer.size(), Error::Save);
    }
    void close() {
        if (!handle) return;
        std::error_code error;
        const bool ok = handle->close(error);
        handle.reset();
        require(ok, Error::Save);
    }
    void verifyWritten(std::uint64_t expected) const {
        std::error_code error;
        const auto opened = Platform::readFileDescriptorSnapshot(handle->get(), error);
        const auto named = Platform::readFileSnapshot(path, error);
        require(opened && named && *opened == *named && opened->bytes == expected &&
            identity && opened->identity == *identity, Error::Save);
    }
    bool owns() const {
        std::error_code error;
        const auto snapshot = Platform::readFileSnapshot(path, error);
        return identity && snapshot && snapshot->identity == *identity;
    }
    fs::path cleanup() noexcept {
        handle.reset();
        if (path.empty()) return {};
        try {
            std::error_code error;
            if (owns() && fs::remove(path, error)) { path.clear(); return {}; }
            if (!fs::exists(path, error) && !error) { path.clear(); return {}; }
        } catch (...) {}
        return path;
    }
    ~StagedFile() { cleanup(); }
};

void executableVersion(const fs::path& path, std::wstring_view version, PackageKind kind)
{
    std::error_code error;
    auto handle = Platform::openRegularFileReadOnly(path, error);
    require(static_cast<bool>(handle), Error::Integrity);
    std::array<unsigned char, 64> dos{};
    require(_read(handle.get(), dos.data(), static_cast<unsigned>(dos.size())) == dos.size() &&
        dos[0] == 'M' && dos[1] == 'Z', Error::Integrity);
    const auto offset = static_cast<std::uint32_t>(dos[60]) | static_cast<std::uint32_t>(dos[61]) << 8 |
        static_cast<std::uint32_t>(dos[62]) << 16 | static_cast<std::uint32_t>(dos[63]) << 24;
    require(offset >= 64 && offset <= 1024 * 1024 && _lseeki64(handle.get(), offset, SEEK_SET) == offset, Error::Integrity);
    std::array<unsigned char, 26> pe{};
    require(_read(handle.get(), pe.data(), static_cast<unsigned>(pe.size())) == pe.size() &&
        pe[0] == 'P' && pe[1] == 'E' && !pe[2] && !pe[3], Error::Integrity);
    const auto machine = pe[4] | pe[5] << 8, format = pe[24] | pe[25] << 8;
    const auto characteristics = pe[22] | pe[23] << 8;
    require((characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) && !(characteristics & IMAGE_FILE_DLL), Error::Integrity);
    // A genuine NSIS installer uses an x86 bootstrap to install the x64 payload.
    // Legacy bare applications must themselves be x64. The complete package's
    // payload architecture is independently bound by the verified build-info.
    require((machine == 0x8664 && format == 0x20b) ||
        (kind == PackageKind::WindowsInstaller && machine == 0x14c && format == 0x10b), Error::Integrity);
    const auto length = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    require(length && length <= 16 * 1024 * 1024, Error::Integrity);
    std::vector<unsigned char> data(length);
    require(GetFileVersionInfoW(path.c_str(), 0, length, data.data()) != FALSE, Error::Integrity);
    VS_FIXEDFILEINFO* info{}; UINT size{};
    require(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size) != FALSE &&
        size >= sizeof(VS_FIXEDFILEINFO) && info && info->dwSignature == 0xfeef04bd, Error::Integrity);
    const std::array<int, 3> actual{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS)};
    require(Core::parseReleaseVersion(version) == actual, Error::Integrity);
}

struct OperationTimer {
    ThreadPoolTimer timer{nullptr};
    ~OperationTimer() { try { if (timer) timer.Cancel(); } catch (...) {} }
};
struct ClientLifetime {
    HttpClient value{nullptr};
    ~ClientLifetime() { try { if (value) value.Close(); } catch (...) {} }
};
struct TransferLifetime {
    HttpResponseMessage response{nullptr};
    IInputStream stream{nullptr};
    ~TransferLifetime() {
        try { if (stream) stream.Close(); } catch (...) {}
        try { if (response) response.Close(); } catch (...) {}
    }
};
} // namespace

struct Download::State {
    Options options;
    std::optional<LoopbackFixture> fixture;
    std::function<void(Snapshot)> sink;
    std::atomic<bool> cancelled{};
    std::mutex mutex;
    std::condition_variable completion;
    bool done{};
    IAsyncInfo operation{nullptr};
    std::thread worker;

    void check() const { if (cancelled) throw Failure{Error::None}; }
    void cancel() noexcept {
        cancelled = true;
        try {
            IAsyncInfo pending{nullptr};
            { std::lock_guard lock(mutex); pending = operation; }
            if (pending) pending.Cancel();
        } catch (...) {}
    }
    void emit(Snapshot snapshot) noexcept {
        try { if (sink) sink(std::move(snapshot)); } catch (...) {}
    }
    template<class Operation> auto await(const Operation& pending, std::chrono::steady_clock::time_point deadline) {
        { std::lock_guard lock(mutex); operation = pending; }
        if (cancelled) { pending.Cancel(); check(); }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) { pending.Cancel(); throw Failure{Error::Network}; }
        const auto timeout = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(remaining), std::chrono::milliseconds{30000});
        OperationTimer timer{ThreadPoolTimer::CreateTimer([pending](const ThreadPoolTimer&) {
            try { pending.Cancel(); } catch (...) {}
        }, timeout)};
        auto result = pending.get();
        { std::lock_guard lock(mutex); operation = nullptr; }
        check();
        return result;
    }

    void receive(const HttpClient& client, std::wstring_view canonical, std::uint64_t maximum, const Asset* expected,
        const std::function<void(const std::vector<std::uint8_t>&, std::uint64_t)>& consume)
    {
        check();
        const auto deadline = std::chrono::steady_clock::now() + (maximum > Contract::buildInfoBudget ?
            std::chrono::seconds{600} : std::chrono::seconds{30});
        auto uri = requestUri(canonical, fixture);
        TransferLifetime transfer;
        auto& response = transfer.response;
        for (unsigned redirects = 0;; ++redirects) {
            HttpRequestMessage request{HttpMethod::Get(), uri};
            response = await(client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead), deadline);
            const auto status = static_cast<unsigned>(response.StatusCode());
            if (status < 300 || status >= 400) break;
            require(redirects < 5 && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308), Error::Network);
            // Read the actual header before constructing an absolute WinRT Uri;
            // a valid HTTP Location may be relative to this response's URL.
            require(response.Headers().HasKey(L"Location"), Error::Network);
            const auto location = response.Headers().Lookup(L"Location");
            require(!location.empty() && location.size() <= 8192, Error::Network);
            const Uri next{uri.AbsoluteUri(), location};
            require(allowedRedirect(next, canonical, fixture), Error::Network);
            response.Close(); uri = next;
        }
        require(response.StatusCode() == HttpStatusCode::Ok, Error::Network);
        const auto headers = response.Content().Headers();
        require(headers.ContentEncoding().Size() == 0, Error::Integrity);
        const auto declared = headers.ContentLength();
        if (declared) require(declared.Value() <= maximum && (!expected || declared.Value() == expected->size), Error::Integrity);
        auto& stream = transfer.stream;
        stream = await(response.Content().ReadAsInputStreamAsync(), deadline);
        const auto hash = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256()).CreateHash();
        std::uint64_t received{};
        while (true) {
            check();
            const auto buffer = await(stream.ReadAsync(Buffer{64 * 1024}, 64 * 1024, InputStreamOptions::Partial), deadline);
            const auto count = buffer.Length();
            if (!count) break;
            require(count <= maximum - received && (!expected || count <= expected->size - received), Error::Integrity);
            received += count;
            hash.Append(buffer);
            std::vector<std::uint8_t> data(count);
            DataReader::FromBuffer(buffer).ReadBytes(data);
            consume(data, received);
        }
        stream.Close(); response.Close();
        require(received && (!declared || received == declared.Value()) && (!expected || received == expected->size), Error::Integrity);
        if (expected) require(CryptographicBuffer::EncodeToHexString(hash.GetValueAndReset()) == expected->sha256, Error::Integrity);
    }

    JsonObject metadata(const HttpClient& client, std::wstring_view url, std::uint64_t maximum, const Asset* expected = nullptr)
    {
        std::vector<std::uint8_t> data;
        receive(client, url, maximum, expected, [&](const auto& chunk, std::uint64_t) { data.insert(data.end(), chunk.begin(), chunk.end()); });
        JsonObject result{nullptr};
        const auto text = CryptographicBuffer::ConvertBinaryToString(BinaryStringEncoding::Utf8, CryptographicBuffer::CreateFromByteArray(data));
        require(JsonObject::TryParse(text, result));
        return result;
    }

    void run() noexcept {
        Snapshot result;
        std::unique_ptr<StagedFile> staged;
        Error failure = Error::Network;
        bool apartment{};
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment = true;
            if (fixture) require(validFixture(*fixture));
            check();
            emit({Stage::Checking});
            HttpBaseProtocolFilter filter;
            filter.AllowAutoRedirect(false); filter.AutomaticDecompression(false);
            filter.AllowUI(false); filter.UseProxy(!fixture.has_value());
            filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
            filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
            ClientLifetime clientOwner{HttpClient{filter}};
            const auto& client = clientOwner.value;
            client.DefaultRequestHeaders().TryAppendWithoutValidation(L"User-Agent", Brand::name);
            client.DefaultRequestHeaders().TryAppendWithoutValidation(L"Accept", L"application/vnd.github+json");
            client.DefaultRequestHeaders().TryAppendWithoutValidation(L"Accept-Encoding", L"identity");
            const auto json = metadata(client, Brand::latestReleaseApiUrl, Contract::metadataBudget);
            failure = Error::Metadata;
            const auto latest = release(json);
            result.version = latest.version;
            Selection selected;
            if (const auto manifest = latest.assets.find(L"release-manifest.json"); manifest != latest.assets.end()) {
                require(manifest->second.size <= Contract::metadataBudget);
                failure = Error::Network;
                const auto data = metadata(client, manifest->second.url, Contract::metadataBudget, &manifest->second);
                failure = Error::Metadata;
                selected = selection(data, latest);
                failure = Error::Network;
                const auto info = metadata(client, selected.buildInfo.url, Contract::buildInfoBudget, &selected.buildInfo);
                failure = Error::Metadata;
                buildInfo(info, latest, selected);
            } else {
                // Only absence permits the pre-Qt EXE fallback. An advertised
                // but malformed, missing or invalid manifest always fails.
                const auto legacy = latest.assets.find(std::wstring{Brand::executableName});
                require(legacy != latest.assets.end() && legacy->second.size >= 2 && legacy->second.size <= Contract::legacyBudget);
                selected.package = legacy->second;
            }
            result.packageKind = selected.kind;
            result.total = selected.package.size;
            if (!options.download) {
                result.stage = *Core::parseReleaseVersion(latest.version) > options.currentVersion ? Stage::Available : Stage::Idle;
            } else {
                failure = Error::Folder;
                const auto directory = fixture ? fixture->downloadsDirectory : downloadsDirectory();
                check();
                failure = Error::Save;
                staged = std::make_unique<StagedFile>(directory); staged->initialize();
                result.stage = Stage::Downloading; emit(result);
                std::vector<std::uint8_t> signature;
                unsigned reported{101};
                failure = Error::Network;
                receive(client, selected.package.url, budget(selected.kind), &selected.package,
                    [&](const auto& chunk, std::uint64_t received) {
                        staged->write(chunk);
                        const auto count = std::min<std::size_t>(4 - signature.size(), chunk.size());
                        signature.insert(signature.end(), chunk.begin(), chunk.begin() + count);
                        result.received = received;
                        const auto percent = static_cast<unsigned>(received * 100 / result.total);
                        if (percent != reported) { reported = percent; emit(result); }
                    });
                failure = Error::Integrity;
                if (selected.kind == PackageKind::WindowsPortableZip)
                    require(signature == std::vector<std::uint8_t>{'P', 'K', 3, 4}, Error::Integrity);
                staged->verifyWritten(selected.package.size);
                staged->close();
                require(staged->owns(), Error::Save);
                if (selected.kind != PackageKind::WindowsPortableZip) executableVersion(staged->path, latest.version, selected.kind);
                failure = Error::Save;
                const auto name = selected.kind == PackageKind::LegacyExecutable ?
                    std::wstring{Brand::name} + L"-" + latest.tag + L"-windows-x64.exe" : selected.package.name;
                for (unsigned collision = 0; collision < 10000; ++collision) {
                    check();
                    require(staged->owns(), Error::Save);
                    const fs::path leaf{name};
                    const auto filename = collision ? leaf.stem().wstring() + std::format(L"-{:03}", collision) + leaf.extension().wstring() : name;
                    const auto committed = Platform::commitFileNoReplace(staged->path, directory / filename);
                    if (committed.committed()) {
                        result.file = committed.destination;
                        staged->path.clear();
                        if (!committed.durable()) throw Failure{Error::DurabilityUnknown};
                        result.stage = Stage::Downloaded;
                        break;
                    }
                    require(committed.error == std::errc::file_exists, Error::Save);
                }
                require(result.stage == Stage::Downloaded, Error::Save);
            }
            client.Close();
        } catch (const Failure& error) {
            result.stage = cancelled ? Stage::Cancelled : Stage::Failed;
            result.error = error.error;
        } catch (...) {
            result.stage = cancelled ? Stage::Cancelled : Stage::Failed;
            result.error = failure;
        }
        if (staged) {
            const auto retained = staged->cleanup();
            if (result.file.empty() && !retained.empty()) result.file = retained;
        }
        staged.reset();
        { std::lock_guard lock(mutex); operation = nullptr; }
        emit(std::move(result));
        if (apartment) winrt::uninit_apartment();
        { std::lock_guard lock(mutex); done = true; }
        completion.notify_all();
    }
};

Download::Download(Options options, std::function<void(Snapshot)> sink, std::optional<LoopbackFixture> fixture)
    : state_(std::make_unique<State>())
{
    state_->options = options; state_->sink = std::move(sink); state_->fixture = std::move(fixture);
    state_->worker = std::thread{[state = state_.get()] { state->run(); }};
}
Download::~Download()
{
    cancel();
    if (state_->worker.joinable()) state_->worker.join();
}
void Download::cancel() noexcept { state_->cancel(); }
bool Download::waitFor(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_->mutex);
    return state_->completion.wait_for(lock, timeout, [&] { return state_->done; });
}

} // namespace HuhuCapture::Updates::WindowsBridge
