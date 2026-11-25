#include "services/desktop/SystemClipboard.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void events() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
void waitFor(const std::function<bool()>& predicate, const char* message, int milliseconds = 8000)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < milliseconds) { events(); QThread::msleep(2); }
    require(predicate(), message);
}
class Origin final : public QWidget {
public:
    std::function<void()> action;
    Origin() { setWindowTitle(QStringLiteral("huhucapture clipboard validation")); resize(320, 180); }
protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_F8 && !event->isAutoRepeat()) {
            if (action) { auto callback = std::move(action); callback(); }
            event->accept(); return;
        }
        QWidget::keyPressEvent(event);
    }
};
void show(QWidget& widget)
{ widget.show(); widget.raise(); widget.activateWindow(); widget.setFocus(); events(); }
QImage pixels(int salt = 0)
{
    QImage image(17, 9, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
        image.setPixel(x, y, qRgba((x * 13 + salt) % 256, (y * 23 + salt) % 256, (x + y * 3 + salt) % 256, ((x + y) % 4) * 85));
    return image;
}
void equalPixels(const QImage& image, const QImage& expected)
{
    require(!image.isNull() && image.size() == expected.size(), "the actual clipboard PNG preserves physical dimensions");
    const auto normalized = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
        require(normalized.pixel(x, y) == expected.pixel(x, y), "the receiver observes exact straight-alpha PNG pixels");
}

#ifdef Q_OS_WIN
class ClipboardLock final {
public:
    ClipboardLock()
    {
        waitFor([&] { return opened_ || (opened_ = OpenClipboard(nullptr) != FALSE); }, "the receiving process opens the real Windows clipboard", 3000);
    }
    ~ClipboardLock() { if (opened_) CloseClipboard(); }
private: bool opened_{};
};
class GlobalRead final {
public:
    explicit GlobalRead(UINT format)
    {
        handle_ = static_cast<HGLOBAL>(GetClipboardData(format));
        require(handle_ != nullptr, "the real Windows clipboard exposes a required format");
        size = GlobalSize(handle_); data = static_cast<const unsigned char*>(GlobalLock(handle_));
        require(data != nullptr, "lock the actual transferred clipboard memory");
    }
    ~GlobalRead() { if (data) GlobalUnlock(handle_); }
    const unsigned char* data{}; SIZE_T size{};
private: HGLOBAL handle_{};
};
void receiveImage(const QImage& expected)
{
    ClipboardLock lock;
    const GlobalRead png(RegisterClipboardFormatW(L"PNG"));
    equalPixels(QImage::fromData(png.data, static_cast<int>(png.size), "png"), expected);
    const GlobalRead alpha(CF_DIBV5); require(alpha.size >= sizeof(BITMAPV5HEADER), "DIBV5 has its complete header");
    const auto* v5 = reinterpret_cast<const BITMAPV5HEADER*>(alpha.data);
    require(v5->bV5Width == expected.width() && v5->bV5Height == -expected.height() && v5->bV5BitCount == 32 &&
        v5->bV5Compression == BI_BITFIELDS && v5->bV5AlphaMask == 0xff000000 && v5->bV5CSType == LCS_sRGB,
        "DIBV5 keeps top-down BGRA, explicit alpha and sRGB");
    require(alpha.size >= sizeof(BITMAPV5HEADER) + static_cast<SIZE_T>(expected.width() * expected.height() * 4), "DIBV5 pixel buffer is complete");
    for (int y = 0; y < expected.height(); ++y) for (int x = 0; x < expected.width(); ++x) {
        const auto* value = alpha.data + sizeof(BITMAPV5HEADER) + (y * expected.width() + x) * 4;
        const auto pixel = expected.pixel(x, y);
        require(value[0] == qBlue(pixel) && value[1] == qGreen(pixel) && value[2] == qRed(pixel) && value[3] == qAlpha(pixel), "real DIBV5 retains every alpha byte");
    }
    const GlobalRead legacy(CF_DIB); require(legacy.size >= sizeof(BITMAPINFOHEADER), "DIB has its complete header");
    const auto* dib = reinterpret_cast<const BITMAPINFOHEADER*>(legacy.data);
    const auto stride = (expected.width() * 3 + 3) & ~3;
    require(dib->biWidth == expected.width() && dib->biHeight == expected.height() && dib->biBitCount == 24 && dib->biCompression == BI_RGB &&
        legacy.size >= sizeof(BITMAPINFOHEADER) + static_cast<SIZE_T>(stride * expected.height()), "legacy DIB is real padded bottom-up 24-bit RGB");
    for (int y = 0; y < expected.height(); ++y) for (int x = 0; x < expected.width(); ++x) {
        const auto* value = legacy.data + sizeof(BITMAPINFOHEADER) + (expected.height() - 1 - y) * stride + x * 3;
        const auto pixel = expected.pixel(x, y);
        require(value[0] == qBlue(pixel) && value[1] == qGreen(pixel) && value[2] == qRed(pixel), "legacy receiving applications get correctly oriented BGR pixels");
    }
}
void receiveFile(const QString& expected)
{
    ClipboardLock lock;
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    require(drop && DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1, "real CF_HDROP contains exactly one history file");
    const auto length = DragQueryFileW(drop, 0, nullptr, 0);
    std::wstring path(length + 1, L'\0'); require(DragQueryFileW(drop, 0, path.data(), length + 1) == length, "read the complete wide file-drop path");
    path.resize(length);
    require(QDir::fromNativeSeparators(QString::fromStdWString(path)) == QDir::fromNativeSeparators(expected), "CF_HDROP preserves the full Unicode media path");
}
#else
void receiveImage(const QImage& expected)
{
    auto* clipboard = QApplication::clipboard();
    waitFor([&] { return clipboard->mimeData()->hasFormat(QStringLiteral("image/png")); }, "the separate process receives an actual PNG offer");
    const auto bytes = clipboard->mimeData()->data(QStringLiteral("image/png"));
    equalPixels(QImage::fromData(bytes, "png"), expected);
}
void receiveFile(const QString& expected)
{
    auto* clipboard = QApplication::clipboard();
    waitFor([&] { return clipboard->mimeData()->hasUrls(); }, "the separate process receives an actual file URI offer");
    const auto urls = clipboard->mimeData()->urls();
    require(urls.size() == 1 && urls.front().isLocalFile() && urls.front().toLocalFile() == expected,
        "the file-manager receiver gets the complete Unicode local URI");
}
#endif

void receiver(const QString& kind, const QString& expected)
{
    Origin window; show(window);
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))
        waitFor([&] { return QGuiApplication::focusWindow() == window.windowHandle(); }, "the compositor focuses the actual receiver window");
    if (kind == QStringLiteral("--read-image")) receiveImage(QImage(expected)); else receiveFile(expected);
}
void runReceiver(const QString& kind, const QString& expected)
{
    QProcess child; child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {kind, expected});
    require(child.waitForStarted(2000), "start an actual independent clipboard receiver");
    waitFor([&] { return child.state() == QProcess::NotRunning; }, "the real receiver completes while the owner pumps its event loop");
    const auto output = child.readAll(); std::cout << output.constData();
    require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, "the separate process validates the actual system clipboard");
}

