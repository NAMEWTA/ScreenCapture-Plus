#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#endif

#include "app/SettingsController.h"
#include "services/desktop/AutoStart.h"
#include "services/desktop/GlobalShortcut.h"
#include "services/desktop/LaunchCommand.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <array>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace app = HuhuCapture::Application;
namespace config = HuhuCapture::Config;
namespace desktop = HuhuCapture::Desktop;
namespace localization = HuhuCapture::Localization;
namespace ui = HuhuCapture::UI::Qt;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void waitFor(Predicate predicate, const char* message, int timeout = 12000)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate()) { require(elapsed.elapsed() < timeout, message); settle(); QThread::msleep(1); }
}
void settleFor(int milliseconds)
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { settle(); QThread::msleep(1); }
}
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read actual fixture file");
    const auto value = file.readAll(); require(file.error() == QFileDevice::NoError, "Read complete fixture"); return value;
}
void write(const QString& path, const QByteArray& value)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Create owned fixture directory");
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Open explicit fixture write");
    require(file.write(value) == value.size() && file.flush(), "Write complete fixture");
    file.close(); require(file.error() == QFileDevice::NoError, "Close fixture write");
}
template<class T> T* child(QWidget& widget, const char* name)
{
    auto* value = widget.findChild<T*>(QString::fromLatin1(name)); require(value, name); return value;
}
void key(QWidget& widget, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent override(QEvent::ShortcutOverride, value, modifiers); QApplication::sendEvent(&widget, &override);
    QKeyEvent down(QEvent::KeyPress, value, modifiers); QApplication::sendEvent(&widget, &down);
    QKeyEvent up(QEvent::KeyRelease, value, modifiers); QApplication::sendEvent(&widget, &up); settle();
}
struct Environment {
    std::map<QByteArray, QByteArray> previous;
    void set(const QByteArray& name, const QByteArray& value)
    { if (!previous.contains(name)) previous[name] = qgetenv(name.constData()); qputenv(name.constData(), value); }
    ~Environment()
    { for (const auto& [name, value] : previous) { if (value.isNull()) qunsetenv(name.constData()); else qputenv(name.constData(), value); } }
};

