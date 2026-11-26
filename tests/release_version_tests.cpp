#include "core/ReleaseVersion.h"
#include "core/ReleasePackage.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using HuhuCapture::Core::parseReleaseVersion;

int main()
{
    const auto check = [](bool condition) { if (!condition) std::exit(1); };
    check(parseReleaseVersion(L"v2.5.16") == std::array<int, 3>{2,5,16});
    check(parseReleaseVersion(L"2.5.16") == std::array<int, 3>{2,5,16});
    check(parseReleaseVersion(L"0.0.0") == std::array<int, 3>{0,0,0});
    check(parseReleaseVersion(L"65535.65535.65535").has_value());
    for (const auto invalid : {L"", L"v", L"1", L"1.2", L"1.2.3.4", L"1.2.3.",
                               L"1..3", L"-1.2.3", L"1.2.3-rc1", L"1.2.3+build",
                               L" 1.2.3", L"1.2.3 ", L"V1.2.3", L"65536.1.2", L"9999999999.1.2"}) {
        check(!parseReleaseVersion(invalid).has_value());
    }
    check(*parseReleaseVersion(L"2.10.0") > *parseReleaseVersion(L"2.9.999"));
    check(*parseReleaseVersion(L"3.0.0") > *parseReleaseVersion(L"2.99.99"));
    namespace package = HuhuCapture::Core::ReleasePackage;
    check(package::canonicalVersion(L"v0.0.5", true));
    check(package::canonicalVersion(L"0.0.5"));
    for (const auto invalid : {L"00.0.5", L"0.00.5", L"v0.0.5", L"0.0.05"})
        check(!package::canonicalVersion(invalid));
    check(package::assetName(L"huhucapture-v0.0.5-windows-x64.zip"));
    for (const auto invalid : {L"../payload.zip", L"x/y.zip", L"x:y.exe", L"..", L"a%2f.zip", L"a b.zip"})
        check(!package::assetName(invalid));
    check(package::relativeFile(L"installer/plugins/platforms/qwindows.dll"));
    for (const auto invalid : {L"/root/file", L"a/../file", L"a//file", L"C:/file", L"a\\file", L"a/./file", L"file/"})
        check(!package::relativeFile(invalid));
    check(package::lowerHex(std::wstring(64, L'a'), 64));
    check(!package::lowerHex(std::wstring(64, L'A'), 64));
    std::uint64_t size{};
    check(package::byteCount(65 * 1024 * 1024, package::archiveBudget, size));
    check(!package::byteCount(65 * 1024 * 1024, package::legacyBudget, size));
    check(!package::byteCount(1.5, package::archiveBudget, size));
    check(!package::byteCount(std::numeric_limits<double>::infinity(), package::archiveBudget, size));
    check(!package::byteCount(std::numeric_limits<double>::quiet_NaN(), package::archiveBudget, size));
    check(package::byteCount(0, package::archiveBudget, size, true));
    check(!package::byteCount(0, package::archiveBudget, size));
    std::cout << "release contract: canonical versions, names, paths, hashes, per-kind byte bounds and ordering passed\n";
}
