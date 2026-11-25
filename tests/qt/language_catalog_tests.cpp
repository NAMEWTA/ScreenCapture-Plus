#include "services/localization/LanguageCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

using namespace HuhuCapture::Localization;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void write(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Create isolated language directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Write selected language fixture");
    require(file.write(bytes) == bytes.size(), "Write complete language fixture");
}

QByteArray utf16(const QString& text, bool littleEndian)
{
    QByteArray bytes = littleEndian ? QByteArray::fromHex("fffe") : QByteArray::fromHex("feff");
    for (const auto value : text) {
        const auto code = value.unicode();
        bytes.append(static_cast<char>(littleEndian ? code & 0xff : code >> 8));
        bytes.append(static_cast<char>(littleEndian ? code >> 8 : code & 0xff));
    }
    return bytes;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "Create language test workspace");
    const auto application = temporary.path() + QStringLiteral("/应用程序");
    const auto user = temporary.path() + QStringLiteral("/用户配置");
    LanguageCatalog linuxCatalog({ResourcePolicy::LinuxUserResources, application, user});
    require(linuxCatalog.available().size() == 6, "All six unchanged language resources must be embedded");
    for (const auto& entry : linuxCatalog.available()) {
        const auto loaded = linuxCatalog.load(entry.code);
        require(loaded.usable && !loaded.fellBack && loaded.effectiveCode == entry.code,
            "Load actual shipped UTF-16 language resource");
        require(!linuxCatalog.text(u"annotation.exportFailed").isEmpty()
            && linuxCatalog.text(u"annotation.exportFailed") != QStringLiteral("annotation.exportFailed"),
            "Shipped translation lost the complete-export error");
        require(linuxCatalog.text(u"unrecognized.translation.key") == QStringLiteral("unrecognized.translation.key"),
            "Unknown keys must remain diagnosable");
    }
    const auto chosen = user + QStringLiteral("/Lang/日本語.ja-JP.json");
    write(chosen, utf16(QStringLiteral("{\"tool\":{\"rect\":\"独自の矩形\",\"ellipse\":42,\"arrow\":\"\"}}"), true));
    int changes{};
    QObject::connect(&linuxCatalog, &LanguageCatalog::changed, &app, [&] { ++changes; });
    require(linuxCatalog.load(QStringLiteral("ja-JP")).usable, "Load explicit user language");
    require(linuxCatalog.text(u"tool.rect") == QStringLiteral("独自の矩形"), "Linux user resource overrides the chosen packaged language");
    require(linuxCatalog.text(u"tool.ellipse") == QStringLiteral("Ellipse")
        && linuxCatalog.text(u"tool.arrow") == QStringLiteral("Arrow")
        && linuxCatalog.text(u"tool.save") == QStringLiteral("Save"), "Missing, wrong-type and empty text use embedded English");
    write(chosen, utf16(QStringLiteral("{\"tool\":{\"rect\":\"変更した矩形\"}}"), false));
    require(linuxCatalog.load(QStringLiteral("ja-JP")).usable
        && linuxCatalog.text(u"tool.rect") == QStringLiteral("変更した矩形") && changes == 2,
        "Reload UTF-16BE invalidates text cache and notifies every window");
    write(chosen, QByteArray::fromHex("fffe00d8")); // An actual unpaired UTF-16 surrogate.
    const auto broken = linuxCatalog.load(QStringLiteral("ja-JP"));
    require(broken.usable && broken.fellBack && !broken.error.ok()
        && linuxCatalog.requestedCode() == QStringLiteral("ja-JP")
        && linuxCatalog.effectiveCode() == QStringLiteral("en-US")
        && linuxCatalog.text(u"tool.rect") == QStringLiteral("Rectangle"),
        "Corrupt chosen language falls back without changing the persisted selection");
    write(chosen, QByteArray("\xef\xbb\xbf{\"tool\":{\"rect\":\"BOM UTF-8\"}}"));
    require(!linuxCatalog.load(QStringLiteral("ja-JP")).fellBack
        && linuxCatalog.text(u"tool.rect") == QStringLiteral("BOM UTF-8"), "Decode actual UTF-8 BOM language");
    write(user + QStringLiteral("/Lang/Override.en-US.json"), "{\"tool\":{\"rect\":\"wrong\"}}");
    require(!linuxCatalog.load(QStringLiteral("en-US")).fellBack
        && linuxCatalog.text(u"tool.rect") == QStringLiteral("Rectangle"), "Preserve built-in English authority");
    require(linuxCatalog.available().size() == 6, "Avoid duplicate language choices for overridden codes");

    LanguageCatalog windows({ResourcePolicy::WindowsDirectoryFallback, application, user});
    require(!windows.load(QStringLiteral("ja-JP")).fellBack
        && windows.text(u"tool.rect") == QStringLiteral("BOM UTF-8"), "Windows uses user Lang only when EXE Lang is absent");
    require(QDir().mkpath(application + QStringLiteral("/Lang")), "Create intentionally empty portable Lang directory");
    require(windows.available().size() == 2 && windows.load(QStringLiteral("ja-JP")).fellBack,
        "Existing empty EXE Lang suppresses user Lang; resource directories must not merge");
    write(application + QStringLiteral("/Lang/日本語.ja-JP.json"), "{\"tool\":{\"rect\":\"Portable\"}}");
    write(application + QStringLiteral("/Lang/Ignored.ja-JP.extra.json"), "{}");
    require(!windows.load(QStringLiteral("ja-JP")).fellBack
        && windows.text(u"tool.rect") == QStringLiteral("Portable")
        && windows.available().size() == 3, "Windows portable resource priority and original three-part names");
    require(windows.load(QStringLiteral("ja")).fellBack, "Language codes must not match by substring");
    require(!windows.load(QStringLiteral("zh-CN")).fellBack
        && windows.text(u"tool.rect") != QStringLiteral("Rectangle"), "Built-in Chinese survives external directory failure");
    std::cout << "PASS six original language resources, BOM encodings, English fallback, cache refresh and directory policies\n";
}
