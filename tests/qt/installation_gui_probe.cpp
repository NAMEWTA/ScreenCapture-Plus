// External driver for the actual packaged product, never linked into it.
// Native readback reuses ScreenCaptureBackend. Module inspection follows
// package_runtime_probe.cpp, with an actual child process instead of self.
#include "platform/ScreenCaptureBackend.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include "platform/windows/ComApartment.h"
#else
#include <xcb/xcb.h>
#include <xcb/xtest.h>
#endif

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void events() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
bool observe(const std::function<bool()>& condition, int milliseconds)
{
    QElapsedTimer elapsed; elapsed.start();
    do { events(); if (condition()) return true; QThread::msleep(2); } while (elapsed.elapsed() < milliseconds);
    return condition();
}
QByteArray read(const QString& path, qint64 maximum = 8 * 1024 * 1024)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read a real native process record");
    auto bytes = file.read(maximum + 1);
    require(file.error() == QFileDevice::NoError && bytes.size() <= maximum && file.atEnd(), "Read a complete bounded native process record");
    return bytes;
}
QString digest(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Open the actual executable for hashing");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file) && file.error() == QFileDevice::NoError, "Hash the complete actual executable");
    return QString::fromLatin1(hash.result().toHex());
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Create an exclusive evidence file");
    require(file.write(bytes) == bytes.size() && file.flush(), "Write complete installation evidence");
}
struct NativeWindow {
    quintptr id{};
    qint64 pid{};
    QString title;
    QRect rectangle;
    QJsonObject json() const
    {
        return {{QStringLiteral("id"), QString::number(id)}, {QStringLiteral("pid"), pid},
            {QStringLiteral("title"), title}, {QStringLiteral("x"), rectangle.x()}, {QStringLiteral("y"), rectangle.y()},
            {QStringLiteral("width"), rectangle.width()}, {QStringLiteral("height"), rectangle.height()}};
    }
};
#ifdef Q_OS_WIN
bool belongs(qint64 pid, qint64 root) { return pid == root; }
class Windows final {
public:
    std::vector<NativeWindow> visible(qint64 root)
    {
        struct Data { qint64 root; std::vector<NativeWindow> windows; bool failed{}; } data{root, {}, false};
        data.windows.reserve(32);
        const auto enumerated = EnumWindows([](HWND handle, LPARAM parameter) -> BOOL {
            auto& data = *reinterpret_cast<Data*>(parameter);
            DWORD pid{}; GetWindowThreadProcessId(handle, &pid);
            if (!belongs(pid, data.root) || !IsWindowVisible(handle) || IsIconic(handle)) return TRUE;
            try {
                std::array<wchar_t, 1024> title{};
                const auto length = GetWindowTextW(handle, title.data(), static_cast<int>(title.size()));
                RECT client{}; POINT origin{};
                if (!GetClientRect(handle, &client) || !ClientToScreen(handle, &origin)) return TRUE;
                if (data.windows.size() >= 256) { data.failed = true; return FALSE; }
                data.windows.push_back({reinterpret_cast<quintptr>(handle), pid, QString::fromWCharArray(title.data(), length),
                    {static_cast<int>(origin.x), static_cast<int>(origin.y), static_cast<int>(client.right - client.left), static_cast<int>(client.bottom - client.top)}});
            } catch (...) { data.failed = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&data));
        require(enumerated && !data.failed, "Enumerate real owned Windows application windows");
        return data.windows;
    }
    bool close(const NativeWindow& window, qint64 root)
    {
        const auto handle = reinterpret_cast<HWND>(window.id);
        DWORD pid{}; GetWindowThreadProcessId(handle, &pid);
        return belongs(pid, root) && pid == window.pid && PostMessageW(handle, WM_CLOSE, 0, 0);
    }
    void select(const NativeWindow& window, qint64 root, QPoint first, QPoint last)
    {
        const auto handle = reinterpret_cast<HWND>(window.id);
        DWORD pid{}; GetWindowThreadProcessId(handle, &pid);
        require(belongs(pid, root) && pid == window.pid, "The real selection input targets only the owned product");
        require(GetAncestor(WindowFromPoint({first.x(), first.y()}), GA_ROOT) == handle &&
            GetAncestor(WindowFromPoint({last.x(), last.y()}), GA_ROOT) == handle,
            "The actual capture window owns both native selection input points");
        require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0, "No unrelated left-button gesture is active");
        const auto x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        require(width > 1 && height > 1 && QRect(x, y, width, height).contains(first) && QRect(x, y, width, height).contains(last),
            "Native input lies inside the current physical desktop");
        std::array<INPUT, 4> input{};
        for (auto& value : input) value.type = INPUT_MOUSE;
        auto motion = [&](INPUT& value, QPoint point) {
            value.mi.dx = static_cast<LONG>((static_cast<qint64>(point.x()) - x) * 65535 / (width - 1));
            value.mi.dy = static_cast<LONG>((static_cast<qint64>(point.y()) - y) * 65535 / (height - 1));
            value.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
        };
        motion(input[0], first); input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        motion(input[2], last); input[3].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        const auto inserted = SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
        if (inserted > 1 && inserted < input.size()) SendInput(1, &input.back(), sizeof(INPUT));
        require(inserted == input.size(), "The real Windows selection gesture is inserted completely");
    }
};
QString executablePath(qint64 pid)
{
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    require(process, "Inspect the actual owned executable path");
    std::array<wchar_t, 32768> path{}; auto length = static_cast<DWORD>(path.size());
    const auto ok = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process); require(ok, "Read the actual owned executable path");
    return QFileInfo(QString::fromWCharArray(path.data(), static_cast<qsizetype>(length))).canonicalFilePath();
}
QSet<QString> modules(qint64 pid)
{
    const auto process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
    require(process, "Inspect the owned product's loaded modules");
    const auto close = std::unique_ptr<void, decltype(&CloseHandle)>(process, &CloseHandle);
    std::vector<HMODULE> handles(4096); DWORD bytes{};
    require(K32EnumProcessModulesEx(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &bytes, LIST_MODULES_ALL)
        && bytes <= handles.size() * sizeof(HMODULE), "Read every loaded product DLL");
    QSet<QString> result;
    for (std::size_t index{}; index < bytes / sizeof(HMODULE); ++index) {
        std::array<wchar_t, 32768> path{};
        const auto length = K32GetModuleFileNameExW(process, handles[index], path.data(), static_cast<DWORD>(path.size()));
        require(length && length < path.size(), "Read a complete actual module path");
        result.insert(QFileInfo(QString::fromWCharArray(path.data(), static_cast<qsizetype>(length))).canonicalFilePath());
    }
    return result;
}
QJsonObject shortcut(const QString& path)
{
    // Reuse the product's COM apartment lifetime discipline; the link is
    // loaded read-only and never resolved by executing it or saving it back.
    const HuhuCapture::Platform::ComApartment apartment(COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(apartment.result()), "Initialize the actual Shell link reader");
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf()))),
        "Create the native Shell link reader");
    Microsoft::WRL::ComPtr<IPersistFile> file;
    require(SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(reinterpret_cast<LPCOLESTR>(path.utf16()), STGM_READ)),
        "Read the actual installer-created shortcut");
    std::array<wchar_t, 32768> target{}, arguments{};
    require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) &&
        SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))), "Read the real shortcut target and arguments");
    require(target.front() != L'\0', "The actual shortcut has an executable target");
    return {{QStringLiteral("target"), QString::fromWCharArray(target.data())},
        {QStringLiteral("arguments"), QString::fromWCharArray(arguments.data())}};
}
#else
bool belongs(qint64 pid, qint64 root)
{
    // The pinned type-2 runtime forks AppRun in extract-and-run mode and waits
    // for that actual child. Checking only the launcher's PID misses the GUI.
    for (int depth{}; pid > 1 && depth < 32; ++depth) {
        if (pid == root) return true;
        QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!file.open(QIODevice::ReadOnly)) return false;
        const auto bytes = file.read(16384); const auto boundary = bytes.lastIndexOf(')');
        if (boundary < 0) return false;
        const auto fields = bytes.mid(boundary + 1).simplified().split(' ');
        if (fields.size() < 2) return false;
        bool valid{}; const auto parent = fields[1].toLongLong(&valid);
        if (!valid || parent == pid) return false;
        pid = parent;
    }
    return false;
}
template<class T> struct Free { void operator()(T* pointer) const { std::free(pointer); } };
template<class T> using CPointer = std::unique_ptr<T, Free<T>>;
class Windows final {
public:
    Windows()
    {
        int number{}; connectionOwner_.reset(xcb_connect(nullptr, &number)); connection_ = connectionOwner_.get();
        require(connection_ && !xcb_connection_has_error(connection_), "Connect to the actual X11 desktop");
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        while (number-- > 0 && screens.rem) xcb_screen_next(&screens);
        require(screens.rem && screens.data, "Read the actual X11 screen"); root_ = screens.data->root;
        pid_ = atom("_NET_WM_PID"); name_ = atom("_NET_WM_NAME"); utf8_ = atom("UTF8_STRING");
        protocols_ = atom("WM_PROTOCOLS"); close_ = atom("WM_DELETE_WINDOW");
    }
    ~Windows() = default;
    Windows(const Windows&) = delete;
    Windows& operator=(const Windows&) = delete;
    std::vector<NativeWindow> visible(qint64 root)
    {
        std::vector<NativeWindow> result;
        std::vector<xcb_window_t> pending{root_}; QSet<xcb_window_t> visited;
        for (std::size_t index{}; index < pending.size() && index < 4096; ++index) {
            const auto window = pending[index]; if (visited.contains(window)) continue; visited.insert(window);
            CPointer<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection_, xcb_query_tree(connection_, window), nullptr));
            if (tree) {
                const auto count = xcb_query_tree_children_length(tree.get());
                const auto* children = xcb_query_tree_children(tree.get());
                require(count >= 0 && pending.size() + static_cast<std::size_t>(count) <= 4096, "Bound the actual desktop window tree");
                if (count) pending.insert(pending.end(), children, children + count);
            }
            const auto owner = process(window); if (!belongs(owner, root)) continue;
            CPointer<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(connection_, xcb_get_window_attributes(connection_, window), nullptr));
            if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE) continue;
            CPointer<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window), nullptr));
            CPointer<xcb_translate_coordinates_reply_t> origin(xcb_translate_coordinates_reply(connection_, xcb_translate_coordinates(connection_, window, root_, 0, 0), nullptr));
            if (!geometry || !origin) continue;
            const auto title = property(window, name_, utf8_);
            result.push_back({window, owner, title.isEmpty() ? QString::fromLatin1(property(window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING)) : QString::fromUtf8(title),
                {origin->dst_x, origin->dst_y, geometry->width, geometry->height}});
        }
        require(!xcb_connection_has_error(connection_), "The native desktop connection remains alive");
        return result;
    }
    bool close(const NativeWindow& window, qint64 root)
    {
        if (process(static_cast<xcb_window_t>(window.id)) != window.pid || !belongs(window.pid, root)) return false;
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE; event.format = 32;
        event.window = static_cast<xcb_window_t>(window.id); event.type = protocols_;
        event.data.data32[0] = close_; event.data.data32[1] = XCB_CURRENT_TIME;
        const auto request = xcb_send_event_checked(connection_, 0, event.window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&event));
        CPointer<xcb_generic_error_t> error(xcb_request_check(connection_, request));
        return !error && xcb_flush(connection_) > 0;
    }
    void select(const NativeWindow& window, qint64 root, QPoint first, QPoint last)
    {
        require(process(static_cast<xcb_window_t>(window.id)) == window.pid && belongs(window.pid, root),
            "The real X11 selection input targets only the owned product");
        require(window.rectangle.contains(first) && window.rectangle.contains(last) &&
            first.x() >= -32768 && first.y() >= -32768 && last.x() >= -32768 && last.y() >= -32768 &&
            first.x() <= 32767 && first.y() <= 32767 && last.x() <= 32767 && last.y() <= 32767,
            "The actual selection points fit X11's native input coordinates");
        auto ownedPoint = [&](QPoint point) {
            CPointer<xcb_translate_coordinates_reply_t> answer(xcb_translate_coordinates_reply(connection_,
                xcb_translate_coordinates(connection_, root_, root_, static_cast<std::int16_t>(point.x()), static_cast<std::int16_t>(point.y())), nullptr));
            auto ancestor = static_cast<xcb_window_t>(window.id);
            for (int depth{}; depth < 32; ++depth) {
                CPointer<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection_, xcb_query_tree(connection_, ancestor), nullptr));
                if (!tree) return false;
                if (tree->parent == root_) return answer && answer->child == ancestor;
                if (!tree->parent || tree->parent == ancestor) return false;
                ancestor = tree->parent;
            }
            return false;
        };
        require(ownedPoint(first) && ownedPoint(last), "The real capture window owns both X11 selection input points");
        const std::array requests{
            xcb_test_fake_input_checked(connection_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, static_cast<std::int16_t>(first.x()), static_cast<std::int16_t>(first.y()), 0),
            xcb_test_fake_input_checked(connection_, XCB_BUTTON_PRESS, 1, XCB_CURRENT_TIME, root_, 0, 0, 0),
            xcb_test_fake_input_checked(connection_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, static_cast<std::int16_t>(last.x()), static_cast<std::int16_t>(last.y()), 0),
            xcb_test_fake_input_checked(connection_, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, root_, 0, 0, 0)};
        for (const auto request : requests) {
            CPointer<xcb_generic_error_t> error(xcb_request_check(connection_, request));
            require(!error, "The actual XTest server accepts the complete native selection gesture");
        }
        require(xcb_flush(connection_) > 0, "Flush the actual native selection input");
    }