#ifdef _WIN32
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
struct StartupIsolation {
    HKEY root{};
    std::wstring location;
    explicit StartupIsolation(const QString&)
    {
        location = (QStringLiteral("Software\\huhucapture-tests\\autostart-") + QUuid::createUuid().toString(QUuid::WithoutBraces)).toStdWString();
        require(RegCreateKeyExW(HKEY_CURRENT_USER, location.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS,
            "Create isolated real Windows registry key");
        require(RegOverridePredefKey(HKEY_CURRENT_USER, root) == ERROR_SUCCESS, "Scope HKCU access to this process's real test key");
    }
    ~StartupIsolation()
    {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (root) RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER, location.c_str());
    }
};
QByteArray startupBytes()
{
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    DWORD size{}, type{};
    auto status = RegQueryValueExW(key, L"huhucapture", nullptr, &type, nullptr, &size);
    QByteArray result;
    if (status == ERROR_SUCCESS) {
        result.resize(size);
        status = RegQueryValueExW(key, L"huhucapture", nullptr, &type, reinterpret_cast<BYTE*>(result.data()), &size);
        require(status == ERROR_SUCCESS && type == REG_SZ, "Read actual owned Run entry");
    }
    RegCloseKey(key); return result;
}
void writeStartup(const QByteArray& bytes)
{
    HKEY key{}; require(RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "Open isolated Run key");
    const auto result = bytes.isNull() ? RegDeleteValueW(key, L"huhucapture") :
        RegSetValueExW(key, L"huhucapture", 0, REG_SZ, reinterpret_cast<const BYTE*>(bytes.constData()), static_cast<DWORD>(bytes.size()));
    RegCloseKey(key); require(result == ERROR_SUCCESS || (bytes.isNull() && result == ERROR_FILE_NOT_FOUND), "Mutate only the isolated startup fixture");
}
QByteArray foreignStartup()
{
    const std::wstring value = L"\"C:\\unrelated.exe\" --different-action";
    return QByteArray(reinterpret_cast<const char*>(value.c_str()), static_cast<qsizetype>((value.size() + 1) * sizeof(wchar_t)));
}
void launchRegistered()
{
    const auto bytes = startupBytes(); require(!bytes.isEmpty() && bytes.size() % sizeof(wchar_t) == 0, "Registered command is complete UTF-16");
    std::wstring command(static_cast<std::size_t>(bytes.size()) / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, false, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process),
        "CreateProcess executes the exact registered Unicode command");
    const auto waited = WaitForSingleObject(process.hProcess, 10000); DWORD code{};
    const bool returned = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    require(returned && code == 0, "Registered helper receives --auto-start and exits successfully");
}
void injectChord()
{
    const std::array<WORD, 3> keys{VK_CONTROL, VK_MENU, VK_F9};
    std::array<INPUT, 6> input{};
    for (std::size_t index = 0; index < 3; ++index) {
        input[index].type = INPUT_KEYBOARD; input[index].ki.wVk = keys[index];
        input[3 + index].type = INPUT_KEYBOARD; input[3 + index].ki.wVk = keys[2 - index]; input[3 + index].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    require(SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT)) == input.size(), "Inject real Windows input into the owned global registration");
}
#else
struct StartupIsolation {
    Environment environment;
    explicit StartupIsolation(const QString& root)
    {
        environment.set("XDG_CONFIG_HOME", QDir(root).filePath(QStringLiteral("user 配置")).toUtf8());
        environment.set("XDG_CONFIG_DIRS", QDir(root).filePath(QStringLiteral("system-config")).toUtf8());
        environment.set("XDG_DATA_HOME", QDir(root).filePath(QStringLiteral("data")).toUtf8());
        environment.set("APPIMAGE", {}); environment.set("APPDIR", {});
    }
};
QString startupPath()
{ return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(QStringLiteral("autostart/huhucapture.desktop")); }
QByteArray startupBytes() { return QFileInfo::exists(startupPath()) ? read(startupPath()) : QByteArray(); }
void writeStartup(const QByteArray& bytes)
{
    if (bytes.isNull()) { if (QFileInfo::exists(startupPath())) require(QFile::remove(startupPath()), "Remove only owned startup fixture"); }
    else write(startupPath(), bytes);
}
QByteArray foreignStartup() { return "[Desktop Entry]\nType=Application\nName=unrelated\nExec=/bin/true\n"; }
void launchRegistered()
{
    const auto bytes = startupBytes();
    g_autoptr(GKeyFile) keys = g_key_file_new();
    require(g_key_file_load_from_data(keys, bytes.constData(), static_cast<gsize>(bytes.size()), G_KEY_FILE_NONE, nullptr), "GLib parses actual desktop entry");
    g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new_from_keyfile(keys);
    require(info, "Actual desktop launcher accepts quoted executable and field-code escaping");
    g_autoptr(GError) error = nullptr;
    require(g_app_info_launch(G_APP_INFO(info), nullptr, nullptr, &error), "GLib launches the exact desktop Exec entry");
}
void injectChord()
{
    auto* connection = xcb_connect(nullptr, nullptr); require(connection && !xcb_connection_has_error(connection), "Connect to the actual X11 test server");
    auto* symbols = xcb_key_symbols_alloc(connection); require(symbols, "Use actual server keyboard map");
    std::array<xcb_keycode_t, 3> keys{}; const std::array<xcb_keysym_t, 3> wanted{XKB_KEY_Control_L, XKB_KEY_Alt_L, XKB_KEY_F9};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto* codes = xcb_key_symbols_get_keycode(symbols, wanted[index]); require(codes && *codes, "Translate real XTEST chord");
        keys[index] = *codes; std::free(codes);
    }
    for (const auto code : keys) xcb_test_fake_input(connection, XCB_KEY_PRESS, code, XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0, 0);
    for (auto iterator = keys.rbegin(); iterator != keys.rend(); ++iterator) xcb_test_fake_input(connection, XCB_KEY_RELEASE, *iterator, XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0, 0);
    auto* barrier = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
    require(barrier && !xcb_connection_has_error(connection), "Actual X server processes XTEST input");
    std::free(barrier); xcb_key_symbols_free(symbols); xcb_disconnect(connection);
}
#endif

