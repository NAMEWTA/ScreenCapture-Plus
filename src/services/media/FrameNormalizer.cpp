/* The tiled 32-bit transpose traversal is adapted from FFmpeg 9.0.2
 * libavfilter/vf_transpose.c, commit 946fcce07b6dcd0331c8cc609192aeff5e1924f8.
 * Copyright (c) 2010 Stefano Sabatini; Copyright (c) 2008 Vitor Sessak.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Exact source, original license and adaptation: third_party/ffmpeg-transpose.
 */
#include "services/media/FrameNormalizer.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace HuhuCapture::Media {
namespace {
struct Point { int x{}, y{}; };

Point inputPoint(int x, int y, int width, int height, Core::FrameTransform transform) noexcept
{
    const auto value = static_cast<unsigned>(transform);
    Point point;
    switch (value % 4) {
    case 0: point = {x, y}; break;
    case 1: point = {width - 1 - y, x}; break;
    case 2: point = {width - 1 - x, height - 1 - y}; break;
    case 3: point = {y, height - 1 - x}; break;
    }
    if (value >= 4) point.x = width - 1 - point.x;
    return point;
}

// Upstream's transpose_block_32_c and 8x8 traversal, generalized to signed
// horizontal/vertical input steps. memcpy preserves unaligned source validity;
// no uint32_t alias/alignment assumption is imposed on an owned FrameLease.
void copyBlock(const std::uint8_t* source, std::ptrdiff_t xStep, std::ptrdiff_t yStep,
    std::uint8_t* destination, std::ptrdiff_t destinationStride, int width, int height,
    bool rgba, bool opaque) noexcept
{
    for (int y = 0; y < height; ++y) {
        const auto* input = source + static_cast<std::ptrdiff_t>(y) * yStep;
        auto* output = destination + static_cast<std::ptrdiff_t>(y) * destinationStride;
        for (int x = 0; x < width; ++x) {
            auto* pixel = output + x * 4;
            std::memcpy(pixel, input + static_cast<std::ptrdiff_t>(x) * xStep, 4);
            if (rgba) std::swap(pixel[0], pixel[2]);
            if (opaque) pixel[3] = 255;
        }
    }
}
}

std::optional<FrameExtent> normalizedFrameExtent(const Core::FrameFormat& format,
    const Core::FrameMetadata& metadata) noexcept
{
    if (!Core::validFrameFormat(format) || !Core::validFrameMetadata(metadata, format)) return {};
    int width = metadata.crop ? static_cast<int>(metadata.crop->width) : format.width;
    int height = metadata.crop ? static_cast<int>(metadata.crop->height) : format.height;
    if (static_cast<unsigned>(metadata.transform) % 2) std::swap(width, height);
    return FrameExtent{width, height};
}

FrameNormalizer::FrameNormalizer(Core::FramePoolLimits limits) : pool_(limits) {}

NormalizedFrame FrameNormalizer::normalize(const Core::FrameLease& source,
    std::optional<Core::FrameCrop> selection) noexcept
{
    using Error = FrameNormalizationError;
    if (!source) return {{}, Error::InvalidFrame};
    const auto& input = source.format();
    const auto& metadata = source.metadata();
    const auto extent = normalizedFrameExtent(input, metadata);
    if (!extent) return {{}, Error::InvalidFrame};
    if (input.pixelFormat != Core::FramePixelFormat::Bgra8 && input.pixelFormat != Core::FramePixelFormat::Rgba8)
        return {{}, Error::UnsupportedFormat};
    const auto selected = selection.value_or(Core::FrameCrop{0, 0,
        static_cast<std::uint32_t>(extent->width), static_cast<std::uint32_t>(extent->height)});
    if (!selected.width || !selected.height || selected.x > static_cast<unsigned>(extent->width) ||
        selected.width > static_cast<unsigned>(extent->width) - selected.x ||
        selected.y > static_cast<unsigned>(extent->height) || selected.height > static_cast<unsigned>(extent->height) - selected.y)
        return {{}, Error::InvalidSelection};
    const auto output = Core::packedFrameFormat(Core::FramePixelFormat::Bgra8,
        static_cast<int>(selected.width), static_cast<int>(selected.height), input.color, input.alpha);
    if (!output) return {{}, Error::InvalidFrame};
    if (!format_ || *format_ != *output) {
        // Retained old-generation leases still count toward the real pool
        // budget. Never reuse their storage after a source/selection change.
        pool_.clear(); format_.reset();
        const auto status = pool_.configure(*output);
        if (status != Core::FramePoolStatus::Ready)
            return {{}, status == Core::FramePoolStatus::AllocationFailed ? Error::Allocation : Error::Budget};
        format_ = *output;
    }
    auto writable = pool_.tryAcquire();
    if (!writable) return {{}, Error::Busy};
    const auto crop = metadata.crop.value_or(Core::FrameCrop{0, 0,
        static_cast<std::uint32_t>(input.width), static_cast<std::uint32_t>(input.height)});
    const auto first = inputPoint(static_cast<int>(selected.x), static_cast<int>(selected.y),
        static_cast<int>(crop.width), static_cast<int>(crop.height), metadata.transform);
    // Derive one-pixel strides from the affine transform. These adjacent points
    // determine a direction even for a one-pixel result; they are never read.
    const auto right = inputPoint(static_cast<int>(selected.x) + 1, static_cast<int>(selected.y),
        static_cast<int>(crop.width), static_cast<int>(crop.height), metadata.transform);
    const auto below = inputPoint(static_cast<int>(selected.x), static_cast<int>(selected.y) + 1,
        static_cast<int>(crop.width), static_cast<int>(crop.height), metadata.transform);
    const auto stride = input.planes[0].stride;
    const auto xStep = (right.x - first.x) * std::ptrdiff_t{4} + (right.y - first.y) * stride;
    const auto yStep = (below.x - first.x) * std::ptrdiff_t{4} + (below.y - first.y) * stride;
    const auto* start = source.row(0, crop.y + static_cast<std::uint32_t>(first.y)).data() +
        (static_cast<std::size_t>(crop.x) + static_cast<unsigned>(first.x)) * 4;
    auto* destination = writable->row(0, 0).data();
    const auto destinationStride = output->planes[0].stride;
    const bool rgba = input.pixelFormat == Core::FramePixelFormat::Rgba8;
    const bool opaque = input.alpha == Core::FrameAlpha::Opaque;
    if (xStep == 4 && !rgba) {
        // Common native frame/crop path is a row copy, including negative input
        // stride/vertical reflection. No temporary complete image is created.
        for (int y = 0; y < output->height; ++y) {
            auto row = writable->row(0, static_cast<std::uint32_t>(y));
            std::memcpy(row.data(), start + static_cast<std::ptrdiff_t>(y) * yStep, row.size());
            if (opaque) for (std::size_t x = 3; x < row.size(); x += 4) row[x] = 255;
        }
    } else {
        for (int y = 0; y < output->height; y += 8) for (int x = 0; x < output->width; x += 8) {
            copyBlock(start + static_cast<std::ptrdiff_t>(y) * yStep + static_cast<std::ptrdiff_t>(x) * xStep,
                xStep, yStep, destination + static_cast<std::ptrdiff_t>(y) * destinationStride + x * 4,
                destinationStride, std::min(8, output->width - x), std::min(8, output->height - y), rgba, opaque);
        }
    }
    auto normalizedMetadata = metadata;
    normalizedMetadata.crop.reset(); normalizedMetadata.transform = Core::FrameTransform::None;
    return {std::move(*writable).publish(std::move(normalizedMetadata)), Error::None};
}

} // namespace HuhuCapture::Media