private:
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connectionOwner_{nullptr, &xcb_disconnect};
    xcb_connection_t* connection_{};
    xcb_window_t root_{};
    xcb_atom_t pid_{}, name_{}, utf8_{}, protocols_{}, close_{};
    xcb_atom_t atom(const char* value)
    {
        const auto length = static_cast<std::uint16_t>(std::char_traits<char>::length(value));
        CPointer<xcb_intern_atom_reply_t> answer(xcb_intern_atom_reply(connection_, xcb_intern_atom(connection_, 0, length, value), nullptr));
        require(answer && answer->atom, "Resolve the actual native window protocol atom"); return answer->atom;
    }
    QByteArray property(xcb_window_t window, xcb_atom_t key, xcb_atom_t type)
    {
        CPointer<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_, xcb_get_property(connection_, 0, window, key, type, 0, 4096), nullptr));
        if (!reply || reply->type != type || reply->bytes_after) return {};
        return {static_cast<const char*>(xcb_get_property_value(reply.get())), xcb_get_property_value_length(reply.get())};
    }
    qint64 process(xcb_window_t window)
    {
        const auto value = property(window, pid_, XCB_ATOM_CARDINAL);
        if (value.size() != sizeof(std::uint32_t)) return 0;
        std::uint32_t result{}; std::memcpy(&result, value.constData(), sizeof(result)); return result;
    }
};
QString executablePath(qint64 pid)
{ return QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).canonicalFilePath(); }
QSet<QString> modules(qint64 pid)
{
    QSet<QString> result;
    for (const auto& line : read(QStringLiteral("/proc/%1/maps").arg(pid)).split('\n')) {
        const auto start = line.indexOf('/');
        if (start >= 0) result.insert(QFileInfo(QString::fromUtf8(line.mid(start))).canonicalFilePath());
    }
    return result;
}
#endif
QJsonObject inspectModules(qint64 pid, const QString& executable)
{
    QDir directory(QFileInfo(executable).absolutePath());
#ifdef Q_OS_WIN
    const QStringList required{QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Gui.dll"), QStringLiteral("Qt6Widgets.dll"),
        QStringLiteral("qwindows.dll"), QStringLiteral("vcruntime140.dll"), QStringLiteral("msvcp140.dll")};
    constexpr auto comparison = Qt::CaseInsensitive;
#else
    require(directory.cdUp(), "Read the actual installed Linux prefix");
    const QStringList required{QStringLiteral("libQt6Core.so."), QStringLiteral("libQt6Gui.so."),
        QStringLiteral("libQt6Widgets.so."), QStringLiteral("libqxcb.so")};
    constexpr auto comparison = Qt::CaseSensitive;
#endif
    const auto prefix = directory.canonicalPath() + QChar(u'/');
    const auto loaded = modules(pid); QJsonObject result;
    for (const auto& key : required) {
        QSet<QString> candidates;
        for (const auto& path : loaded) {
            const auto name = QFileInfo(path).fileName();
            if (key.endsWith(u'.') ? name.startsWith(key, comparison) : name.compare(key, comparison) == 0) {
                require(path.startsWith(prefix, comparison), "The actual product loaded a developer or host Qt/CRT module instead of its package");
                candidates.insert(path);
            }
        }
        require(candidates.size() == 1, "Every required native GUI module is uniquely loaded from the real package");
        result.insert(key, *candidates.cbegin());
    }
    return result;
}
QProcessEnvironment productEnvironment(const QString& temporary, bool appimage)
{
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& name : environment.keys()) {
        const auto upper = name.toUpper();
        if (upper.startsWith(QStringLiteral("QT_")) || upper.startsWith(QStringLiteral("QML_")) || upper.startsWith(QStringLiteral("LD_")) ||
            upper.startsWith(QStringLiteral("PIPEWIRE_")) || upper.startsWith(QStringLiteral("SPA_")) ||
            upper == QStringLiteral("APPDIR") || upper == QStringLiteral("APPIMAGE") || upper == QStringLiteral("ARGV0") ||
            upper == QStringLiteral("APPIMAGE_EXTRACT_AND_RUN") || upper == QStringLiteral("NO_CLEANUP")) environment.remove(name);
    }
