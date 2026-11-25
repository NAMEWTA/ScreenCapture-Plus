#include "services/recognition/QrService.h"
#include "fixtures/qr/qr-image.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <future>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace Core = HuhuCapture::Core;
namespace Platform = HuhuCapture::Platform;
namespace Recognition = HuhuCapture::Recognition;
namespace Fixtures = HuhuCapture::TestQrFixtures;

namespace {

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

QByteArray digest(const QImage& image)
{
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
        QCryptographicHash::Sha256);
}

QImage imageFor(std::string_view name)
{
    const auto source = Fixtures::render(Fixtures::fixture(name), 0, false, 12);
    const auto view = source.view();
    const QImage borrowed(view.bgra.data(), view.width, view.height, static_cast<qsizetype>(view.stride), QImage::Format_ARGB32);
    auto result = borrowed.copy();
    require(!result.isNull(), "own the actual generated QR pixels before dispatching a worker");
    return result;
}

Recognition::QrResult fromWorker(QImage image)
{
    return std::async(std::launch::async, [image = std::move(image)] {
        Recognition::QrResult output;
        require(Recognition::recognizeImage(image, output) == Core::QrError::None, "real QR recognition finishes on a worker");
        return output;
    }).get();
}

void formatsAndRawPixels()
{
    const auto original = imageFor("utf8");
    const auto& expected = Fixtures::fixture("utf8");
    const auto expectedText = QString::fromUtf16(expected.text.data(), static_cast<qsizetype>(expected.text.size()));
    for (const auto format : {QImage::Format_ARGB32, QImage::Format_RGB32, QImage::Format_RGB888,
        QImage::Format_RGBA8888, QImage::Format_Grayscale8, QImage::Format_Grayscale16,
        QImage::Format_ARGB32_Premultiplied, QImage::Format_Indexed8}) {
        auto image = original.convertToFormat(format);
        image.setDevicePixelRatio(1.75);
        require(!image.isNull(), "the real Qt image-format conversion is available");
        const auto before = digest(image);
        const auto result = fromWorker(image);
        require(result.detection.codes.size() == 1 && Fixtures::payloadEquals(result.detection.codes[0], expected.payload),
            "each real QImage format reaches the same complete raw QR payload");
        require(result.text == expectedText && result.decoded.unreadable == 0, "Chinese and non-BMP UTF-16 text survives Qt adaptation");
        require(digest(image) == before && image.devicePixelRatio() == 1.75, "recognition never changes source pixels or DPI metadata");
    }
    // A real PNG encoder/decoder round trip, including transparent dark modules.
    QByteArray png; QBuffer buffer(&png); require(buffer.open(QIODevice::WriteOnly), "open bounded fixture buffer");
    require(original.save(&buffer, "PNG"), "the actual Qt PNG encoder writes the QR fixture");
    const auto decoded = QImage::fromData(png, "PNG"); require(!decoded.isNull(), "the actual Qt PNG decoder reads the QR fixture");
    require(fromWorker(decoded).text == expectedText, "QR recognition reads actual decoded PNG pixels");
    const auto nul = fromWorker(imageFor("nul"));
    require(nul.text.size() == 7 && nul.text[3] == QChar::Null, "a valid embedded NUL is not truncated by UTF-16 adaptation");
    std::cout << "PASS eight real Qt image formats, PNG round trip, DPI and complete Unicode text\n";
}

void normalizationBudgetAndFailure()
{
    const auto native = imageFor("ascii");
    const auto work = *Core::qrWorkingBytes(native.width(), native.height());
    Recognition::QrResult output;
    require(Recognition::recognizeImage(native, output, {work}) == Core::QrError::None,
        "native BGRA needs the exact decoder budget without a normalization allocation");
    const auto preservedText = output.text;
    const auto preservedPayload = output.detection.codes[0].payload;
    const auto unchanged = [&] {
        require(output.text == preservedText && output.detection.codes.size() == 1 &&
            output.detection.codes[0].payload == preservedPayload && output.decoded.codes.size() == 1,
            "failure preserves every prior result layer");
    };
    const auto rgb = native.convertToFormat(QImage::Format_RGB888);
    const auto normalization = static_cast<std::size_t>(native.width()) * static_cast<std::size_t>(native.height()) * 4;
    require(Recognition::recognizeImage(rgb, output, {work + normalization - 1}) == Core::QrError::BudgetExceeded,
        "normalization must be included before Qt allocates the additional BGRA image"); unchanged();
    require(Recognition::recognizeImage(rgb, output, {work + normalization}) == Core::QrError::None,
        "the exact decoder plus normalization budget admits the real image"); unchanged();
    require(Recognition::recognizeImage(QImage{}, output) == Core::QrError::InvalidInput,
        "null images do not overwrite the earlier result"); unchanged();
    std::stop_source cancelled; cancelled.request_stop();
    require(Recognition::recognizeImage(rgb, output, {}, cancelled.get_token()) == Core::QrError::Cancelled,
        "a cancelled job does not publish partial recognition"); unchanged();
    QImage blank(80, 80, QImage::Format_RGB32); blank.fill(Qt::white);
    require(Recognition::recognizeImage(blank, output) == Core::QrError::None && output.text.isEmpty() && output.detection.codes.empty(),
        "a completed no-code result is distinct from failed recognition");
    std::cout << "PASS real normalization budget, cancellation and transactional result ownership\n";
}

void unknownEncodingRetainsBytes()
{
    const auto result = fromWorker(imageFor("legacy_unknown"));
    require(result.detection.codes.size() == 1 && Fixtures::payloadEquals(result.detection.codes[0], Fixtures::fixture("legacy_unknown").payload),
        "all platforms retain the exact non-UTF-8 raw payload");
    require(result.decoded.codes.size() == 1, "every raw code has one explicit text result");
#ifdef Q_OS_WIN
    require(result.decoded.codes[0].encoding == Platform::QrTextEncoding::WindowsAnsi && result.decoded.codes[0].readable(),
        "Windows preserves the existing actual ACP conversion");
#else
    require(result.text.isEmpty() && result.decoded.unreadable == 1 &&
        result.decoded.codes[0].status == Platform::QrTextStatus::UnknownEncoding,
        "Linux does not claim unknown Windows ACP bytes are readable text");
#endif
    std::cout << "PASS raw payload preservation and explicit platform encoding boundary\n";
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        formatsAndRawPixels(); normalizationBudgetAndFailure(); unknownEncodingRetainsBytes();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
