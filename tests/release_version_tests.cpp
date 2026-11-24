#include "core/ReleaseVersion.h"
#include <cstdlib>
#include <iostream>

using ScreenCapture::Core::parseReleaseVersion;

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
    std::cout << "release version parsing: valid tags, bounds, invalid tags and ordering passed\n";
}
