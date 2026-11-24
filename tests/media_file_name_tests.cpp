#include "core/MediaFileName.h"
#include <cassert>
#include <iostream>
#include <vector>

int main()
{
    using namespace HuhuCapture::Core;
    assert(makeMediaFileName(L"20261124-154503", L"png") == L"20261124-154503.png");
    assert(makeMediaFileName(L"20261124-154503", L"mp4", 1) == L"20261124-154503-001.mp4");
    assert(makeMediaFileName(L"20261124-154503", L"gif", 1000) == L"20261124-154503-1000.gif");
    assert(parseMediaFileName(L"20240229-235959.png"));
    assert(parseMediaFileName(L"20261124-154503-4294967295.mp4"));
    const std::vector<std::wstring> rejected{
        L"notes.txt", L"photo.png", L"temp.mp4", L"20261124-154503.tmp",
        L"../20261124-154503.png", L"folder/20261124-154503.png", L"20261124154503.png",
        L"20261124-154503-000.png", L"20261124-154503-1.png", L"20261124-154503-4294967296.png",
        L"20261124-154503.png.bak", L"20261124-154503.png/other.png", L"20260229-154503.png",
        L"20261324-154503.png", L"20260431-154503.png", L"20261124-246001.png"
    };
    for (const auto& name : rejected) assert(!parseMediaFileName(name));
    assert(makeMediaFileName(L"20261124-154503", L"../png").empty());
    assert(newerMediaFileName(L"20261124-154504.png", L"20261124-154503-999.png"));
    assert(newerMediaFileName(L"20261124-154503-001.png", L"20261124-154503.png"));
    assert(newerMediaFileName(L"20261124-154503-1000.png", L"20261124-154503-999.png"));
    std::cout << "PASS archive timestamps, collision suffixes, ordering and managed-file safety boundary\n";
}
