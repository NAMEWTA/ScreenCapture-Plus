#include "services/desktop/FileRevealer.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#include "platform/windows/ComApartment.h"
#include <Windows.h>
#include <Exdisp.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <ShlGuid.h>
#include <ServProv.h>
#include <wrl/client.h>
#endif

using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void diagnose(const char* operation, const Desktop::RevealResult& result)
{
    std::cout << "RESULT " << operation << " error=" << static_cast<int>(result.failure.code)
        << " native=" << result.failure.nativeCode << " (0x" << std::hex
        << static_cast<std::uint64_t>(result.failure.nativeCode) << std::dec << ") detail="
        << result.failure.detail.toStdString() << " effect=" << static_cast<int>(result.effect)
        << " path=" << result.path.toStdString() << " exists=" << QFile::exists(result.path) << '\n';
}
void events() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
template<class Function> auto worker(Function function) { return std::async(std::launch::async, std::move(function)).get(); }
void waitFor(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 20000) { events(); QThread::msleep(2); }
    require(predicate(), message);
}

#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;

void requireShell(HRESULT result, const char* message)
{
    if (FAILED(result)) std::cerr << "SHELL " << message << " HRESULT=0x" << std::hex
        << static_cast<unsigned long>(result) << std::dec << '\n';
    require(SUCCEEDED(result), message);
}

struct ShellMemory {
    void operator()(void* address) const noexcept { CoTaskMemFree(address); }
};

QString normalizedPath(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}

bool samePath(const QString& first, const QString& second)
{
    return normalizedPath(first).compare(normalizedPath(second), Qt::CaseInsensitive) == 0;
}

struct ExplorerView {
    ComPtr<IWebBrowser2> browser;
    ComPtr<IFolderView2> folder;
    HWND window = nullptr;
    HWND view = nullptr;
    QString path;
};

// Use the actual displayed Shell view and PIDL, not a window title or a launch
// acknowledgement. This follows the public IShellWindows -> IShellBrowser ->
// IFolderView route described by Microsoft; no Shell implementation is copied.
ExplorerView explorerView(const ComPtr<IWebBrowser2>& browser)
{
    ExplorerView result;
    SHANDLE_PTR window = 0;
    if (FAILED(browser->get_HWND(&window))) return result;
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> shell;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder;
    ComPtr<IPersistFolder2> persistent;
    if (FAILED(browser.As(&provider)) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(shell.GetAddressOf()))) ||
        FAILED(shell->QueryActiveShellView(view.GetAddressOf())) ||
        FAILED(view.As(&folder)) ||
        FAILED(folder->GetFolder(IID_PPV_ARGS(persistent.GetAddressOf())))) return result;
    PIDLIST_ABSOLUTE rawId = nullptr;
    const auto folderStatus = persistent->GetCurFolder(&rawId);
    const std::unique_ptr<ITEMIDLIST, ShellMemory> id(rawId);
    if (FAILED(folderStatus) || !id) return result;
    PWSTR rawName = nullptr;
    const auto pathStatus = SHGetNameFromIDList(id.get(), SIGDN_FILESYSPATH, &rawName);
    const std::unique_ptr<wchar_t, ShellMemory> name(rawName);
    if (FAILED(pathStatus) || !name || FAILED(view->GetWindow(&result.view))) return {};
    result.browser = browser;
    result.folder = std::move(folder);
    result.window = reinterpret_cast<HWND>(window);
    result.path = QString::fromWCharArray(name.get());
    return result;
}

QStringList selectedPaths(IFolderView2* folder)
{
    ComPtr<IShellItemArray> selection;
    if (FAILED(folder->GetSelection(FALSE, selection.GetAddressOf())) || !selection) return {};
    DWORD count = 0;
    if (FAILED(selection->GetCount(&count))) return {};
    QStringList paths;
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        if (FAILED(selection->GetItemAt(index, item.GetAddressOf())) || !item) return {};
        PWSTR rawName = nullptr;
        const auto status = item->GetDisplayName(SIGDN_FILESYSPATH, &rawName);
        const std::unique_ptr<wchar_t, ShellMemory> name(rawName);
        if (FAILED(status) || !name) return {};
        paths.push_back(QString::fromWCharArray(name.get()));
    }
    return paths;
}

