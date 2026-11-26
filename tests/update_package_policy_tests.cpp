#include "core/UpdateManifest.h"
#include "core/UpdatePackageHeader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace Core = HuhuCapture::Core;
using Kind = Core::UpdatePackageKind;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
bool header(const std::vector<std::uint8_t>& bytes, Kind kind)
{
    return Core::updatePackageHeader(kind, [&bytes](std::uint64_t offset, std::span<std::uint8_t> target) {
        if (offset >= bytes.size()) return std::size_t{};
        const auto amount = (std::min)(target.size(), bytes.size() - static_cast<std::size_t>(offset));
        std::copy_n(bytes.data() + offset, amount, target.data());
        return amount;
    });
}
} // namespace

int main()
{
    try {
        require(Core::updatePackagePolicy(Kind::WindowsPortableZip).maximumBytes == 2ull * 1024 * 1024 * 1024,
            "Full portable packages retain the independent 2 GiB budget");
        require(Core::UpdateManifest::legacyExecutableLimit == 64ull * 1024 * 1024,
            "Only the standalone legacy fallback retains the 64 MiB bound");
        for (const auto* version : {"0.0.5", "100.20.1"})
            require(Core::UpdateManifest::canonicalVersion(version), "Canonical version rejected");
        for (const auto* version : {"v0.0.5", "00.0.5", "0.00.5", "0.0.05", "0.0.5-beta", "0.0.5/../", "-1.0.0"})
            require(!Core::UpdateManifest::canonicalVersion(version), "Noncanonical version accepted");
        require(Core::updateInventoryPath("portable-zip/Lang/简体中文.zh-CN.json"), "Existing Unicode language path rejected");
        for (const auto* path : {"", "/absolute", "../relative", "a/../b", "a//b", "a/./b", "C:/file", "a\\file", "a/", "a\x01"})
            require(!Core::updateInventoryPath(path), "Unsafe inventory path accepted");
        require(!Core::updateInventoryPath(std::string_view("a\0b", 3)), "Embedded NUL inventory path accepted");
        require(!Core::updateAssetName("../huhucapture.exe") && !Core::updateAssetName("huhucapture.exe:stream"),
            "Remote asset name escaped its leaf namespace");

        Core::UpdateManifest::Identity identity{"huhucapture", "NAMEWTA/huhucapture",
            "https://github.com/NAMEWTA/huhucapture/releases", "https://github.com/NAMEWTA/huhucapture/releases/download/"};
        Core::UpdateManifest::Release release;
        release.version = "0.0.5"; release.tag = "v0.0.5";
        Core::UpdateManifest::Asset legacy{"huhucapture.exe", std::string(64, 'a'),
            Core::UpdateManifest::assetUrl(identity, release.tag, "huhucapture.exe"), 100};
        release.assets.emplace(legacy.name, legacy);
        Core::UpdateManifest::Asset selected;
        require(Core::UpdateManifest::selectLegacyExecutable(identity, release, selected), "Absent manifest must permit verified legacy selection");
        release.assets.emplace("release-manifest.json", Core::UpdateManifest::Asset{});
        require(!Core::UpdateManifest::selectLegacyExecutable(identity, release, selected),
            "Even a malformed present manifest must prevent silent legacy downgrade");

        require(header({'P', 'K', 3, 4}, Kind::WindowsPortableZip), "ZIP type signature rejected");
        require(!header({'P', 'K', 3}, Kind::WindowsPortableZip), "Truncated ZIP signature accepted");
        std::vector<std::uint8_t> pe(64 + 26);
        pe[0] = 'M'; pe[1] = 'Z'; pe[60] = 64;
        pe[64] = 'P'; pe[65] = 'E'; pe[68] = 0x4c; pe[69] = 0x01; pe[86] = 2; pe[88] = 0x0b; pe[89] = 0x01;
        require(header(pe, Kind::WindowsInstaller), "Actual NSIS x86 stub architecture must be permitted around x64 payload");
        pe[68] = 0x64; pe[69] = 0x86; pe[89] = 2;
        require(header(pe, Kind::WindowsInstaller), "PE32+ executable signature rejected");
        pe[87] = 0x20;
        require(!header(pe, Kind::WindowsInstaller), "DLL cannot masquerade as an installer executable");
        pe[87] = 0; pe[63] = 0x7f;
        require(!header(pe, Kind::WindowsInstaller), "Unbounded PE offset must fail without reading beyond input");
        pe[63] = 0; pe.pop_back();
        require(!header(pe, Kind::WindowsInstaller), "Truncated PE optional header accepted");

        std::vector<std::uint8_t> deb(72, ' ');
        std::memcpy(deb.data(), "!<arch>\n", 8); std::memcpy(deb.data() + 8, "debian-binary", 13);
        deb[56] = '4'; deb[66] = '`'; deb[67] = '\n'; std::memcpy(deb.data() + 68, "2.0\n", 4);
        require(header(deb, Kind::LinuxDeb), "Debian package type signature rejected");
        deb[56] = '5'; require(!header(deb, Kind::LinuxDeb), "Incorrect debian-binary member size accepted");
        deb[56] = ' '; require(!header(deb, Kind::LinuxDeb), "Empty ar size field accepted");
        std::vector<std::uint8_t> appimage(64);
        std::memcpy(appimage.data(), "\x7f" "ELF", 4); appimage[4] = 2; appimage[5] = 1;
        std::memcpy(appimage.data() + 8, "AI\x02", 3); appimage[18] = 62;
        require(header(appimage, Kind::LinuxAppImage), "AppImage amd64 type signature rejected");
        appimage[18] = 183;
        require(!header(appimage, Kind::LinuxAppImage), "ARM64 AppImage accepted as amd64");
        std::cout << "Shared update package policy/type-boundary checks passed; no package payload or installation claim.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
