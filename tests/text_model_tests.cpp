#include "core/HistoryStack.h"
#include "editor/TextModel.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace HuhuCapture::Editor;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void boundsEqual(EditorBounds actual, EditorBounds expected)
{
    require(std::abs(actual.left - expected.left) < 0.0001f && std::abs(actual.top - expected.top) < 0.0001f &&
        std::abs(actual.right - expected.right) < 0.0001f && std::abs(actual.bottom - expected.bottom) < 0.0001f,
        "Text source-space bounds changed");
}

void textAndStyleOwnership()
{
    TextModel text(9); // Existing six-DIP padding at 150% creation scale.
    require(!text.startEditing(), "Unplaced text entered the shared editor");
    require(text.place({100, 80}), "Text could not be placed");
    boundsEqual(text.bounds(), {91, 71, 109, 89});
    require(text.startEditing() && !text.startEditing(), "A duplicate edit start created two owners");
    const TextStyle first{30, 0xCF1322FF, true, false};
    require(text.setStyle(first) && text.style() == first, "Text did not own its complete style");
    const std::wstring input = L"完整中文 \U0001F600\nA full unwrapped line\n" + std::wstring(10000, L'W');
    text.setText(input);
    require(text.text() == input, "Full text was wrapped, trimmed or truncated in the model");
    require(text.finishEditing() && !text.finishEditing(), "Text edit termination was not idempotent");
    require(text.text() == input, "Ending editing discarded the authoritative text");
    const TextStyle next{45, 0x0958D9FF, false, true};
    require(text.setStyle(next) && text.startEditing(), "Reentry could not adopt the current tool style");
    require(text.style() == next && text.text() == input, "Reentering text lost its content or current tool style");
    auto invalid = next;
    invalid.fontSize = (std::numeric_limits<float>::quiet_NaN)();
    require(!text.setStyle(invalid) && text.style() == next, "Invalid font metrics replaced a valid style");
    std::cout << "PASS source-space padding, single edit owner, full Unicode/10000-character text and style reentry\n";
}

void hitTestingAndMovement()
{
    TextModel text(6);
    text.place({20, 30});
    require(text.setLayoutBounds({14, 24, 180, 90}), "Actual input layout was not accepted");
    require(text.hitTest({80, 50}, 1) == TextHandle::Edit, "Text interior lost its editing target");
    require(text.hitTest({14, 50}, 1) == TextHandle::Move, "Text border could not move");
    require(text.hitTest({200, 50}, 1) == TextHandle::None, "Inactive text captured an outside click");
    text.startEditing();
    require(text.hitTest({200, 50}, 1) == TextHandle::FinishEditing,
        "An outside press would create a second shape before ending current text editing");
    text.beginMove({14, 50});
    text.finishEditing();
    text.drag({4, 60});
    boundsEqual(text.bounds(), {4, 34, 170, 100});
    text.drag({-6, 30});
    boundsEqual(text.bounds(), {-6, 4, 160, 70});
    text.finishGesture();
    text.drag({500, 500});
    boundsEqual(text.bounds(), {-6, 4, 160, 70});
    require(!text.setLayoutBounds({0, 0, -1, 10}), "Inverted layout bounds were accepted");
    boundsEqual(text.bounds(), {-6, 4, 160, 70});
    std::cout << "PASS text edit/border/outside hit semantics, actual layout bounds and whole-text movement\n";
}

void deferredEmptyDeletion()
{
    HuhuCapture::Core::HistoryStack<TextModel> history;
    auto* empty = history.append(std::make_unique<TextModel>(6.0f));
    empty->place({20, 30});
    empty->startEditing();
    empty->setText(L"");
    empty->finishEditing();
    const auto pending = history.ticket(empty);
    require(pending.get() == empty, "Empty edit was destroyed while its callback still owned this");
    require(history.erase(empty), "Could not clear the empty text after the event");
    auto* replacement = history.append(std::make_unique<TextModel>(6.0f));
    replacement->place({20, 30});
    replacement->setText(L"保留新的文本 / keep this replacement");
    require(pending.get() == nullptr, "A stale empty-text callback resolved to a replacement shape");
    const auto retained = history.ticket(replacement);
    require(history.undo() == replacement && history.redo() == replacement && retained.get() == replacement,
        "Text content or its stable lifetime ticket was lost during undo/redo");
    require(replacement->text() == L"保留新的文本 / keep this replacement", "Undo/redo reconstructed incomplete text");
    std::cout << "PASS deferred empty removal and real HistoryStack ticket/undo/redo lifetimes\n";
}

} // namespace

int main()
{
    try {
        textAndStyleOwnership();
        hitTestingAndMovement();
        deferredEmptyDeletion();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