class ExplorerFixture final {
public:
    explicit ExplorerFixture(QTemporaryDir& files) : files_(files)
    {
        requireShell(apartment_.result(), "initialize the Explorer observer's real STA apartment");
        requireShell(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(windows_.GetAddressOf())), "connect to the real Shell window collection");
        for (const auto& browser : browsers()) {
            SHANDLE_PTR window = 0;
            if (SUCCEEDED(browser->get_HWND(&window))) originalWindows_.insert(reinterpret_cast<HWND>(window));
        }
    }

    ~ExplorerFixture()
    {
        if (!files_.autoRemove()) {
            // A failed or unobserved asynchronous Shell request can still arrive
            // later. Keep its real directory so it cannot create a delayed
            // "Location is not available" dialog in another native test.
            try {
                for (const auto& browser : browsers()) {
                    auto view = explorerView(browser);
                    if (view.path.isEmpty() || originalWindows_.contains(view.window)) continue;
                    for (const auto& path : requested_) {
                        if (samePath(view.path, path)) { close(std::move(view)); break; }
                    }
                }
            } catch (const std::exception& error) {
                std::cerr << "Explorer fixture cleanup: " << error.what() << '\n';
            }
            std::cerr << "RETAINED actual Shell fixture: " << files_.path().toStdString() << '\n';
        }
    }

    void begin(const QString& directory)
    {
        // Disable deletion before submitting, including when the API result or
        // the subsequent presentation/selection check fails.
        files_.setAutoRemove(false);
        requested_.push_back(directory);
        pending_.insert(normalizedPath(directory));
    }

    void displayedAndClosed(const QString& directory, const QString& selected = {})
    {
        ExplorerView found;
        QStringList lastSelection;
        try {
            waitFor([&] {
                for (const auto& browser : browsers()) {
                    auto view = explorerView(browser);
                    if (view.path.isEmpty() || !samePath(view.path, directory)) continue;
                    require(!originalWindows_.contains(view.window),
                        "the isolated fixture must not close a pre-existing Explorer window or tab");
                    if (!IsWindowVisible(view.window) || !IsWindowVisible(view.view) || IsIconic(view.window)) continue;
                    if (!selected.isEmpty()) {
                        lastSelection = selectedPaths(view.folder.Get());
                        if (lastSelection.size() != 1 || !samePath(lastSelection.front(), selected)) continue;
                    }
                    found = std::move(view);
                    return true;
                }
                return false;
            }, "Explorer actually displays the exact directory and requested file selection");
        } catch (...) {
            std::cerr << "Expected Explorer directory=" << directory.toStdString()
                << " selected=" << selected.toStdString()
                << " actualSelection=" << lastSelection.join(QStringLiteral(" | ")).toStdString() << '\n';
            describe();
            throw;
        }
        std::cout << "SHELL displayed hwnd=0x" << std::hex << reinterpret_cast<quintptr>(found.window) << std::dec
            << " path=" << found.path.toStdString() << " selected="
            << lastSelection.join(QStringLiteral(" | ")).toStdString() << '\n';
        close(std::move(found));
        pending_.erase(normalizedPath(directory));
    }

    void finish()
    {
        require(pending_.empty(), "every submitted Shell operation has been observed and closed");
        for (const auto& browser : browsers()) {
            const auto view = explorerView(browser);
            for (const auto& directory : requested_) {
                require(view.path.isEmpty() || !samePath(view.path, directory),
                    "no real Shell view still refers to a directory that the fixture will delete");
            }
        }
        files_.setAutoRemove(true);
    }

