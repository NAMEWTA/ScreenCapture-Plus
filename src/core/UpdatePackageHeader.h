#pragma once

#include "core/UpdatePolicy.h"

#include <array>
#include <charconv>
#include <span>
#include <string_view>

namespace HuhuCapture::Core {

// A bounded type guard shared by the Qt downloader and the old WinRT bridge.
// It does not unpack an archive or prove its runtime dependencies. The exact
// package hash is bound to build-info; full payload/installation verification
// remains a separate release gate. Reader returns bytes read at an offset.
template<class Reader>
bool updatePackageHeader(UpdatePackageKind kind, Reader read)
{
    std::array<std::uint8_t, 96> prefix{};
    const auto length = read(0, std::span(prefix));
    if (length > prefix.size()) return false;
    const std::string_view bytes(reinterpret_cast<const char*>(prefix.data()), length);
    const auto le16 = [](const std::uint8_t* data) {
        return static_cast<std::uint16_t>(data[0] | (std::uint16_t{data[1]} << 8));
    };
    switch (kind) {
    case UpdatePackageKind::WindowsPortableZip:
        return bytes.starts_with(std::string_view("PK\x03\x04", 4));
    case UpdatePackageKind::LinuxDeb: {
        if (length < 72 || !bytes.starts_with("!<arch>\n") || bytes.substr(66, 2) != "`\n" ||
            bytes.substr(68, 4) != "2.0\n") return false;
        const auto trim = [](std::string_view value) {
            const auto first = value.find_first_not_of(' '), last = value.find_last_not_of(' ');
            return first == std::string_view::npos ? std::string_view{} : value.substr(first, last - first + 1);
        };
        if (trim(bytes.substr(8, 16)) != "debian-binary") return false;
        const auto field = trim(bytes.substr(56, 10));
        if (field.empty()) return false;
        unsigned amount{};
        const auto parsed = std::from_chars(field.data(), field.data() + field.size(), amount);
        return parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size() && amount == 4;
    }
    case UpdatePackageKind::LinuxAppImage:
        return length >= 64 && bytes.starts_with(std::string_view("\x7f" "ELF", 4)) &&
            prefix[4] == 2 && prefix[5] == 1 && bytes.substr(8, 3) == std::string_view("AI\x02", 3) &&
            le16(prefix.data() + 18) == 62;
    case UpdatePackageKind::WindowsInstaller: {
        if (length < 64 || !bytes.starts_with("MZ")) return false;
        const auto offset = std::uint32_t{prefix[60]} | (std::uint32_t{prefix[61]} << 8) |
            (std::uint32_t{prefix[62]} << 16) | (std::uint32_t{prefix[63]} << 24);
        if (offset < 64 || offset > 1024 * 1024) return false;
        std::array<std::uint8_t, 26> header{};
        if (read(offset, std::span(header)) != header.size() ||
            std::string_view(reinterpret_cast<const char*>(header.data()), 4) != std::string_view("PE\0\0", 4)) return false;
        const auto machine = le16(header.data() + 4), flags = le16(header.data() + 22), magic = le16(header.data() + 24);
        // NSIS can have an x86 stub around an x64 application payload.
        return ((machine == 0x014c && magic == 0x010b) || (machine == 0x8664 && magic == 0x020b)) &&
            (flags & 0x0002) && !(flags & 0x2000);
    }
    }
    return false;
}

} // namespace HuhuCapture::Core
