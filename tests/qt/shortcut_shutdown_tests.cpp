#include "app/SettingsController.h"
#include "services/desktop/GlobalShortcut.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace desktop = HuhuCapture::Desktop;
namespace application = HuhuCapture::Application;
namespace config = HuhuCapture::Config;
namespace localization = HuhuCapture::Localization;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void waitFor(Predicate predicate, const char* message)
{
    const auto until = std::chrono::steady_clock::now() + 20s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < until, message);
        settle(); std::this_thread::sleep_for(1ms);
    }
}
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read the actual settings file");
    return file.readAll();
}

void queuedBindings()
{
    desktop::GlobalShortcut shortcut;
    unsigned bindings{}, drained{};
    quint64 first{}, rejected{};
    QObject::connect(&shortcut, &desktop::GlobalShortcut::bindingFinished, &shortcut,
        [&](quint64 request, const desktop::ShortcutFailure& failure) {
            require((request == first || request == rejected) && failure.code == desktop::ShortcutError::Cancelled,
                "The actual queued binding and busy rejection are both cancelled by shutdown");
            ++bindings;
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            require(drained == 0, "The facade must not finish shutdown inside a pending binding callback");
        });
    QObject::connect(&shortcut, &desktop::GlobalShortcut::shutdownFinished, &shortcut, [&] {
        require(++drained == 1 && bindings == 2, "Both admitted requests finish before facade shutdown");
        require(!shortcut.snapshot().registered && shortcut.bind({QStringLiteral("Ctrl"), QStringLiteral("F11")}, nullptr) == 0,
            "A drained facade cannot register or queue a new native shortcut");
        shortcut.shutdown();
    });
    first = shortcut.bind({QStringLiteral("Ctrl"), QStringLiteral("Shift"), QStringLiteral("F11")}, nullptr);
    rejected = shortcut.bind({QStringLiteral("Ctrl"), QStringLiteral("Shift"), QStringLiteral("F10")}, nullptr);
    require(first && rejected && first != rejected && bindings == 0, "Queue two distinct real shortcut admissions without dispatching them");
    shortcut.shutdown(); require(drained == 0, "The facade cannot acknowledge shutdown inline");
    waitFor([&] { return drained == 1; }, "Queued shortcut cancellation drains");
    settle(); require(bindings == 2 && drained == 1, "No completion is repeated after shutdown");
}

void initialization()
{
    desktop::GlobalShortcut shortcut;
    unsigned initialized{}, drained{};
    QObject::connect(&shortcut, &desktop::GlobalShortcut::initialized, &shortcut, [&] {
        require(++initialized == 1, "The actual platform initialization has one completion");
        shortcut.shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        require(drained == 0, "Shutdown must not precede the return of platform initialization");
    });
    QObject::connect(&shortcut, &desktop::GlobalShortcut::shutdownFinished, &shortcut, [&] {
        require(++drained == 1 && initialized == 1 && !shortcut.snapshot().registered,
            "Native resources and actual initialization complete before shutdown");
    });
    shortcut.initialize(); shortcut.shutdown();
    waitFor([&] { return drained == 1; }, "Actual shortcut platform initialization cancellation drains");
    shortcut.initialize(); settle(); require(initialized == 1 && drained == 1, "Shutdown refuses reinitialization");

    auto disposable = std::make_unique<desktop::GlobalShortcut>();
    const QPointer<desktop::GlobalShortcut> life(disposable.get());
    QObject::connect(disposable.get(), &desktop::GlobalShortcut::shutdownFinished, qApp, [&] { disposable.reset(); });
    disposable->shutdown();
    waitFor([&] { return !life; }, "A host may destroy a shortcut service from its actual drain acknowledgement");
}

void settings(const QString& root)
{
    config::SettingsStore store(root + QStringLiteral("/config.json"));
    QFile file(store.path());
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Create actual legacy settings for shutdown");
    const QByteArray document("{\"common\":{\"language\":\"en-US\"},\"shortcutKey\":{\"cap\":\"Ctrl+Shift+F11\"},\"unknown\":{\"retain\":17}}");
    require(file.write(document) == document.size(), "Write the complete original settings"); file.close();
    require(store.load().ok(), "Load the actual settings without a replacement provider");
    localization::LanguageCatalog catalog({
#ifdef Q_OS_WIN
        localization::ResourcePolicy::WindowsDirectoryFallback,
#else
        localization::ResourcePolicy::LinuxUserResources,
#endif
        root, root});
    require(catalog.load(QStringLiteral("en-US")).usable, "Load actual shared language resources");
    const auto before = read(store.path());
    auto controller = std::make_unique<application::SettingsController>(store, catalog, QCoreApplication::applicationFilePath());
    unsigned drained{};
    QObject::connect(controller.get(), &application::SettingsController::shutdownFinished, qApp, [&] {
        require(++drained == 1 && !controller->busy() && !controller->captureShortcutAvailable(),
            "Settings acknowledge shutdown only after the actual shortcut service has drained");
        require(!controller->openSettings(), "A drained settings owner cannot reopen a view");
        controller.reset();
    });
    require(controller->initialize(), "Initialize the real settings/native shortcut assembly");
    controller->shutdown();
    require(controller->busy() && drained == 0, "Settings retain the shutdown gate until the service's asynchronous acknowledgement");
    waitFor([&] { return drained == 1; }, "Settings shutdown follows the real platform drain");
    require(!controller && read(store.path()) == before,
        "Shutdown and synchronous host destruction preserve the existing configuration bytes");
    settle();
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir directory; require(directory.isValid(), "Create an isolated actual settings directory");
        queuedBindings(); initialization(); settings(directory.path());
        std::cout << "PASS real shortcut admission, initialization and settings shutdown order with synchronous owner deletion\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
