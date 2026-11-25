#include "app/EditorPreferences.h"
#include "app/MediaWorkspace.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/localization/LanguageCatalog.h"
#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPointer>
#include <QSlider>
#include <QTemporaryDir>
#include <QToolButton>
#include <array>
#include <iostream>
#include <stdexcept>

namespace app = HuhuCapture::Application;
namespace config = HuhuCapture::Config;
namespace desktop = HuhuCapture::Desktop;
namespace media = HuhuCapture::Media;
namespace ui = HuhuCapture::UI::Qt;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void events()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Open the owned configuration fixture");
    require(file.write(bytes) == bytes.size() && file.flush(), "Write the complete real configuration fixture");
}
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read the actual configuration file");
    const auto result = file.readAll(); require(file.error() == QFileDevice::NoError, "Read complete configuration bytes");
    return result;
}
QByteArray legacyBytes(const QString& text)
{
    QByteArray bytes("\xff\xfe", 2);
    for (const auto character : text) {
        const auto value = character.unicode();
        bytes.append(static_cast<char>(value & 255)); bytes.append(static_cast<char>(value >> 8));
    }
    return bytes;
}
QToolButton* button(ui::EditorWindow& window, const char* name)
{
    auto* found = window.findChild<QToolButton*>(QString::fromLatin1(name));
    require(found && found->isEnabled(), "The real editor exposes its enabled toolbar control"); return found;
}
void select(ui::EditorWindow& window, const char* name, ui::EditorTool tool)
{
    button(window, name)->click();
    require(window.canvas()->tool() == tool, "Select the actual shared editor tool through its toolbar");
}
void value(ui::EditorWindow& window, int next)
{
    auto* slider = window.findChild<QSlider*>(QStringLiteral("toolValue"));
    require(slider && slider->isEnabled(), "The actual tool slider is available"); slider->setValue(next);
}
void pointer(ui::EditorCanvas& canvas, QEvent::Type type, QPointF source, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    const auto local = canvas.sourceToView(source);
    QMouseEvent event(type, local, QPointF(canvas.mapTo(canvas.window(), local.toPoint())),
        QPointF(canvas.mapToGlobal(local.toPoint())), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
QImage exported(ui::EditorWindow& window)
{
    QImage output; require(!window.canvas()->renderExport(output), "Render the actual source and active editor shapes");
    return output;
}
QImage sourceImage()
{
    QImage image(160, 110, QImage::Format_ARGB32);
    image.fill(QColor(230, 241, 249, 211)); image.setPixelColor(1, 1, QColor(55, 66, 77, 0)); return image;
}
void realEditorPersistence(const QString& root)
{
    const auto path = QDir(root).filePath(QStringLiteral("config.json"));
    // This is the production Setting/ToolSub schema, encoded like the native
    // product's UTF-16 file. Unknown data is intentional compatibility input.
    const auto original = legacyBytes(QStringLiteral(R"json({
      "common":{"language":"zh-CN","future":"原始设置"},
      "shortcutKey":{"cap":"F1"},
      "toolPin":{
        "rect":{"width":7.25,"fill":true,"colorIndex":4,"future":{"value":19}},
        "ellipse":{"width":25.5,"fill":false,"colorIndex":2},
        "arrow":{"width":9.5,"fill":false,"colorIndex":1},
        "number":{"radius":44.25,"fill":true,"colorIndex":6},
        "line":{"width":31,"semiTransparent":true,"colorIndex":3},
        "text":{"fontSize":33.5,"bold":true,"italic":true,"colorIndex":7,"width":2},
        "mosaic":{"width":51,"rect":true,"colorIndex":7},
        "eraser":{"width":47,"rect":false},
        "annotation":{"future":"不修改"},"futureTool":{"values":[1,2,3]}
      },"futureRoot":{"enabled":true}
    })json"));
    write(path, original);
    config::SettingsStore settings(path); require(settings.load().ok(), "Load the actual legacy UTF-16 tool settings");
    auto archive = std::make_shared<media::MediaArchiveService>(QDir(root).filePath(QStringLiteral("archive")));
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    app::MediaWorkspace workspace(archive, clipboard, revealer);
    HuhuCapture::Localization::LanguageCatalog language({
#ifdef Q_OS_WIN
        HuhuCapture::Localization::ResourcePolicy::WindowsDirectoryFallback,
#else
        HuhuCapture::Localization::ResourcePolicy::LinuxUserResources,
#endif
        root, root});
    require(language.load(QStringLiteral("zh-CN")).usable, "Load the production Chinese catalog");
    workspace.setTranslator([&](QStringView key) { return language.text(key); });
    require(workspace.setEditorSettings(settings) && workspace.setEditorSettings(settings), "Bind the one real SettingsStore idempotently");
    int failures{}; QString detail;
    QObject::connect(&workspace, &app::MediaWorkspace::operationFailed, &workspace,
        [&](const QString& message) { ++failures; detail = message; });

    const auto image = sourceImage();
    QPointer<ui::EditorWindow> first = workspace.openEditor(image, true);
    require(first, "Open a real pinned editor using the application's configuration"); events();
    const std::array tools{ui::EditorTool::Rectangle, ui::EditorTool::Ellipse, ui::EditorTool::Arrow,
        ui::EditorTool::Number, ui::EditorTool::Line, ui::EditorTool::Text, ui::EditorTool::Mosaic, ui::EditorTool::Eraser};
    constexpr std::array widths{7.25f, 25.5f, 9.5f, 44.25f, 31.f, 33.5f, 51.f, 47.f};
    constexpr std::array colors{4, 2, 1, 6, 3, 7};
    for (std::size_t index = 0; index < tools.size(); ++index) {
        const auto style = first->canvas()->toolStyle(tools[index]);
        require(style.value == widths[index], "Restore each tool's exact logical-pixel preference without integer rounding");
        if (index < colors.size()) require(style.rgba == HuhuCapture::Editor::toolColors[colors[index]],
            "Restore the native palette index as its exact original RGBA color");
    }
    require(first->canvas()->toolStyle(ui::EditorTool::Rectangle).filled &&
        !first->canvas()->toolStyle(ui::EditorTool::Ellipse).filled &&
        !first->canvas()->toolStyle(ui::EditorTool::Arrow).filled &&
        first->canvas()->toolStyle(ui::EditorTool::Number).filled &&
        first->canvas()->toolStyle(ui::EditorTool::Line).semiTransparent &&
        first->canvas()->toolStyle(ui::EditorTool::Text).bold && first->canvas()->toolStyle(ui::EditorTool::Text).italic &&
        first->canvas()->toolStyle(ui::EditorTool::Mosaic).rectangleMode &&
        !first->canvas()->toolStyle(ui::EditorTool::Eraser).rectangleMode,
        "Restore every existing ToolSub toggle under its original key");
    select(*first, "tool.rect", ui::EditorTool::Rectangle);
    require(first->canvas()->toolStyle(ui::EditorTool::Rectangle).value == 7.25f && read(path) == original,
        "Loading and selecting tools does not round stored values or rewrite the existing file");

    pointer(*first->canvas(), QEvent::MouseButtonPress, {15, 16}, Qt::LeftButton, Qt::LeftButton);
    pointer(*first->canvas(), QEvent::MouseMove, {95, 70}, Qt::NoButton, Qt::LeftButton);
    pointer(*first->canvas(), QEvent::MouseButtonRelease, {95, 70}, Qt::LeftButton, Qt::NoButton);
    require(first->canvas()->document().activeCount() == 1, "A real editor gesture creates the existing shared rectangle model");
    const auto drawn = exported(*first);
    value(*first, 11); button(*first, "tool.rectFill")->click(); button(*first, "color.white")->click();
    require(settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("width")}).toDouble() == 11 &&
        !settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("fill")}).toBool(true) &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("colorIndex")}).toInt() == 8,
        "Actual slider, fill and palette controls persist through the sole SettingsStore");
    require(exported(*first) == drawn, "Persisting a preference does not restyle already-created document shapes");
    const auto backups = QDir(root).entryList({QStringLiteral("config.json.pre-qt-*.bak")}, QDir::Files);
    require(backups.size() == 1 && read(QDir(root).filePath(backups.front())) == original,
        "The first real migration keeps an exact backup of the original UTF-16 bytes");

    QPointer<ui::EditorWindow> second = workspace.openEditor(image, false);
    require(second && second->canvas()->toolStyle(ui::EditorTool::Rectangle) == first->canvas()->toolStyle(ui::EditorTool::Rectangle),
        "A new screenshot editor restores the same committed preferences as a pinned editor");
    select(*first, "tool.ellipse", ui::EditorTool::Ellipse);
    select(*second, "tool.rect", ui::EditorTool::Rectangle); value(*second, 15);
    select(*first, "tool.rect", ui::EditorTool::Rectangle);
    require(first->canvas()->toolStyle(ui::EditorTool::Rectangle).value == 15 && exported(*first) == drawn,
        "Selecting a tool rereads another window's saved value without changing existing shapes");
    button(*first, "tool.rectFill")->click(); value(*second, 16);
    require(settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("fill")}).toBool() &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("width")}).toDouble() == 16,
        "A stale window changing only width cannot overwrite a fill change committed by another window");
    value(*second, 15);
    select(*first, "tool.text", ui::EditorTool::Text);
    require(first->canvas()->toolStyle(ui::EditorTool::Text).value == 33.5f,
        "Changing from a line-width tool preserves the explicitly saved text size");
    value(*first, 44); button(*first, "tool.bold")->click(); button(*first, "tool.italic")->click(); button(*first, "color.blue")->click();
    select(*first, "tool.line", ui::EditorTool::Line); button(*first, "tool.semiTransparent")->click();
    select(*first, "tool.mosaic", ui::EditorTool::Mosaic); button(*first, "tool.rectFill")->click(); value(*first, 55);
    select(*first, "tool.eraser", ui::EditorTool::Eraser); button(*first, "tool.rectFill")->click();
    select(*first, "tool.number", ui::EditorTool::Number); button(*first, "tool.numberFill")->click(); value(*first, 50);
    require(settings.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("fontSize")}).toDouble() == 44 &&
        !settings.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("bold")}).toBool(true) &&
        !settings.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("italic")}).toBool(true) &&
        !settings.value({QStringLiteral("toolPin"), QStringLiteral("line"), QStringLiteral("semiTransparent")}).toBool(true) &&
        !settings.value({QStringLiteral("toolPin"), QStringLiteral("mosaic"), QStringLiteral("rect")}).toBool(true) &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("eraser"), QStringLiteral("rect")}).toBool() &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("number"), QStringLiteral("radius")}).toDouble() == 50,
        "Every real style-control family writes the retained ToolSub key, including radius and rect");
    require(settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("future")}).toObject().value(QStringLiteral("value")).toInt() == 19 &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("annotation"), QStringLiteral("future")}).toString() == QStringLiteral("不修改") &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("width")}).toInt() == 2 &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("mosaic"), QStringLiteral("colorIndex")}).toInt() == 7 &&
        settings.value({QStringLiteral("futureRoot"), QStringLiteral("enabled")}).toBool(),
        "Preference writes retain unrelated, unknown and unused existing fields");

    config::SettingsStore reloaded(path); require(reloaded.load().ok(), "Reload the actual committed configuration from disk");
    for (const auto tool : tools) require(app::readEditorPreference(settings, tool) == app::readEditorPreference(reloaded, tool),
        "A separate real reload reconstructs every committed tool style");
    require(!workspace.setEditorSettings(reloaded), "A live workspace cannot silently switch to a second configuration owner");
    const auto expected = app::readEditorPreference(settings, ui::EditorTool::Text);
    first->close(); second->close(); events(); require(!first && !second, "Close the actual editing windows");
    QPointer<ui::EditorWindow> reopened = workspace.openEditor(image);
    require(reopened && reopened->canvas()->toolStyle(ui::EditorTool::Text) == expected,
        "Closing and reopening an editor retains the actual persisted preferences");

    // A real independent write changes the file after this store's read. Its
    // byte-level optimistic concurrency check must remain the only writer gate.
    auto external = reloaded.document(); auto common = external.value(QStringLiteral("common")).toObject();
    common.insert(QStringLiteral("external"), QStringLiteral("retain the independent writer")); external.insert(QStringLiteral("common"), common);
    const auto externalBytes = QJsonDocument(external).toJson(QJsonDocument::Compact); write(path, externalBytes);
    select(*reopened, "tool.rect", ui::EditorTool::Rectangle); value(*reopened, 19); value(*reopened, 20);
    require(failures == 0 && workspace.busy(), "A queued save failure prevents early exit without reentering an active toolbar dispatch"); events();
    require(failures == 1 && !workspace.busy() && !detail.isEmpty() && read(path) == externalBytes &&
        settings.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("width")}).toDouble() == 15,
        "An actual disk conflict is reported once and changes neither the other writer's bytes nor the committed snapshot");
    require(reopened->canvas()->toolStyle(ui::EditorTool::Rectangle).value == 20,
        "A failed preference write does not undo the user's current editor state");
    require(settings.load().ok(), "Explicitly reload the same configuration owner after resolving a real conflict");
    select(*reopened, "tool.text", ui::EditorTool::Text); select(*reopened, "tool.rect", ui::EditorTool::Rectangle);
    require(reopened->canvas()->toolStyle(ui::EditorTool::Rectangle).value == 15,
        "Tool reselection after reload uses the last actual committed value");
    value(*reopened, 21);
    config::SettingsStore final(path); require(final.load().ok() &&
        final.value({QStringLiteral("toolPin"), QStringLiteral("rect"), QStringLiteral("width")}).toDouble() == 21 &&
        final.value({QStringLiteral("common"), QStringLiteral("external")}).toString() == QStringLiteral("retain the independent writer"),
        "A later successful edit preserves the independent writer's other fields");
    std::cout << "PASS legacy UTF-16 -> real editor controls -> reopen/reload; exact backups, shared selection refresh and real write conflict\n";
}

