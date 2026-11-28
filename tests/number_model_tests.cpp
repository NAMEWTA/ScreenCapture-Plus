#include "core/HistoryStack.h"
#include "editor/NumberModel.h"

#include <algorithm>
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

void pointEqual(EditorPoint actual, EditorPoint expected)
{
    require(std::abs(actual.x - expected.x) < 0.0001f && std::abs(actual.y - expected.y) < 0.0001f,
        "Directional number geometry changed");
}

void markerAndRotation()
{
    NumberModel number(7, 15);
    require(number.number() == 7 && !number.placed(), "Ordinary number identity changed");
    require(number.hitTest({0, 0}, 6) == NumberHandle::None, "An unplaced number hit the origin");
    require(number.beginGesture({50, 60}) == NumberHandle::Move, "A single click did not place the number");
    number.finishGesture();
    pointEqual(number.center(), {50, 60});
    pointEqual(number.outline().tip, {50, 80});
    pointEqual(number.outline().middle, {50, 45});
    require(number.hitTest({50, 60}, 6) == NumberHandle::Move &&
        number.hitTest(number.outline().tip, 6) == NumberHandle::Rotate &&
        number.hitTest(number.outline().middle, 6) == NumberHandle::Resize,
        "Number's move/rotate/resize handles changed");
    require(number.hitTest({60, 60}, 6) == NumberHandle::None, "Number unexpectedly intercepted its whole filled interior");

    number.beginGesture(number.outline().tip, NumberHandle::Rotate);
    number.drag({70, 60}, 8);
    pointEqual(number.outline().tip, {70, 60});
    pointEqual(number.outline().middle, {35, 60});
    require(number.angle() == 0, "Rotation to the right did not use the existing angle convention");
    number.drag({50, 40}, 8);
    pointEqual(number.outline().tip, {50, 40});
    require(std::abs(number.angle() - 90) < 0.0001f, "Rotation upward inverted the marker");
    number.finishGesture();
    std::cout << "PASS click-to-place directional marker, pointed silhouette, three handles and rotation convention\n";
}

void resizeAndMovement()
{
    NumberModel number(12, 16);
    number.beginGesture({100, 100});
    number.finishGesture();
    number.beginGesture(number.outline().middle, NumberHandle::Resize);
    const auto small = number.drag({101, 101}, 12); // 8 DIP at a 1.5 scale.
    require(small && *small == 12 && number.radius() == 16,
        "Radius request did not apply its gesture floor or changed before configuration accepted it");
    require(number.setRadius(*small), "Accepted radius could not be committed");
    const auto large = number.drag({100, -1000}, 12);
    require(large && *large == 1100, "Resize request was truncated before the existing slider policy");
    const float applied = std::clamp(*large, 9.0f, 129.0f); // Existing 6..86 DIP slider at 1.5 scale.
    number.setRadius(applied);
    require(number.radius() == 129, "Actual radius no longer matches the accepted slider value");
    pointEqual(number.outline().tip, {100, 272});
    number.finishGesture();
    number.beginGesture({100, 100}, NumberHandle::Move);
    number.drag({-40, 30}, 12);
    pointEqual(number.center(), {-40, 30});
    pointEqual(number.outline().tip, {-40, 202});
    number.finishGesture();
    number.drag({500, 500}, 12);
    pointEqual(number.center(), {-40, 30});
    require(!number.setRadius(0) && !number.setRadius((std::numeric_limits<float>::quiet_NaN)()) && number.radius() == 129,
        "Invalid radius destroyed the valid marker");
    std::cout << "PASS resize request/accepted-radius separation, DPI floor, full-marker movement and ended gestures\n";
}

void ordinaryNumberHistory()
{
    HuhuCapture::Core::HistoryStack<NumberModel> history;
    const auto append = [&]() {
        std::uint64_t maximum = 0;
        for (const auto& item : history.activeItems()) maximum = (std::max)(maximum, item->number());
        return history.append(std::make_unique<NumberModel>(maximum + 1, 16.0f));
    };
    auto* first = append();
    auto* second = append();
    auto* third = append();
    require(first->number() == 1 && second->number() == 2 && third->number() == 3,
        "Ordinary active numbers were not sequential");
    require(history.undo() == third, "Undo did not hide the highest ordinary number");
    const auto discarded = history.ticket(third);
    auto* branch = append();
    require(branch->number() == 3 && discarded.get() == nullptr,
        "Ordinary numbers incorrectly adopted annotation's never-reuse sequence");
    require(history.erase(branch), "Could not erase ordinary number");
    require(append()->number() == 3, "Deleting the active highest number did not permit the documented reuse");
    std::cout << "PASS ordinary active-maximum numbering across undo, deletion and redo-branch replacement\n";
}

} // namespace

int main()
{
    try {
        markerAndRotation();
        resizeAndMovement();
        ordinaryNumberHistory();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
