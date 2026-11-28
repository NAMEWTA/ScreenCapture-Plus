#include "services/media/FrameNormalizer.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
Core::FrameLease makeFrame(int width, int height, bool rgba = false, bool bottomUp = false,
    Core::FrameTransform transform = Core::FrameTransform::None,
    std::optional<Core::FrameCrop> crop = {}, Core::FrameAlpha alpha = Core::FrameAlpha::Opaque)
{
    auto format = *Core::packedFrameFormat(rgba ? Core::FramePixelFormat::Rgba8 : Core::FramePixelFormat::Bgra8,
        width, height, {Core::ColorPrimaries::DisplayP3, Core::ColorTransfer::Srgb,
            Core::ColorMatrix::Rgb, Core::ColorRange::Full}, alpha);
    const auto stride = static_cast<std::size_t>(width) * 4 + 12;
    format.bytes = 4 + stride * height;
    format.planes[0] = {4, stride * height, bottomUp ? stride * (height - 1) : 0,
        bottomUp ? -static_cast<std::ptrdiff_t>(stride) : static_cast<std::ptrdiff_t>(stride),
        static_cast<std::size_t>(width) * 4, static_cast<std::uint32_t>(height)};
    Core::FramePool pool({1, 1024 * 1024, 16384});
    require(pool.configure(format) == Core::FramePoolStatus::Ready, "allocate a real owned padded frame pool");
    auto writable = pool.tryAcquire();
    require(writable.has_value(), "acquire a real writable frame slot");
    std::fill(writable->plane(0).begin(), writable->plane(0).end(), 213);
    for (int y = 0; y < height; ++y) {
        auto row = writable->row(0, static_cast<std::uint32_t>(y));
        for (int x = 0; x < width; ++x) {
            const auto value = static_cast<std::uint8_t>(y * width + x + 1);
            const auto offset = static_cast<std::size_t>(x) * 4;
            row[offset] = rgba ? value + 80 : value;
            row[offset + 1] = value + 40;
            row[offset + 2] = rgba ? value : value + 80;
            row[offset + 3] = value + 120;
        }
    }
    Core::FrameMetadata metadata;
    metadata.sessionId = 27; metadata.sourceId = 31;
    metadata.timing.sourcePts = std::chrono::milliseconds(57);
    metadata.timing.sourceClock = Core::FrameClockDomain::PipeWire;
    metadata.timing.clockInstance = 27;
    metadata.timing.sourceSequence = 73;
    metadata.timing.receivedAt = Core::MediaClock::now();
    metadata.crop = crop; metadata.transform = transform;
    return std::move(*writable).publish(std::move(metadata));
}
std::vector<unsigned> values(const Core::FrameLease& frame)
{
    std::vector<unsigned> result;
    for (int y = 0; y < frame.format().height; ++y) {
        const auto row = frame.row(0, static_cast<std::uint32_t>(y));
        for (int x = 0; x < frame.format().width; ++x) {
            const auto offset = static_cast<std::size_t>(x) * 4;
            require(row[offset + 1] == row[offset] + 40 && row[offset + 2] == row[offset] + 80,
                "normalization preserves every color channel and does not sample scanline padding");
            require(row[offset + 3] == (frame.format().alpha == Core::FrameAlpha::Opaque ? 255 : row[offset] + 120),
                "only explicitly opaque input normalizes the unused alpha byte");
            result.push_back(row[offset]);
        }
    }
    return result;
}
void transformsAndOwnedStorage()
{
    const std::array<std::vector<unsigned>, 8> expected{{
        {1,2,3,4,5,6}, {2,4,6,1,3,5}, {6,5,4,3,2,1}, {5,3,1,6,4,2},
        {2,1,4,3,6,5}, {1,3,5,2,4,6}, {5,6,3,4,1,2}, {6,4,2,5,3,1}}};
    for (int mode = 0; mode < 4; ++mode) for (unsigned transform = 0; transform < expected.size(); ++transform) {
        auto source = makeFrame(2, 3, mode & 1, mode & 2, static_cast<Core::FrameTransform>(transform));
        const auto originalTime = source.metadata().timing.receivedAt;
        const auto originalColor = source.format().color;
        Media::NormalizedFrame normalized;
        {
            Media::FrameNormalizer normalizer;
            normalized = normalizer.normalize(source);
            require(normalized.error == Media::FrameNormalizationError::None && normalized.frame,
                "normalize all eight real frame transforms with either packed order and stride direction");
        }
        source.reset();
        require(values(normalized.frame) == expected[transform],
            "normalized pixels have the documented reflect-first counter-clockwise orientation");
        require(normalized.frame.format().width == (transform % 2 ? 3 : 2) &&
            normalized.frame.format().height == (transform % 2 ? 2 : 3), "quarter turns exchange the actual dimensions");
        require(normalized.frame.format().color == originalColor && normalized.frame.metadata().sessionId == 27 &&
            normalized.frame.metadata().sourceId == 31 && normalized.frame.metadata().timing.receivedAt == originalTime &&
            normalized.frame.metadata().timing.sourcePts == std::chrono::milliseconds(57) &&
            normalized.frame.metadata().timing.sourceSequence == 73 && !normalized.frame.metadata().crop &&
            normalized.frame.metadata().transform == Core::FrameTransform::None,
            "leases outlive producer/normalizer and retain source timing/color without a second transform");
    }
    std::cout << "PASS all eight orientations, RGBA/BGRA, padded/bottom-up storage and retained frame ownership\n";
}
void nativeAndUserCrop()
{
    auto source = makeFrame(5, 5, true, true, Core::FrameTransform::Rotate90,
        Core::FrameCrop{1, 1, 3, 2}, Core::FrameAlpha::Straight);
    require(Media::normalizedFrameExtent(source.format(), source.metadata()) == Media::FrameExtent{2, 3},
        "source extent applies the actual native crop before its transform");
    Media::FrameNormalizer normalizer;
    const auto whole = normalizer.normalize(source);
    require(whole.frame && values(whole.frame) == std::vector<unsigned>({9,14,8,13,7,12}),
        "the normalized source contains only the authorized native crop");
    const auto selected = normalizer.normalize(source, Core::FrameCrop{0,1,2,2});
    require(selected.frame && values(selected.frame) == std::vector<unsigned>({8,13,7,12}) &&
        selected.frame.format().alpha == Core::FrameAlpha::Straight,
        "a user selection is applied in displayed source pixels after native orientation, preserving straight alpha");
    std::cout << "PASS native crop then displayed-source selection with preserved alpha semantics\n";
}
void boundedPool()
{
    auto source = makeFrame(2, 3);
    Media::FrameNormalizer normalizer({2, 48, 16384});
    auto first = normalizer.normalize(source);
    auto second = normalizer.normalize(source);
    require(first.frame && second.frame && normalizer.allocatedBytes() == 48, "the actual output pool has two bounded frame slots");
    require(normalizer.normalize(source).error == Media::FrameNormalizationError::Busy &&
        values(first.frame) == std::vector<unsigned>({1,2,3,4,5,6}), "a retained consumer lease cannot be overwritten to produce another frame");
    first.frame.reset();
    require(static_cast<bool>(normalizer.normalize(source).frame), "releasing a frame makes exactly that pool slot reusable");
    require(normalizer.normalize(source, Core::FrameCrop{0,0,3,3}).error == Media::FrameNormalizationError::InvalidSelection &&
        values(second.frame) == std::vector<unsigned>({1,2,3,4,5,6}), "invalid selection does not invalidate a previously published frame");
    std::cout << "PASS bounded pool reuse without overwriting retained consumers\n";
}
}
int main()
{
    try { transformsAndOwnedStorage(); nativeAndUserCrop(); boundedPool(); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