struct Bindings {
    desktop::GlobalShortcut& shortcut;
    std::map<quint64, desktop::ShortcutFailure> completed;
    QMetaObject::Connection connection;
    explicit Bindings(desktop::GlobalShortcut& value) : shortcut(value)
    {
        connection = QObject::connect(&shortcut, &desktop::GlobalShortcut::bindingFinished, &shortcut,
            [this](quint64 id, const desktop::ShortcutFailure& error) { completed.emplace(id, error); });
    }
    ~Bindings() { QObject::disconnect(connection); }
    desktop::ShortcutFailure wait(quint64 id)
    { waitFor([&] { return completed.contains(id); }, "Actual native binding reaches its terminal result"); return completed.at(id); }
    desktop::ShortcutFailure bind(const QStringList& keys, desktop::GlobalShortcut::Persist persist = {})
    { return wait(shortcut.bind(keys, nullptr, std::move(persist))); }
};
void initializeNative(desktop::GlobalShortcut& shortcut)
{
    shortcut.initialize(); waitFor([&] { return shortcut.snapshot().initialized; }, "Native shortcut adapter initializes");
    require(shortcut.snapshot().editing == desktop::ShortcutEditing::LocalCapture && !shortcut.snapshot().portal,
        "This test requires actual Windows or true X11, not a local/Wayland substitute");
}

void nativeContracts(const QString& root)
{
    const auto firstKeys = desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F9"));
    const auto secondKeys = desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F10"));
    require(desktop::parseShortcut(QStringLiteral("ctrl++")) == QStringList{QStringLiteral("Ctrl"), QStringLiteral("+")}, "Legacy literal plus survives parsing");
    require(desktop::parseShortcut(QStringLiteral("Ctrl+Alt+A")) == QStringList{QStringLiteral("Ctrl"), QStringLiteral("Alt"), QStringLiteral("A")}, "Explicit migrated custom shortcut stays unchanged");
    require(desktop::parseShortcut(QStringLiteral("Ctrl+C")).isEmpty(), "Shared reserved shortcut policy is reused");
    desktop::GlobalShortcut first, second; Bindings a(first), b(second); initializeNative(first); initializeNative(second);
    require(!a.bind(firstKeys), "Actual screenshot shortcut registers");
    require(b.bind(firstKeys).code == desktop::ShortcutError::Conflict, "A second real native client cannot acquire the same shortcut");
    config::SettingsStore conflict(QDir(root).filePath(QStringLiteral("native-conflict.json"))); require(conflict.load().ok(), "Load actual missing settings");
    write(conflict.path(), "{\"outside\":true}");
    bool attempted{};
    const auto rejected = a.bind(secondKeys, [&] { attempted = true; return conflict.setValue({"shortcutKey", "cap"}, "Ctrl+Alt+F10").ok(); });
    require(attempted && rejected.code == desktop::ShortcutError::SaveFailed, "Real stale-config write rejects a successfully registered candidate");
    require(first.snapshot().registered && first.snapshot().requestedKeys == firstKeys, "Previous actual binding survives failed persistence");
    require(b.bind(firstKeys).code == desktop::ShortcutError::Conflict, "Preserved binding still conflicts at the OS");
    require(!b.bind(secondKeys), "Rejected candidate was actually unregistered");
    second.shutdown();
    const auto queued = first.bind(secondKeys, nullptr, [&] { throw std::runtime_error("Canceled callback must never execute"); return false; });
    first.cancel(queued); require(a.wait(queued).code == desktop::ShortcutError::Cancelled, "Cancel before GUI dispatch never registers or persists a candidate");

    int activations{};
    QObject::connect(&first, &desktop::GlobalShortcut::activated, &first, [&](const QString& token) { require(token.isEmpty(), "Native shortcut does not invent an activation token"); ++activations; });
    injectChord(); waitFor([&] { return activations == 1; }, "Real OS hotkey input reaches the application-owned service");
    require(!first.setCaptureActive(true) && first.snapshot().suspended, "Shortcut recording unregisters this application's binding");
    injectChord(); settleFor(100); require(activations == 1, "Recording suppresses actual global shortcut activation");
    {
        desktop::GlobalShortcut competitor; Bindings other(competitor); initializeNative(competitor);
        require(!other.bind(firstKeys), "Released binding is available to a separate real registration");
        require(first.setCaptureActive(false).code == desktop::ShortcutError::RestoreFailed && !first.snapshot().registered,
            "External conflict while recording reports failure instead of claiming a restored key");
        competitor.shutdown();
    }
    require(!first.setCaptureActive(true) && !first.setCaptureActive(false) && first.snapshot().registered, "Retry restores the original actual native registration");
    injectChord(); waitFor([&] { return activations == 2; }, "Restored registration receives real OS input");
    first.shutdown();
    desktop::GlobalShortcut released; Bindings releasedEvents(released); initializeNative(released);
    require(!releasedEvents.bind(firstKeys), "Application shutdown releases its native global shortcut"); released.shutdown();
    std::cout << "Native shortcut registration, conflict, rollback, suspension and OS input passed; physical keyboard not tested.\n";
}