#ifdef Q_OS_WIN
    const auto system = environment.value(QStringLiteral("SystemRoot"));
    require(QDir::isAbsolutePath(system) && QDir(system + QStringLiteral("/System32")).exists(), "Resolve the actual Windows system directory");
    environment.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(system + QStringLiteral("/System32")) + u';' + QDir::toNativeSeparators(system));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("windows"));
    Q_UNUSED(temporary); Q_UNUSED(appimage);
#else
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb"));
    environment.insert(QStringLiteral("TMPDIR"), temporary);
    if (appimage) environment.insert(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"), QStringLiteral("1"));
#endif
    return environment;
}
class Pattern final : public QWidget {
public:
    static QColor color() { return {39, 118, 203}; }
    int painted{};
    Pattern() { setWindowTitle(QStringLiteral("huhucapture installation readback")); resize(320, 220); setWindowFlag(Qt::WindowStaysOnTopHint); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), color());
        painter.fillRect(QRect(12, 12, 64, 48), QColor(194, 69, 88));
        painter.fillRect(QRect(width() - 76, height() - 60, 64, 48), QColor(68, 187, 113));
        ++painted;
    }
};
struct RestoreCursor {
    HuhuCapture::Platform::ScreenCaptureBackend& backend;
    HuhuCapture::Platform::CaptureDesktop desktop;
    ~RestoreCursor() { try { QPoint actual; backend.moveCursor(desktop, desktop.cursor, actual, {}); } catch (...) {} }
};
QJsonArray windowList(const std::vector<NativeWindow>& windows)
{ QJsonArray values; for (const auto& window : windows) values.append(window.json()); return values; }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    application.setApplicationName(QStringLiteral("huhucapture")); application.setOrganizationName({});
    QCommandLineParser parser; parser.addHelpOption();
    for (const auto* name : {"launch", "profile", "executable-sha256", "artifacts", "write-png", "shortcut"})
        parser.addOption(QCommandLineOption(QString::fromLatin1(name), QString::fromLatin1(name), QStringLiteral("value")));
    parser.addOption(QCommandLineOption(QStringLiteral("paths"), QStringLiteral("Read actual user-data paths without starting the product")));
    parser.addOption(QCommandLineOption(QStringLiteral("appimage"), QStringLiteral("Run the real type-2 runtime in extract-and-run mode")));
    parser.process(application);
    QJsonObject evidence{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("passed"), false},
        {QStringLiteral("qpa"), QGuiApplication::platformName()}};
    QProcess product;
    try {
        if (parser.isSet(QStringLiteral("paths"))) {
            evidence.insert(QStringLiteral("history"), QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("huhucapture")));
#ifdef Q_OS_WIN
            evidence.insert(QStringLiteral("programs"), QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation));