template<class Function> auto worker(Function function) { return std::async(std::launch::async, std::move(function)).get(); }

class Results final {
public:
    Desktop::SystemClipboard clipboard;
    std::map<quint64, Desktop::ClipboardResult> values;
    Results()
    {
        QObject::connect(&clipboard, &Desktop::SystemClipboard::finished, &clipboard, [this](quint64 token, const Desktop::ClipboardResult& result) {
            require(!values.contains(token), "every clipboard request has exactly one terminal result"); values.emplace(token, result);
        });
    }
    quint64 request(const std::shared_ptr<Desktop::PreparedClipboard>& content, Origin& origin)
    {
        quint64 token{};
        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
            origin.action = [&] { token = clipboard.publish(content, origin.windowHandle()); };
            std::cout << "NEED_INPUT\n" << std::flush;
            waitFor([&] { return token != 0; }, "the owned compositor delivers a real F8 input serial");
        } else token = clipboard.publish(content, origin.windowHandle());
        return token;
    }
    Desktop::ClipboardResult result(quint64 token)
    { waitFor([&] { return values.contains(token); }, "actual clipboard publication completes asynchronously"); return values.at(token); }
};

Media::Entry entryAt(Media::MediaArchiveService& archive, const QString& path)
{
    const auto list = worker([&] { return archive.list(); }); require(!list.failure, "enumerate the real archive");
    for (const auto& entry : list.entries) if (entry.path == path) return entry;
    throw std::runtime_error("the real media file is absent from history");
}