void startupContracts(const QString& root, const QString& executable)
{
    desktop::AutoStart startup(executable);
    require(startup.snapshot().canChange && !startup.snapshot().enabled && startupBytes().isNull(), "Read-only startup inspection creates no OS registration");
    config::SettingsStore store(QDir(root).filePath(QStringLiteral("startup-settings.json"))); require(store.load().ok(), "Load isolated actual startup settings");
    write(store.path(), "{\"external\":17}");
    bool observedRegistered{};
    const auto rejected = startup.setEnabled(true, [&] {
        observedRegistered = startup.snapshot().enabled;
        return store.setValue({"common", "autoStart"}, true).ok();
    });
    require(observedRegistered && rejected.error == desktop::AutoStartError::SaveFailed && !rejected.effective.enabled && startupBytes().isNull(),
        "OS startup succeeds before config write, and real save failure restores the exact missing entry");
    require(store.load().ok(), "Explicit fixture reload acknowledges the external edit");
    require(startup.setEnabled(true, [&] { return store.setValue({"common", "autoStart"}, true).ok(); }).ok(), "Enable actual owned system startup and persist preference");
    require(startup.snapshot().enabled, "Actual OS registration is enabled");
    [[maybe_unused]] const auto saved = startupBytes();
    const auto proof = QDir(root).filePath(QStringLiteral("startup-proof.txt")); Environment environment;
    environment.set("HUHU_STARTUP_TEST_PROOF", proof.toUtf8()); launchRegistered();
    waitFor([&] { return QFileInfo::exists(proof); }, "Registered Unicode executable produces an actual child-process proof");
    require(read(proof) == "--auto-start", "Registered command preserves the tray-start argument");
#ifndef _WIN32
    g_autoptr(GKeyFile) document = g_key_file_new();
    require(g_key_file_load_from_data(document, saved.constData(), static_cast<gsize>(saved.size()), G_KEY_FILE_KEEP_COMMENTS, nullptr), "Load real own desktop file");
    g_key_file_set_string(document, "Desktop Entry", "X-Future-Field", "keep-me");
    gsize length{}; g_autofree gchar* future = g_key_file_to_data(document, &length, nullptr);
    writeStartup(QByteArray(future, static_cast<qsizetype>(length)));
#endif
    require(startup.setEnabled(false, [&] { return store.setValue({"common", "autoStart"}, false).ok(); }).ok(), "Disable only this application's actual startup entry");
    require(!startup.snapshot().enabled, "Disabled actual entry is reflected");
#ifndef _WIN32
    const auto disabled = startupBytes();
    require(disabled.contains("Hidden=true") && disabled.contains("X-Future-Field=keep-me"), "XDG override disables inherited autostart and preserves unknown keys");
    // The system package's entry remains untouched; the user's Hidden=true
    // entry must take precedence over the identical system filename.
    const auto system = QString::fromUtf8(qgetenv("XDG_CONFIG_DIRS"));
    const auto systemFile = QDir(system).filePath(QStringLiteral("autostart/huhucapture.desktop"));
    write(systemFile, saved);
    require(!startup.snapshot().enabled && read(systemFile) == saved, "User Hidden override wins over the actual system directory entry");
    require(QFile::remove(systemFile), "Clean only owned system-directory fixture");
#endif
    writeStartup({});
    write(store.path(), "{\"external\":18}");
    const auto foreign = foreignStartup();
    const auto outside = startup.setEnabled(true, [&] { writeStartup(foreign); return store.setValue({"common", "autoStart"}, true).ok(); });
    require(outside.error == desktop::AutoStartError::RollbackFailed && startupBytes() == foreign,
        "An external writer during failed persistence is preserved instead of overwritten by rollback");
    require(startup.setEnabled(false, {}).error == desktop::AutoStartError::ForeignEntry && startupBytes() == foreign,
        "A foreign same-name registration is never removed");
    writeStartup({});
#ifndef _WIN32
    const auto mount = QDir(root).filePath(QStringLiteral("fake-mount")); require(QDir().mkpath(mount), "Create real fixture mount directory");
    Environment appImage;
    appImage.set("APPIMAGE", executable.toUtf8()); appImage.set("APPDIR", mount.toUtf8());
    require(desktop::persistentLaunchExecutable(executable), "Persistent actual APPIMAGE path is accepted");
    const auto inner = QDir(mount).filePath(QStringLiteral("huhucapture")); require(QFile::copy(executable, inner), "Copy real inner executable fixture");
    require(QFile::setPermissions(inner, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "Set actual executable mode");
    require(!desktop::persistentLaunchExecutable(inner) && !desktop::AutoStart(inner).snapshot().canChange,
        "Mounted inner binary is never saved as the autostart command");
#endif
    std::cout << "Actual startup entry, child command, unknown-field preservation and save rollback passed; login autostart not tested.\n";
}

localization::LanguagePaths languagePaths(const QString& root)
{ return {localization::ResourcePolicy::LinuxUserResources, QCoreApplication::applicationDirPath(), QDir(root).filePath(QStringLiteral("languages"))}; }
void render(QWidget& window, const QString& directory, const QString& name)
{
    require(QDir().mkpath(directory), "Create requested PNG evidence directory"); settle();
    require(window.grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))), "Write actual rendered production settings PNG");
}

