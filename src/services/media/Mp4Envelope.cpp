/*
 * MOV demuxer
 * Copyright (c) 2001 Fabrice Bellard
 * Copyright (c) 2009 Baptiste Coudurier <baptiste dot coudurier at gmail dot com>
 *
 * first version by Francois Revol <revol@free.fr>
 * seek function by Gael Chardon <gael.dev@4now.net>
 *
 * This file derives the top-level size/extended-size traversal from FFmpeg
 * 9.0.2 libavformat/mov.c, mov_probe and mov_read_default. The original notice
 * and LGPL-2.1-or-later license are retained in third_party/ffmpeg-mov/.
 * Application adaptations require each box to fit its physical file, bound
 * total non-media metadata, and check cancellation between fixed-size reads.
 * No sample table, codec or other MOV semantic parsing is implemented here.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "services/media/Mp4Envelope.h"

#include <array>
#include <cstring>

namespace HuhuCapture::Media::Detail {
namespace {
std::uint64_t bigEndian(std::span<const std::uint8_t> bytes)
{
    std::uint64_t value{};
    for (const auto byte : bytes) value = (value << 8) | byte;
    return value;
}
Failure invalid(const char* detail) { return {Error::InvalidMedia, QString::fromLatin1(detail)}; }
} // namespace

Failure checkMp4Envelope(std::uint64_t fileBytes, const Mp4HeaderReader& read,
    const Limits& limits, std::stop_token stop)
{
    if (stop.stop_requested()) return {Error::Cancelled, {}};
    if (!limits.maximumFileBytes || !limits.maximumImageBytes || fileBytes > limits.maximumFileBytes)
        return {Error::Budget, QStringLiteral("MP4 envelope exceeds the configured byte budget")};
    std::uint64_t offset{};
    std::uint64_t metadata{};
    bool movie = false;
    bool media = false;
    while (offset < fileBytes) {
        if (stop.stop_requested()) return {Error::Cancelled, {}};
        std::array<std::uint8_t, 8> header{};
        if (fileBytes - offset < header.size()) return invalid("MP4 ends inside a physical box header");
        if (!read(offset, header)) return {Error::Read, QStringLiteral("Cannot read an MP4 box header from the owned file")};
        auto size = bigEndian(std::span(header).first<4>());
        std::uint64_t minimum = 8;
        if (size == 1) {
            std::array<std::uint8_t, 8> extended{};
            minimum = 16;
            if (fileBytes - offset < minimum) return invalid("MP4 ends inside an extended-size box header");
            if (!read(offset + 8, extended)) return {Error::Read, QStringLiteral("Cannot read an extended MP4 box size")};
            size = bigEndian(extended);
        } else if (size == 0) {
            size = fileBytes - offset;
        }
        if (size < minimum || size > fileBytes - offset)
            return invalid("Declared MP4 box does not fit the physical file");
        const auto is = [&](const char* type) { return std::memcmp(header.data() + 4, type, 4) == 0; };
        if (is("moov")) movie = true;
        if (is("mdat")) media = true;
        // Media payload and explicitly free/skip padding are not loaded by this
        // check. Bound all other top-level metadata before a native parser can
        // allocate for a physically huge moov/moof/unknown metadata box.
        if (!is("mdat") && !is("free") && !is("skip")) {
            if (size > limits.maximumImageBytes - metadata)
                return {Error::Budget, QStringLiteral("MP4 metadata boxes exceed the byte budget")};
            metadata += size;
        }
        offset += size;
    }
    if (stop.stop_requested()) return {Error::Cancelled, {}};
    if (!movie || !media) return invalid("MP4 lacks a physical movie or media-data box");
    return {};
}
} // namespace HuhuCapture::Media::Detail