#endif
            evidence.insert(QStringLiteral("passed"), true);
        }
#ifdef Q_OS_WIN
        else if (parser.isSet(QStringLiteral("shortcut"))) {
            evidence.insert(QStringLiteral("shortcut"), shortcut(parser.value(QStringLiteral("shortcut"))));
            evidence.insert(QStringLiteral("passed"), true);
        }
#endif
        else if (parser.isSet(QStringLiteral("write-png"))) {
            const auto path = parser.value(QStringLiteral("write-png"));
            require(!QFileInfo::exists(path), "The real PNG fixture must not replace an existing file");
            QImage image(73, 59, QImage::Format_ARGB32); image.fill(Pattern::color());
            { QPainter painter(&image); painter.fillRect(QRect(9, 7, 23, 31), QColor(194, 69, 88)); }
            require(image.save(path, "PNG") && QImage(path).convertToFormat(QImage::Format_ARGB32) == image, "Create and decode an actual PNG media fixture");
            evidence.insert(QStringLiteral("pngSha256"), digest(path)); evidence.insert(QStringLiteral("passed"), true);
        } else {
            require(QGuiApplication::platformName() == QStringLiteral("windows") || QGuiApplication::platformName() == QStringLiteral("xcb"),
                "Installation GUI acceptance requires an actual native Windows or X11 desktop");
            require(parser.isSet(QStringLiteral("launch")) && parser.isSet(QStringLiteral("profile")) &&
                parser.isSet(QStringLiteral("artifacts")) && parser.isSet(QStringLiteral("executable-sha256")),
                "Native installation GUI validation requires explicit launch, profile, identity and evidence arguments");
            const auto launch = QFileInfo(parser.value(QStringLiteral("launch"))).absoluteFilePath();
            const auto profile = QFileInfo(parser.value(QStringLiteral("profile"))).absoluteFilePath();
            const auto artifacts = QFileInfo(parser.value(QStringLiteral("artifacts"))).absoluteFilePath();
            const auto expected = parser.value(QStringLiteral("executable-sha256"));
            require(QFileInfo(launch).isFile() && QDir(profile).exists() && QDir(artifacts).exists() && expected.size() == 64,
                "The probe receives a real package executable, profile, evidence directory and exact executable hash");
            const auto temporary = QDir(artifacts).filePath(QStringLiteral("runtime-temp"));
            require(QDir().mkpath(temporary), "Create the owned AppImage/Qt temporary directory");
            Windows windows;
            HuhuCapture::Platform::ScreenCaptureBackend backend;
            const auto initialized = backend.initialize(); require(!initialized && !backend.requiresPortal(), "Initialize the actual native readback backend");
            Pattern pattern;
            auto* screen = QGuiApplication::primaryScreen(); require(screen, "The installed product test has a real display");
            pattern.move(screen->availableGeometry().center() - pattern.rect().center()); pattern.show(); pattern.raise();
            require(observe([&] { return pattern.windowHandle() && pattern.windowHandle()->isExposed() && pattern.painted; }, 10000),
                "Present the independent native input image before launching the package");
            const auto own = windows.visible(QCoreApplication::applicationPid());
            const auto marker = std::find_if(own.begin(), own.end(), [](const auto& value) { return value.title == QStringLiteral("huhucapture installation readback"); });
            require(marker != own.end(), "Find the actual independent readback fixture window");
            HuhuCapture::Platform::NativeScreenshot before;
            require(observe([&] {
                before = backend.capture({});
                return !before.failure && before.image.rect().contains(marker->rectangle.center() - before.desktop.bounds.topLeft()) &&
                    before.image.pixelColor(marker->rectangle.center() - before.desktop.bounds.topLeft()) == Pattern::color();
            }, 5000), "Actual desktop pixels contain the presented independent pattern");
            require(before.image.save(QDir(artifacts).filePath(QStringLiteral("before-launch.png")), "PNG"), "Retain actual pre-launch desktop pixels");
            RestoreCursor restoreCursor{backend, before.desktop};
            product.setProcessEnvironment(productEnvironment(temporary, parser.isSet(QStringLiteral("appimage"))));
            product.setWorkingDirectory(profile);
            product.setProgram(launch); product.setArguments({QStringLiteral("--auto-quit"), QStringLiteral("--portable-dir=") + profile});
            product.start();
            require(product.waitForStarted(15000), "Start the real packaged product using its normal one-shot CLI");
            const auto root = product.processId(); evidence.insert(QStringLiteral("launcherPid"), root);
            std::vector<NativeWindow> visible;
            NativeWindow capture;
            const bool opened = observe([&] {
                visible = windows.visible(root);
                for (const auto& window : visible) if (window.title == QStringLiteral("Screen capture") && !window.rectangle.isEmpty()) { capture = window; return true; }
                return product.state() == QProcess::NotRunning;
            }, 45000);
            evidence.insert(QStringLiteral("visibleWindows"), windowList(visible));
            require(opened && capture.id && product.state() != QProcess::NotRunning, "The actual installed product opens its visible native screenshot-selection UI");
            require(std::none_of(visible.begin(), visible.end(), [](const auto& window) {
                return !window.title.isEmpty() && window.title != QStringLiteral("Screen capture");
            }), "Normal one-shot startup must not open an error dialog or another product window");
            const auto actualExecutable = executablePath(capture.pid);
            require(!actualExecutable.isEmpty() && digest(actualExecutable) == expected, "The visible GUI is the exact packaged product executable");
            evidence.insert(QStringLiteral("actualExecutable"), actualExecutable);
            evidence.insert(QStringLiteral("executableSha256"), expected);
            evidence.insert(QStringLiteral("modules"), inspectModules(capture.pid, actualExecutable));
            const auto displayed = backend.capture({});
            require(!displayed.failure && !displayed.image.isNull() && displayed.desktop.bounds.intersects(capture.rectangle),
                "Read back the actual desktop containing the visible installed capture window");
            require(displayed.image.save(QDir(artifacts).filePath(QStringLiteral("installed-capture-window.png")), "PNG"), "Retain actual installed GUI pixels");
            const auto center = marker->rectangle.center();
            const QPoint outside(marker->rectangle.left() + marker->rectangle.width() / 8, center.y());
            const QPoint first(marker->rectangle.left() + marker->rectangle.width() / 4, marker->rectangle.top() + marker->rectangle.height() / 4);
            const QPoint last(marker->rectangle.left() + marker->rectangle.width() * 3 / 4, marker->rectangle.top() + marker->rectangle.height() * 3 / 4);
            windows.select(capture, root, first, last);
            HuhuCapture::Platform::NativeScreenshot selected;
            const bool adjusted = observe([&] {
                if (product.state() == QProcess::NotRunning) return true;
                selected = backend.capture({});
                const auto selectedPoint = center - selected.desktop.bounds.topLeft(), maskedPoint = outside - selected.desktop.bounds.topLeft();
                return !selected.failure && selected.image.rect().contains(selectedPoint) && selected.image.rect().contains(maskedPoint) &&
                    selected.image.pixelColor(selectedPoint) == Pattern::color() && selected.image.pixelColor(maskedPoint) != Pattern::color();
            }, 10000);
            if (!selected.image.isNull()) require(selected.image.save(QDir(artifacts).filePath(QStringLiteral("selected-capture-window.png")), "PNG"),
                "Retain the actual selection gesture's pixels even on failure");
            require(adjusted && product.state() != QProcess::NotRunning, "The real native drag produces an undimmed selected source and a masked outside source");
            const auto afterSelection = windows.visible(root);
            evidence.insert(QStringLiteral("windowsAfterSelection"), windowList(afterSelection));
            require(std::none_of(afterSelection.begin(), afterSelection.end(), [](const auto& window) {
                return !window.title.isEmpty() && window.title != QStringLiteral("Screen capture");
            }), "The actual native selection must not open a product error dialog");
            evidence.insert(QStringLiteral("nativeSelectionObserved"), true);
            require(windows.close(capture, root), "Submit the native window-close protocol to this owned application window");
            require(observe([&] { return product.state() == QProcess::NotRunning; }, 20000), "Normal one-shot close drains the packaged application before exiting");
            evidence.insert(QStringLiteral("exitCode"), product.exitCode());
            require(product.exitStatus() == QProcess::NormalExit && product.exitCode() == 0, "The actual installed application exits successfully after normal GUI close");
            evidence.insert(QStringLiteral("passed"), true);
        }
    } catch (const std::exception& error) {
        evidence.insert(QStringLiteral("failure"), QString::fromUtf8(error.what()));
        if (parser.isSet(QStringLiteral("artifacts")) && (QGuiApplication::platformName() == QStringLiteral("windows") ||
            QGuiApplication::platformName() == QStringLiteral("xcb"))) {
            try {
                HuhuCapture::Platform::ScreenCaptureBackend failureCapture;
                if (!failureCapture.initialize() && !failureCapture.requiresPortal()) {
                    const auto actual = failureCapture.capture({});
                    if (!actual.failure && !actual.image.isNull()) evidence.insert(QStringLiteral("failureDesktopSaved"),
                        actual.image.save(QDir(parser.value(QStringLiteral("artifacts"))).filePath(QStringLiteral("failure-desktop.png")), "PNG"));
                }
            } catch (...) { evidence.insert(QStringLiteral("failureDesktopSaved"), false); }
        }
        if (product.state() != QProcess::NotRunning) {
            product.terminate();
            if (!product.waitForFinished(3000)) { product.kill(); product.waitForFinished(3000); }
            evidence.insert(QStringLiteral("forcedCleanup"), true);
        }
    }
    if (parser.isSet(QStringLiteral("artifacts"))) {
        try {
            const QDir artifacts(parser.value(QStringLiteral("artifacts")));
            write(artifacts.filePath(QStringLiteral("product-stdout.log")), product.readAllStandardOutput());
            write(artifacts.filePath(QStringLiteral("product-stderr.log")), product.readAllStandardError());
        } catch (const std::exception& error) { evidence.insert(QStringLiteral("passed"), false); evidence.insert(QStringLiteral("logFailure"), QString::fromUtf8(error.what())); }
    }
    std::cout << QJsonDocument(evidence).toJson(QJsonDocument::Compact).constData() << '\n';
    return evidence.value(QStringLiteral("passed")).toBool() ? 0 : 1;
}