void defaultsAndReadOnlyCompatibility(const QString& root)
{
    const auto path = QDir(root).filePath(QStringLiteral("compatibility.json"));
    const QByteArray original = R"json({"toolPin":{"rect":{"width":0,"colorIndex":2.75},"text":{"width":2,"bold":"old unknown value"},"mosaic":{"width":80,"rect":true},"eraser":"retain original","future":{"key":7}}})json";
    write(path, original); config::SettingsStore store(path); require(store.load().ok(), "Load retained hand-edited compatible settings");
    require(app::readEditorPreference(store, ui::EditorTool::Rectangle).value == 1 &&
        app::readEditorPreference(store, ui::EditorTool::Rectangle).rgba == HuhuCapture::Editor::toolColors[2] &&
        app::readEditorPreference(store, ui::EditorTool::Mosaic).value == 68 &&
        app::readEditorPreference(store, ui::EditorTool::Text).value == 20 &&
        !app::readEditorPreference(store, ui::EditorTool::Text).bold,
        "Shared mature limits and valid palette truncation apply without inheriting an unrelated 2 px text width");
    require(read(path) == original, "Reading fallback preferences never rewrites raw values");
    auto eraser = app::readEditorPreference(store, ui::EditorTool::Eraser); auto changed = eraser; changed.value += 1;
    const auto refused = app::writeEditorPreference(store, ui::EditorTool::Eraser, eraser, changed);
    require(refused.error == config::SettingsError::InvalidKey && read(path) == original &&
        store.value({QStringLiteral("toolPin"), QStringLiteral("eraser")}).toString() == QStringLiteral("retain original"),
        "A known tool whose parent is not an object is retained and requires an explicit repair");
    auto text = app::readEditorPreference(store, ui::EditorTool::Text); auto colored = text; colored.rgba = HuhuCapture::Editor::toolColors[1];
    require(app::writeEditorPreference(store, ui::EditorTool::Text, text, colored).ok() &&
        store.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("bold")}).toString() == QStringLiteral("old unknown value") &&
        store.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("fontSize")}, QJsonValue::Undefined).isUndefined(),
        "Changing a color writes only colorIndex and preserves untouched noncanonical values");
    auto annotation = app::readEditorPreference(store, ui::EditorTool::Annotation); auto altered = annotation; altered.value = 9;
    require(app::writeEditorPreference(store, ui::EditorTool::Annotation, annotation, altered).ok() &&
        store.value({QStringLiteral("toolPin"), QStringLiteral("annotation")}, QJsonValue::Undefined).isUndefined(),
        "The annotation tool does not invent a preference group absent from its original UI");

    auto archive = std::make_shared<media::MediaArchiveService>(QDir(root).filePath(QStringLiteral("late-archive")));
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    app::MediaWorkspace late(archive, clipboard, revealer);
    require(late.openEditor(sourceImage()) && !late.setEditorSettings(store),
        "A late binding is refused instead of changing an existing editing journey's preferences");
    const auto freshPath = QDir(root).filePath(QStringLiteral("fresh.json"));
    config::SettingsStore fresh(freshPath); require(fresh.load().ok(), "Load a real new profile without an existing toolPin group");
    app::MediaWorkspace freshWorkspace(archive, clipboard, revealer);
    require(freshWorkspace.setEditorSettings(fresh), "Bind a new profile's single SettingsStore");
    QPointer<ui::EditorWindow> newEditor = freshWorkspace.openEditor(sourceImage()); require(newEditor, "Open a new profile's actual editor");
    select(*newEditor, "tool.text", ui::EditorTool::Text); value(*newEditor, 28);
    config::SettingsStore freshReload(freshPath);
    require(freshReload.load().ok() &&
        freshReload.value({QStringLiteral("toolPin"), QStringLiteral("text"), QStringLiteral("fontSize")}).toDouble() == 28 &&
        freshReload.value({QStringLiteral("toolPin"), QStringLiteral("rect")}, QJsonValue::Undefined).isUndefined(),
        "The first actual preference adjustment creates only its required tool group in a new profile");
    std::cout << "PASS defaults, unchanged foreign fields and rejected late/configuration replacement\n";
}

