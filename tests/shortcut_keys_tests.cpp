#include "core/ShortcutKeys.h"

#include <cstdlib>
#include <iostream>

namespace {
using namespace HuhuCapture::Core;
void check(bool value, const char* expression, int line)
{
    if (!value) { std::cerr << "shortcut_keys_tests:" << line << ": " << expression << '\n'; std::exit(EXIT_FAILURE); }
}
#define CHECK(value) check(!!(value), #value, __LINE__)
ShortcutValidity validity(std::initializer_list<std::wstring> keys, bool supported = true)
{ return validateShortcutKeys(std::span(keys.begin(), keys.size()), supported); }
}

int main()
{
    CHECK(validity({L"F1"}) == ShortcutValidity::Valid);
    CHECK(validity({L"F12"}) == ShortcutValidity::Valid);
    CHECK(validity({L"PrintScreen"}) == ShortcutValidity::Valid);
    CHECK(validity({L"Pause"}) == ShortcutValidity::Valid);
    CHECK(validity({L"ScrollLock"}) == ShortcutValidity::Valid);
    for (const auto* key : {L"A", L"9", L"Space", L"Enter", L"Tab", L"Left", L"."})
        CHECK(validity({key}) == ShortcutValidity::NeedsModifier);
    CHECK(validity({}) == ShortcutValidity::MissingKey);
    CHECK(validity({L"Ctrl", L"Shift"}) == ShortcutValidity::MissingKey);
    CHECK(validity({L"Ctrl", L"A", L"B"}) == ShortcutValidity::MultipleKeys);
    CHECK(validity({L"Ctrl", L"A", L"A"}) == ShortcutValidity::MultipleKeys);
    CHECK(validity({L"Ctrl", L"", L"C"}) == ShortcutValidity::UnsupportedKey);
    CHECK(validity({L"Ctrl", L"CapsLock"}, false) == ShortcutValidity::UnsupportedKey);
    CHECK(validity({L"F99"}, false) == ShortcutValidity::UnsupportedKey);
    CHECK(validity({L"Ctrl", L"Alt", L"A"}) == ShortcutValidity::Valid);
    CHECK(validity({L"RWin", L"Shift", L"S"}) == ShortcutValidity::Valid);
    CHECK(validity({L"Alt", L"F4"}) == ShortcutValidity::Reserved);
    CHECK(validity({L"C", L"Ctrl"}) == ShortcutValidity::Reserved);
    CHECK(validity({L"L", L"LWin", L"RWin"}) == ShortcutValidity::Reserved);
    CHECK(validity({L"Ctrl", L"Ctrl", L"C"}) == ShortcutValidity::Reserved);
    std::vector<std::wstring> keys{L"A", L"RWin", L"Shift", L"Alt", L"Ctrl", L"LWin"};
    normalizeShortcutKeys(keys);
    CHECK(keys.size() == 5);
    CHECK(joinShortcutKeys(keys) == L"Ctrl+Alt+Shift+Win+A");
    CHECK(validateShortcutKeys(keys, true) == ShortcutValidity::Valid);
    std::cout << "shortcut_keys_tests: legacy policy, canonical modifiers and reserved-key aliases passed\n";
}
