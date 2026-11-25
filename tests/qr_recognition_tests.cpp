#include "core/QrRecognition.h"
#include "platform/QrText.h"
#include "fixtures/qr/qr-image.h"

#include "quirc/quirc_internal.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Core = HuhuCapture::Core;
namespace Platform = HuhuCapture::Platform;
namespace Fixtures = HuhuCapture::TestQrFixtures;

namespace {

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "qr_recognition_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

Core::QrDetection recognize(const Fixtures::Image& image)
{
    Core::QrDetection result;
    CHECK(Core::recognizeQr(image.view(), result) == Core::QrError::None);
    return result;
}

#ifdef _WIN32
std::u16string nativeAnsi(std::string_view bytes)
{
    const int length = static_cast<int>(bytes.size());
    const int count = MultiByteToWideChar(CP_ACP, 0, bytes.data(), length, nullptr, 0);
    CHECK(count > 0);
    std::wstring text(static_cast<std::size_t>(count), L'\0');
    CHECK(MultiByteToWideChar(CP_ACP, 0, bytes.data(), length, text.data(), count) == count);
    return {text.begin(), text.end()};
}
#endif

void realPayloadsAndText()
{
    for (const auto& source : Fixtures::fixtures) {
        const auto image = Fixtures::render(source, 0, false, 13);
        const auto detection = recognize(image);
        if (detection.codes.size() != 1) std::cerr << "fixture=" << source.name << " candidates=" << detection.candidates << '\n';
        CHECK(detection.codes.size() == 1 && detection.candidates == 1 && detection.undecodableCandidates == 0);
        const auto& code = detection.codes.front();
        CHECK(Fixtures::payloadEquals(code, source.payload));
        CHECK(code.dataType == source.dataType && code.eci == source.eci);
        CHECK(code.version >= 1 && code.version <= 40 && code.eccLevel == QUIRC_ECC_LEVEL_H && code.mask == 2);
        CHECK(!code.mirrored);
        for (const auto corner : code.corners)
            CHECK(corner.x >= 0 && corner.y >= 0 && corner.x <= image.width && corner.y <= image.height);
        Platform::QrTextDocument text;
        CHECK(Platform::decodeQrText(detection, text) == Core::QrError::None && text.codes.size() == 1);
        const auto& converted = text.codes.front();
#ifdef _WIN32
        if (source.name == "legacy_unknown" || source.name == "eci_unsupported" || source.name == "eci_shift_jis" || source.name == "eci_latin15") {
            CHECK(converted.readable() && converted.encoding == Platform::QrTextEncoding::WindowsAnsi);
            CHECK(converted.codePage == GetACP() && converted.text == nativeAnsi(source.payload));
            CHECK(text.joined == converted.text && text.unreadable == 0);
        } else
#else
        if (source.name == "legacy_unknown" || source.name == "eci_unsupported") {
            CHECK(!converted.readable() && converted.text.empty() && text.joined.empty() && text.unreadable == 1);
            CHECK(converted.status == (source.name == "legacy_unknown" ? Platform::QrTextStatus::UnknownEncoding : Platform::QrTextStatus::UnsupportedEncoding));
            CHECK(Fixtures::payloadEquals(code, source.payload));
        } else
#endif
        {
            CHECK(converted.readable() && converted.text == source.text && text.joined == source.text && text.unreadable == 0);
            CHECK(converted.status == (source.payload.empty() ? Platform::QrTextStatus::Empty : Platform::QrTextStatus::Decoded));
        }
        if (source.name == "kanji" || source.name == "cp932_wave_dash")
            CHECK(converted.encoding == Platform::QrTextEncoding::Windows932 && converted.codePage == 932);
        if (source.name == "nul") CHECK(text.joined.size() == 7 && text.joined[3] == u'\0');
        std::cout << "PASS real QR " << source.name << '\n';
    }
}

void transformationsAndEcc()
{
    const auto& source = Fixtures::fixture("utf8");
    for (int rotation = 0; rotation < 4; ++rotation) for (const bool mirrored : {false, true}) {
        const auto image = Fixtures::render(source, rotation, mirrored);
        const auto result = recognize(image);
        CHECK(result.codes.size() == 1 && Fixtures::payloadEquals(result.codes.front(), source.payload));
        CHECK(result.codes.front().mirrored == mirrored);
    }
    auto damaged = Fixtures::render(source);
    const int module = source.size - 1;
    const auto ink = source.modules[static_cast<std::size_t>(module * source.size + module)] == '1' ? 255 : 0;
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 6; ++x)
        damaged.pixel((module + 4) * 6 + x, (module + 4) * 6 + y,
            {static_cast<std::uint8_t>(ink), static_cast<std::uint8_t>(ink), static_cast<std::uint8_t>(ink), 255});
    const auto repaired = recognize(damaged);
    CHECK(repaired.codes.size() == 1 && Fixtures::payloadEquals(repaired.codes.front(), source.payload));
    CHECK(!repaired.codes.front().mirrored);
    std::cout << "PASS rotations, mirrored ECC retry and real data-cell correction\n";
}