private:
    std::vector<ComPtr<IWebBrowser2>> browsers() const
    {
        long count = 0;
        requireShell(windows_->get_Count(&count), "enumerate the real registered Shell windows");
        std::vector<ComPtr<IWebBrowser2>> result;
        for (long index = 0; index < count; ++index) {
            VARIANT position{};
            position.vt = VT_I4;
            position.lVal = index;
            ComPtr<IDispatch> dispatch;
            ComPtr<IWebBrowser2> browser;
            if (windows_->Item(position, dispatch.GetAddressOf()) == S_OK && dispatch &&
                SUCCEEDED(dispatch.As(&browser))) result.push_back(std::move(browser));
        }
        return result;
    }

    void close(ExplorerView view)
    {
        const HWND window = view.window;
        const QString path = view.path;
        // Re-read the displayed folder immediately before the only mutation;
        // never send close to an arbitrary HWND or a window that navigated away.
        {
            const auto current = explorerView(view.browser);
            require(current.window == window && !originalWindows_.contains(window) && samePath(current.path, path),
                "only close the newly observed Explorer object still displaying this exact test directory");
        }
        requireShell(view.browser->Quit(), "the exact Explorer object accepts its own close request");
        view.folder.Reset();
        view.browser.Reset();
        waitFor([&] {
            if (IsWindow(window)) return false;
            for (const auto& browser : browsers()) {
                SHANDLE_PTR registered = 0;
                if (SUCCEEDED(browser->get_HWND(&registered)) && reinterpret_cast<HWND>(registered) == window) return false;
            }
            return true;
        }, "the observed Explorer window exits and unregisters before fixture deletion");
        std::cout << "SHELL closed hwnd=0x" << std::hex << reinterpret_cast<quintptr>(window) << std::dec
            << " path=" << path.toStdString() << '\n';
    }

    void describe() const
    {
        for (const auto& browser : browsers()) {
            const auto view = explorerView(browser);
            if (view.path.isEmpty()) continue;
            std::cerr << "SHELL observed hwnd=0x" << std::hex << reinterpret_cast<quintptr>(view.window) << std::dec
                << " original=" << originalWindows_.contains(view.window)
                << " visible=" << IsWindowVisible(view.window) << " path=" << view.path.toStdString() << '\n';
        }
    }

    QTemporaryDir& files_;
    Platform::ComApartment apartment_{COINIT_APARTMENTTHREADED};
    ComPtr<IShellWindows> windows_;
    std::set<HWND> originalWindows_;
    std::vector<QString> requested_;
    std::set<QString> pending_;
};
#endif
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir isolated; require(isolated.isValid(), "isolate real file-manager test files");
        QWidget origin; origin.resize(320, 180); origin.setWindowTitle(QStringLiteral("huhucapture file manager validation")); origin.show(); events();
        Desktop::FileRevealer revealer;
        std::map<quint64, Desktop::RevealResult> results;
        QObject::connect(&revealer, &Desktop::FileRevealer::finished, &revealer, [&](quint64 token, const Desktop::RevealResult& result) {
            require(!results.contains(token), "a file-manager request completes exactly once"); results.emplace(token, result);
        });
        const auto completed = [&](quint64 token) {
            waitFor([&] { return results.contains(token); }, "the actual file-manager request completes asynchronously"); return results.at(token);
        };
        require(completed(revealer.openDirectory(QStringLiteral("relative/path"), origin.windowHandle())).failure.code == Desktop::Error::InvalidArgument,
            "relative locations never reach a system launcher");
        require(completed(revealer.openDirectory(isolated.path() + QStringLiteral("/missing"), origin.windowHandle())).failure.code == Desktop::Error::InvalidArgument,
            "missing directory reports its real validation failure");
        require(!QFile::exists(isolated.path() + QStringLiteral("/missing")), "validation never creates an unsolicited directory");
        Media::MediaArchiveService archive(isolated.path() + QStringLiteral("/历史 archive"));
        QImage image(20, 16, QImage::Format_ARGB32); image.fill(qRgba(45, 98, 160, 170));
        const auto saved = worker([&] { return archive.archiveImage(image); }); require(saved.durable(), "write a real archived image");
        const auto listed = worker([&] { return archive.list(); }); require(!listed.failure && listed.entries.size() == 1, "enumerate the confirmed actual history entry");
        const auto entry = listed.entries.front();
        auto stale = worker([&] { return archive.lease(entry); }); require(!stale.failure, "acquire a real history read lease");
        { QFile modified(entry.path); require(modified.open(QIODevice::Append), "modify real media outside the application"); require(modified.write("changed") == 7 && modified.flush(), "persist changed bytes before reveal"); }
        require(completed(revealer.revealFile(std::move(stale.lease), origin.windowHandle())).failure.code == Desktop::Error::ChangedOnDisk,
            "a changed file is rejected before the file manager sees its path");
        std::cout << "PASS asynchronous file-manager preconditions and actual stale-file protection\n";
        if (!app.arguments().contains(QStringLiteral("--real-desktop"))) {
            std::cout << "Real Explorer/FileManager1/GIO launch was not requested by this validation mode\n"; return 0;
        }
        // This opt-in mode calls the actual desktop file manager. No D-Bus
        // service, shell launch, response, or URI is simulated by the test.
        const auto directory = isolated.path() + QStringLiteral("/new archive folder");
#ifdef Q_OS_WIN
        ExplorerFixture explorer(isolated);
        explorer.begin(directory);
#endif
        const auto opened = completed(revealer.openDirectory(directory, origin.windowHandle(), true));
        diagnose("open actual directory", opened);
        require(opened.completed() && opened.effect == Desktop::RevealEffect::OpenedDirectory && QDir(directory).exists(),
            "the real desktop accepts opening the explicitly requested created archive directory");
#ifdef Q_OS_WIN
        explorer.displayedAndClosed(directory);
#endif
        const auto second = worker([&] { return archive.archiveImage(image); }); require(second.durable(), "write real media for native file selection");
        const auto current = worker([&] { return archive.list(); }); require(!current.failure, "read current actual history");
        Media::Entry selected;
        for (const auto& item : current.entries) if (item.path == second.path) selected = item;
        require(!selected.path.isEmpty(), "select the exact newly archived file");
        auto lease = worker([&] { return archive.lease(selected); }); require(!lease.failure, "reserve the selected original");
#ifdef Q_OS_WIN
        explorer.begin(QFileInfo(selected.path).absolutePath());
#endif
        const auto revealed = completed(revealer.revealFile(std::move(lease.lease), origin.windowHandle()));
        diagnose("select actual history file", revealed);
        require(revealed.completed() && (revealed.effect == Desktop::RevealEffect::SelectedFile || revealed.effect == Desktop::RevealEffect::OpenedParent),
            "the real desktop accepts file selection or reports the actual parent-folder fallback");
#ifdef Q_OS_WIN
        require(revealed.effect == Desktop::RevealEffect::SelectedFile, "Windows uses its preserved SHOpenFolderAndSelectItems path");
        explorer.displayedAndClosed(QFileInfo(selected.path).absolutePath(), selected.path);
        explorer.finish();
#endif
        require(!worker([&] { return archive.reserveRemoval(selected); }).failure, "native completion releases the history read reservation");
        std::cout << "PASS real desktop file-manager launch, effect=" << static_cast<int>(revealed.effect) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