void failureMayDestroyWorkspace(const QString& root)
{
    const auto path = QDir(root).filePath(QStringLiteral("reentrant.json"));
    write(path, QByteArrayLiteral("{\"common\":{\"language\":\"en-US\"}}"));
    config::SettingsStore settings(path); require(settings.load().ok(), "Load the real owner-lifetime fixture");
    auto archive = std::make_shared<media::MediaArchiveService>(QDir(root).filePath(QStringLiteral("reentrant-archive")));
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    auto workspace = std::make_unique<app::MediaWorkspace>(archive, clipboard, revealer);
    require(workspace->setEditorSettings(settings), "Bind the existing store before opening the reentrant editor");
    QPointer<ui::EditorWindow> editor = workspace->openEditor(sourceImage()); require(editor, "Open the real editor under its owning workspace");
    select(*editor, "tool.rect", ui::EditorTool::Rectangle);
    const QByteArray external = "{\"common\":{\"language\":\"en-US\"},\"independent\":true}";
    write(path, external);
    QObject observer; int failures{};
    QObject::connect(workspace.get(), &app::MediaWorkspace::operationFailed, &observer,
        [&](const QString&) { ++failures; workspace.reset(); });
    button(*editor, "color.blue")->click();
    require(workspace && editor && failures == 0, "The real color-control event returns before an error handler may delete its owner");
    events();
    require(!workspace && !editor && failures == 1 && read(path) == external,
        "The deferred actual save-error notification permits synchronous workspace and editor destruction safely");
    std::cout << "PASS actual failed preference write and reentrant workspace destruction after toolbar dispatch\n";
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir directory; require(directory.isValid(), "Create private real configuration fixtures");
        realEditorPersistence(directory.path()); defaultsAndReadOnlyCompatibility(directory.path());
        failureMayDestroyWorkspace(directory.path());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
