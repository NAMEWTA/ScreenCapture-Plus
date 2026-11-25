#include "core/Startup.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace HuhuCapture::Core;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "startup_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

StartupRequest parse(std::initializer_list<std::string_view> arguments)
{
    const auto result = parseStartupArguments(std::span(arguments.begin(), arguments.size()));
    std::vector<std::wstring> storage;
    for (auto argument : arguments) storage.emplace_back(argument.begin(), argument.end());
    std::vector<std::wstring_view> wide;
    for (const auto& argument : storage) wide.emplace_back(argument);
    CHECK(result == parseStartupArguments<wchar_t>(wide));
    return result;
}
}

int main()
{
    CHECK(parse({}) == StartupRequest{});
    CHECK(parse({"--enter=pin"}).entry == CaptureEntry::Pin);
    CHECK(parse({"--enter=long"}).entry == CaptureEntry::Scroll);
    CHECK(parse({"--enter=video"}).entry == CaptureEntry::Recording);
    CHECK(parse({"--enter=qr"}).entry == CaptureEntry::Qr);
    CHECK(parse({"--enter=qrcode"}).entry == CaptureEntry::Qr);
    CHECK(parse({"--enter=tray"}) == StartupRequest{CaptureEntry::Capture, false, true});
    CHECK(parse({"--auto-start", "--enter=video"}) == StartupRequest{CaptureEntry::Recording, false, true});
    CHECK(parse({"--auto-start=true", "--auto-quit=true", "--enter=tray"}) ==
        StartupRequest{CaptureEntry::Capture, true, false});
    CHECK(parse({"--auto-start", "--auto-quit", "--enter=long"}) ==
        StartupRequest{CaptureEntry::Scroll, true, false});
    CHECK(parse({"--auto-quit=false", "--auto-quit"}).oneShot == false);
    CHECK(parse({"--auto-quit", "--auto-quit=false"}).oneShot == false);
    CHECK(parse({"--auto-quit=false", "--auto-quit=true"}).oneShot == true);
    CHECK(parse({"--auto-start=false", "--auto-start"}).background == false);
    CHECK(parse({"--auto-start", "--auto-start=false"}).background == false);
    CHECK(parse({"--enter=video", "--enter"}).entry == CaptureEntry::Recording);
    CHECK(parse({"--enter", "--enter=pin"}).entry == CaptureEntry::Pin);
    CHECK(parse({"--enter=pin", "--enter=qrcode"}).entry == CaptureEntry::Qr);
    for (const auto* flag : {"--auto-quit=TRUE", "--auto-quit=1", "--auto-quit=", "--auto-quit=true=false"})
        CHECK(!parse({flag}).oneShot);
    for (const auto* argument : {"--enter=PIN", "--enter=unknown", "--enter=", "--enter=pin=extra", "--ENTER=pin",
            "--enter", "--enter pin", "a.png", "--unknown", "=true"})
        CHECK(parse({argument}) == StartupRequest{});
    CHECK(parse({"--auto-start=false", "--enter=tray"}).background);
    CHECK(!parse({"--auto-quit", "--enter=tray"}).background);
    std::cout << "startup_tests: existing parameter, precedence, duplicate and one-shot contracts passed\n";
}