void controllerContracts(const QString& root, const QString& executable, const QString& artifacts)
{
    const auto path = QDir(root).filePath(QStringLiteral("controller-config.json"));
    const QByteArray original = "{\"common\":{\"autoStart\":true,\"language\":\"en-US\",\"future\":42},\"shortcutKey\":{\"cap\":\"Ctrl+Alt+F8\"},\"outside\":\"preserve\"}";
    write(path, original); config::SettingsStore store(path); require(store.load().ok(), "Load actual controller config");
    localization::LanguageCatalog catalog(languagePaths(root)); require(catalog.load(QStringLiteral("en-US")).usable, "Load actual embedded language catalog");
    app::SettingsController controller(store, catalog, executable);
    require(controller.initialize(), "Initialize the production controller with shared loaded services");
    waitFor([&] { return !controller.busy(); }, "Application-owned original shortcut registers");
    require(controller.snapshot().captureShortcut == QStringLiteral("Ctrl+Alt+F8"), "UI reflects actual registered shortcut");
    require(!controller.snapshot().autoStart && startupBytes().isNull() && read(path) == original,
        "Initialization reports actual disabled OS state and neither applies saved autoStart=true nor rewrites config");
    QPointer<ui::SettingsWindow> window(controller.openSettings(ui::SettingsWindow::Page::Shortcut)); require(window, "Open the production shared settings window"); settle();
    child<QPushButton>(*window, "settingsCaptureShortcut")->click(); settle(); require(window->capturingShortcut(), "Actual shortcut recorder enters capture state");
    {
        desktop::GlobalShortcut competitor; Bindings events(competitor); initializeNative(competitor);
        require(!events.bind(desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F8"))), "UI capture signal actually releases the OS hotkey"); competitor.shutdown();
    }
    key(*child<QKeySequenceEdit>(*window, "settingsShortcutRecorder"), Qt::Key_F10, Qt::ControlModifier | Qt::AltModifier);
    waitFor([&] { return !controller.busy() && !window->changePending(); }, "Actual settings shortcut request completes");
    require(store.value({"shortcutKey", "cap"}).toString() == QStringLiteral("Ctrl+Alt+F10") &&
        controller.snapshot().captureShortcut == QStringLiteral("Ctrl+Alt+F10"), "UI chord flows through native registration and real persistence");
    desktop::GlobalShortcut competitor; Bindings events(competitor); initializeNative(competitor);
    require(events.bind(desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F10"))).code == desktop::ShortcutError::Conflict,
        "Controller's new binding is genuinely owned at the OS");
    auto external = store.document(); external.insert(QStringLiteral("externalWriter"), true);
    const auto externalBytes = QJsonDocument(external).toJson(); write(path, externalBytes);
    child<QPushButton>(*window, "settingsCaptureShortcut")->click(); settle();
    key(*child<QKeySequenceEdit>(*window, "settingsShortcutRecorder"), Qt::Key_F11, Qt::ControlModifier | Qt::AltModifier);
    waitFor([&] { return !controller.busy() && !window->changePending(); }, "Failed settings write reaches actual UI terminal state");
    require(controller.snapshot().captureShortcut == QStringLiteral("Ctrl+Alt+F10") && read(path) == externalBytes,
        "Failed UI change preserves old native binding and outside config bytes");
    require(!events.bind(desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F11"))), "Failed UI candidate is released for a real other client");
    render(*window, artifacts, QStringLiteral("settings-shortcut-rollback-en"));
    window->setPage(ui::SettingsWindow::Page::General);
    child<QCheckBox>(*window, "settingsAutoStart")->click(); settle();
    waitFor([&] { return !window->changePending(); }, "Actual startup toggle failure returns");
    require(!controller.snapshot().autoStart && startupBytes().isNull() && read(path) == externalBytes,
        "Controller restores actual startup when shared settings save conflicts");
    window->close(); settle(); require(!window, "Closing settings deletes only its view");
    require(events.bind(desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F10"))).code == desktop::ShortcutError::Conflict,
        "Closing the view leaves the application shortcut registered");
    bool stopped{}; QObject::connect(&controller, &app::SettingsController::shutdownFinished, &controller, [&] { stopped = true; });
    controller.shutdown(); waitFor([&] { return stopped; }, "Settings controller shutdown reaches completion");
    require(!events.bind(desktop::parseShortcut(QStringLiteral("Ctrl+Alt+F10"))), "Controller shutdown unregisters the shortcut"); competitor.shutdown();

    require(store.load().ok(), "Explicit fixture reload acknowledges newer settings");
    app::SettingsController restarted(store, catalog, executable); require(restarted.initialize(), "Restart uses the same store and catalog");
    waitFor([&] { return !restarted.busy(); }, "Restored shortcut is actually registered");
    window = restarted.openSettings(); settle();
    auto* languages = child<QComboBox>(*window, "settingsLanguage");
    languages->showPopup(); settle(); key(*languages->view(), Qt::Key_Home); key(*languages->view(), Qt::Key_Return);
    waitFor([&] { return !window->changePending() && catalog.effectiveCode() == QStringLiteral("zh-CN"); }, "Real language picker persists and refreshes the shared catalog");
    require(store.value({"common", "future"}).toInt() == 42 && store.value({"externalWriter"}).toBool(), "Settings edits preserve existing unknown fields");
    require(window->windowTitle() == catalog.text(u"setting.title"), "The same settings window uses the new language immediately");
    child<QCheckBox>(*window, "settingsAutoStart")->click(); settle(); waitFor([&] { return !window->changePending(); }, "Actual successful startup change completes");
    require(restarted.snapshot().autoStart && desktop::AutoStart(executable).snapshot().enabled, "Settings page reflects the enabled actual OS entry");
    render(*window, artifacts, QStringLiteral("settings-general-zh-cn"));
    restarted.shutdown(); waitFor([&] { return !restarted.busy(); }, "Restarted controller shuts down");
    require(desktop::AutoStart(executable).snapshot().enabled, "Application shutdown leaves the user's saved startup registration intact");
    require(desktop::AutoStart(executable).setEnabled(false, [&] { return store.setValue({"common", "autoStart"}, false).ok(); }).ok(), "Clean only the owned startup fixture");
    writeStartup({});
    std::cout << "Production settings window/controller/native-service integration passed with real storage and rendered PNGs.\n";
}

void unavailableContracts(const QString& root, const QString& executable, const QString& artifacts)
{
    config::SettingsStore store(QDir(root).filePath(QStringLiteral("unsupported-config.json")));
    write(store.path(), "{\"common\":{\"language\":\"en-US\"}}"); require(store.load().ok(), "Load actual unsupported-backend fixture");
    localization::LanguageCatalog catalog(languagePaths(root)); require(catalog.load(QStringLiteral("en-US")).usable, "Load actual language");
    app::SettingsController controller(store, catalog, executable); require(controller.initialize(), "Settings remain usable without a native shortcut backend");
    waitFor([&] { return !controller.busy(); }, "Unavailable actual backend completes");
    auto* window = controller.openSettings(ui::SettingsWindow::Page::Shortcut); settle();
    require(controller.snapshot().shortcutEditing == ui::ShortcutEditing::Unavailable && !child<QPushButton>(*window, "settingsCaptureShortcut")->isEnabled(),
        "No desktop backend never presents a local recorder as a global shortcut");
    require(child<QLineEdit>(*window, "settingsShortcutCommand")->isReadOnly() && !child<QLineEdit>(*window, "settingsShortcutCommand")->text().isEmpty(),
        "Unavailable global shortcut provides an actual copyable persistent launch command");
    int captured{}; QObject::connect(&controller, &app::SettingsController::captureRequested, &controller, [&](const QString&) { ++captured; });
    child<QPushButton>(*window, "settingsShortcutRun")->click(); require(captured == 1, "Fallback entry requests the same real application capture action");
    require(startupBytes().isNull(), "Unavailable shortcut inspection does not modify startup");
    render(*window, artifacts, QStringLiteral("settings-shortcut-unavailable")); controller.shutdown();
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--auto-start") {
        QCoreApplication application(argc, argv); const auto proof = qEnvironmentVariable("HUHU_STARTUP_TEST_PROOF");
        if (proof.isEmpty()) return 2;
        try { write(proof, "--auto-start"); return 0; } catch (...) { return 1; }
    }
    try {
        QTemporaryDir directory; require(directory.isValid(), "Create actual isolated filesystem fixture");
        StartupIsolation startup(directory.path());
        QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
        const auto arguments = application.arguments();
        const bool unavailable = arguments.contains(QStringLiteral("--unavailable"));
        const auto artifacts = arguments.size() > 1 && !arguments[1].startsWith(QStringLiteral("--")) ? arguments[1] :
            QDir(directory.path()).filePath(QStringLiteral("render"));
        const auto folder = QDir(directory.path()).filePath(QStringLiteral("工具 files")); require(QDir().mkpath(folder), "Create real Unicode executable directory");
#ifdef _WIN32
        const auto executable = QDir(folder).filePath(QStringLiteral("huhu capture.exe"));
#else
        const auto executable = QDir(folder).filePath(QStringLiteral("hu$hu%capture"));
#endif
        require(QFile::copy(QCoreApplication::applicationFilePath(), executable), "Copy the actual executable to a spaced Unicode path");
        require(QFile::setPermissions(executable, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "Make the actual copied test executable runnable");
        require(desktop::persistentLaunchExecutable(executable), "Actual persistent executable validation succeeds");
        if (unavailable) unavailableContracts(directory.path(), executable, artifacts);
        else { nativeContracts(directory.path()); startupContracts(directory.path(), executable); controllerContracts(directory.path(), executable, artifacts); }
        settle(); return 0;
    } catch (const std::exception& error) { std::cerr << "settings_controller_tests: " << error.what() << '\n'; return 1; }
}