void contracts(const QString& root)
{
    Results results; auto origin = std::make_unique<Origin>(); show(*origin);
    const auto image = pixels(); const auto expected = root + QStringLiteral("/expected.png"); require(image.save(expected), "save exact receiver reference pixels");
    auto prepared = worker([&] { return Desktop::prepareImage(image); }); require(!prepared.failure && prepared.content, "prepare actual image clipboard formats on a worker");
    const auto result = results.result(results.request(prepared.content, *origin));
    require(result.completed(), "actual platform receipt confirms image publication"); runReceiver(QStringLiteral("--read-image"), expected);
    require(!results.result(results.clipboard.publish(prepared.content, origin->windowHandle())).completed(), "a consumed preparation cannot transfer the same native handles again");
    require(!worker([&] { return Desktop::prepareImage(image, {8}); }).content, "pixel budget is enforced before publication");
    std::stop_source stopped; stopped.request_stop();
    require(worker([&] { return Desktop::prepareImage(image, {}, stopped.get_token()); }).failure.code == Desktop::Error::Cancelled, "cancelled preparation never touches the system clipboard");
    auto cancelled = worker([&] { return Desktop::prepareImage(pixels(90)); });
    const auto cancelledToken = results.clipboard.publish(cancelled.content, origin->windowHandle()); results.clipboard.cancel(cancelledToken);
    require(results.result(cancelledToken).failure.code == Desktop::Error::Cancelled, "queued publication can be cancelled before the system handoff");
    runReceiver(QStringLiteral("--read-image"), expected);

#ifdef Q_OS_WIN
    QProcess holder; holder.setProcessChannelMode(QProcess::MergedChannels);
    holder.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-clipboard")});
    require(holder.waitForStarted(2000), "start actual competing clipboard owner");
    QByteArray holderOutput;
    waitFor([&] { holderOutput += holder.readAll(); return holderOutput.contains("HELD"); }, "the other process holds the Windows clipboard open");
    auto blocked = worker([&] { return Desktop::prepareImage(pixels(33)); });
    const auto busy = results.result(results.request(blocked.content, *origin));
    holder.write("release\n"); holder.closeWriteChannel();
    waitFor([&] { return holder.state() == QProcess::NotRunning; }, "the competing process releases the clipboard");
    require(busy.failure.code == Desktop::Error::Busy && busy.publishedFormats.isEmpty(), "real OpenClipboard contention reports Busy, never false success");
    require(holder.exitCode() == 0, "actual clipboard holder exits cleanly");
    runReceiver(QStringLiteral("--read-image"), expected);
#endif
    Media::MediaArchiveService archive(root + QStringLiteral("/历史 archive"));
    const auto saved = worker([&] { return archive.archiveImage(image); }); require(saved.durable(), "archive real PNG for history clipboard testing");
    auto imageEntry = entryAt(archive, saved.path);
    auto lease = worker([&] { return archive.lease(imageEntry); }); require(!lease.failure && lease.lease, "reserve an actual PNG read lease");
    auto history = worker([owned = std::move(lease.lease)]() mutable { return Desktop::prepareFile(std::move(owned)); });
    require(!history.failure && history.content, "prepare actual archived PNG as pixels");
    require(worker([&] { return archive.reserveRemoval(imageEntry); }).failure.code == Media::Error::Busy, "prepared history retains its read lease until publication");
    require(results.result(results.request(history.content, *origin)).completed(), "archived PNG is actually published");
    { const auto removal = worker([&] { return archive.reserveRemoval(imageEntry); }); require(!removal.failure && removal.lease, "publication releases the read lease even while its prepared object and clipboard image still live"); }
    runReceiver(QStringLiteral("--read-image"), expected);

    const auto gifPath = archive.directory() + QStringLiteral("/20000101-000001.gif");
    // A complete, decodable one-pixel GIF89a; this tests real file/URI transfer,
    // not GIF encoding or a mocked archive/clipboard implementation.
    { QFile file(gifPath); require(file.open(QIODevice::WriteOnly), "create a complete GIF media fixture");
      const auto bytes = QByteArray::fromHex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b");
      require(file.write(bytes) == bytes.size() && file.flush(), "write the complete real GIF"); }
    require(!QImage(gifPath).isNull(), "the GIF fixture really decodes");
    const auto gifEntry = entryAt(archive, gifPath);
    auto gifLease = worker([&] { return archive.lease(gifEntry); }); require(!gifLease.failure, "reserve the real GIF file");
    auto fileContent = worker([owned = std::move(gifLease.lease)]() mutable { return Desktop::prepareFile(std::move(owned)); });
    require(!fileContent.failure && fileContent.content, "prepare actual file clipboard data");
    require(results.result(results.request(fileContent.content, *origin)).completed(), "the platform really accepts the file clipboard");
    { const auto removal = worker([&] { return archive.reserveRemoval(gifEntry); }); require(!removal.failure && removal.lease, "a persistent clipboard URI never pins history against cleanup"); }
    runReceiver(QStringLiteral("--read-file"), gifPath);

    auto changedLease = worker([&] { return archive.lease(imageEntry); });
    auto changedContent = worker([owned = std::move(changedLease.lease)]() mutable { return Desktop::prepareFile(std::move(owned)); });
    require(!changedContent.failure, "prepare a real PNG before an external change");
    { QFile file(imageEntry.path); require(file.open(QIODevice::Append), "make a real external history modification"); require(file.write("changed") == 7 && file.flush(), "persist the external modification"); }
    require(results.result(results.request(changedContent.content, *origin)).failure.code == Desktop::Error::ChangedOnDisk,
        "the asynchronous final identity check rejects changed history before native publication");
    runReceiver(QStringLiteral("--read-file"), gifPath);

    auto finalImage = worker([&] { return Desktop::prepareImage(image); });
    require(results.result(results.request(finalImage.content, *origin)).completed(), "final real owner image publication");
    origin.reset(); events(); runReceiver(QStringLiteral("--read-image"), expected);
    std::cout << "PASS real independent image/file receiver, bounded preparation, cancellation, changed-file rejection and application-owned clipboard\n";
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        const auto arguments = app.arguments();
        if (arguments.size() >= 3 && (arguments[1] == QStringLiteral("--read-image") || arguments[1] == QStringLiteral("--read-file"))) {
            receiver(arguments[1], arguments[2]); std::cout << "PASS separate system clipboard receiver\n"; return 0;
        }
#ifdef Q_OS_WIN
        if (arguments.contains(QStringLiteral("--hold-clipboard"))) {
            ClipboardLock lock; std::cout << "HELD\n" << std::flush; std::string release; std::getline(std::cin, release); return 0;
        }
#endif
        if (arguments.contains(QStringLiteral("--reject-backend"))) {
            Results results; Origin origin; show(origin);
            auto prepared = Desktop::prepareImage(pixels()); const auto token = results.clipboard.publish(prepared.content, origin.windowHandle());
            require(results.result(token).failure.code == Desktop::Error::Unavailable, "offscreen must never masquerade as a working system clipboard");
            std::cout << "PASS unsupported QPA is an explicit clipboard failure\n"; return 0;
        }
        QTemporaryDir isolated; require(isolated.isValid(), "create isolated clipboard media directory");
        contracts(isolated.path()); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