void multipleAndWeightedGray()
{
    const auto& first = Fixtures::fixture("ascii");
    const auto& second = Fixtures::fixture("utf8");
    Fixtures::Image combined(620, 330, 7);
    Fixtures::paint(combined, first, 0, 0);
    Fixtures::paint(combined, second, 310, 0);
    const auto result = recognize(combined);
    std::cout << "multi candidates=" << result.candidates << ", decoded=" << result.codes.size()
        << ", undecodable=" << result.undecodableCandidates << '\n';
    // quirc may group finder patterns across the two real codes into an extra
    // undecodable candidate. The old command skips it and keeps both payloads.
    CHECK(result.codes.size() == 2 && result.candidates == result.codes.size() + result.undecodableCandidates);
    CHECK(Fixtures::payloadEquals(result.codes[0], first.payload) && Fixtures::payloadEquals(result.codes[1], second.payload));
    Platform::QrTextDocument text;
    CHECK(Platform::decodeQrText(result, text) == Core::QrError::None);
    CHECK(text.joined == std::u16string(first.text) + u'\n' + std::u16string(second.text));

    auto colored = Fixtures::render(first);
    for (int y = 0; y < colored.height; ++y) for (int x = 0; x < colored.width; ++x) {
        const bool ink = colored.pixels[static_cast<std::size_t>(y) * colored.stride + static_cast<std::size_t>(x) * 4] == 0;
        // The original BGRA weights make blue=29 and red=76. Swapping R/B or
        // compositing the intentionally transparent ink would lose the code.
        colored.pixel(x, y, ink ? std::array<std::uint8_t, 4>{255, 0, 0, 0} : std::array<std::uint8_t, 4>{0, 0, 255, 255});
    }
    const auto weighted = recognize(colored);
    CHECK(weighted.codes.size() == 1 && Fixtures::payloadEquals(weighted.codes[0], first.payload));
    std::cout << "PASS discovery-order multi-code join and original BGRA/alpha semantics\n";
}

void budgetsAndTransactionalFailure()
{
    CHECK(QUIRC_MAX_REGIONS == 65534 && !QUIRC_PIXEL_ALIAS_IMAGE && sizeof(quirc_pixel_t) == 2);
    CHECK(Core::qrWorkingBytes(320, 240) == sizeof(quirc) + std::size_t{320 * 240 * 3} +
        std::size_t{240 * 2 / 3} * sizeof(quirc_flood_fill_vars));
    CHECK(!Core::qrWorkingBytes(0, 100) && !Core::qrWorkingBytes(-1, 100));
    CHECK(!Core::qrWorkingBytes((std::numeric_limits<int>::max)(), 2));
    const auto image = Fixtures::render(Fixtures::fixture("ascii"), 0, false, 13);
    auto output = recognize(image);
    const auto preserved = output.codes[0].payload;
    const auto unchanged = [&] { CHECK(output.codes.size() == 1 && output.codes[0].payload == preserved); };
    auto shortView = image.view(); shortView.bgra = shortView.bgra.first(shortView.bgra.size() - 14);
    CHECK(Core::recognizeQr(shortView, output) == Core::QrError::InvalidInput); unchanged();
    auto badStride = image.view(); badStride.stride = static_cast<std::size_t>(image.width) * 4 - 1;
    CHECK(Core::recognizeQr(badStride, output) == Core::QrError::InvalidInput); unchanged();
    badStride.stride = (std::numeric_limits<std::size_t>::max)();
    CHECK(Core::recognizeQr(badStride, output) == Core::QrError::InvalidInput); unchanged();
    Core::QrLimits limits{*Core::qrWorkingBytes(image.width, image.height) - 1};
    CHECK(Core::recognizeQr(image.view(), output, limits) == Core::QrError::BudgetExceeded); unchanged();
    ++limits.maximumWorkingBytes;
    CHECK(Core::recognizeQr(image.view(), output, limits) == Core::QrError::None); unchanged();
    std::stop_source cancelled; cancelled.request_stop();
    CHECK(Core::recognizeQr(image.view(), output, {}, cancelled.get_token()) == Core::QrError::Cancelled); unchanged();
    Platform::QrTextDocument words; words.joined = u"retained";
    CHECK(Platform::decodeQrText(output, words, cancelled.get_token()) == Core::QrError::Cancelled && words.joined == u"retained");
    Fixtures::Image blank(80, 80);
    CHECK(Core::recognizeQr(blank.view(), output) == Core::QrError::None && output.codes.empty() && output.candidates == 0);
    std::cout << "PASS actual work budget, stride bounds, cancellation and unchanged failure output\n";
}

void largeBrightScreenshot()
{
    Fixtures::Image image(8192, 4096);
    // A valid desktop-sized input. The white histogram alone exceeds the old
    // unsigned 255*count product. Original otsu chooses 99 and erases gray=100
    // QR modules; the pinned upstream floating multiplication retains them.
    for (int y = 1024; y < 2024; ++y) for (int x = 1024; x < 2024; ++x) image.pixel(x, y, {0, 0, 0, 255});
    const auto& source = Fixtures::fixture("ascii");
    Fixtures::paint(image, source, 100, 100, 6, 0, false, 100);
    const auto result = recognize(image);
    CHECK(result.codes.size() == 1 && Fixtures::payloadEquals(result.codes[0], source.payload));
    std::cout << "PASS real 8192x4096 bright screenshot QR after upstream Otsu overflow fix\n";
    std::fill(image.pixels.begin(), image.pixels.end(), 255);
    // A normal zoomed QR on the right-hand screen of an 8K desktop. The old
    // line-intersection numerator overflowed int for this real image layout.
    Fixtures::paint(image, source, 5000, 1000, 70);
    const auto zoomed = recognize(image);
    CHECK(zoomed.codes.size() == 1 && Fixtures::payloadEquals(zoomed.codes[0], source.payload));
    for (const auto corner : zoomed.codes[0].corners)
        CHECK(corner.x >= 5000 && corner.x < image.width && corner.y >= 1000 && corner.y < image.height);
    std::cout << "PASS real 8K screenshot with a large QR away from the origin\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--large-only") { largeBrightScreenshot(); return 0; }
    realPayloadsAndText(); transformationsAndEcc(); multipleAndWeightedGray(); budgetsAndTransactionalFailure(); largeBrightScreenshot();
    return 0;
}
