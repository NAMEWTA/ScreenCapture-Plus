#include "editor/StrokeModel.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture::Editor;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void pointEqual(EditorPoint actual, EditorPoint expected)
{
    require(std::abs(actual.x - expected.x) < 0.0001f && std::abs(actual.y - expected.y) < 0.0001f,
        "Stroke point or order changed");
}

StrokeModel line(float start = 0, float end = 100)
{
    StrokeModel result;
    result.beginGesture({start, 0}, true);
    result.drag({end, 0}, true);
    result.finishGesture();
    return result;
}

void drawingAndContinuation()
{
    StrokeModel stroke;
    require(stroke.hitTest({0, 0}, 6, 12, 1) == PathHandle::None, "An unplaced stroke hit the origin");
    require(stroke.beginGesture({10, 20}, false) == PathHandle::End, "Freehand did not begin at the end handle");
    require(stroke.points().size() == 1, "Freehand press added a duplicate vertex");
    stroke.drag({20, 40}, false);
    stroke.drag({25, 45}, false);
    stroke.finishGesture();
    require(stroke.points().size() == 3, "Freehand dropped sampled vertices");
    stroke.drag({999, 999}, false);
    require(stroke.points().size() == 3, "Ended stroke continued drawing without a gesture");

    stroke.beginGesture({10, 20}, false, PathHandle::Start);
    stroke.drag({-5, 7}, false);
    require(stroke.points().size() == 4, "Start-handle continuation replaced existing vertices");
    pointEqual(stroke.points()[0], {-5, 7});
    pointEqual(stroke.points()[1], {10, 20});
    stroke.drag({-7, 9}, true);
    require(stroke.points().size() == 4, "Shift did not replace the current start endpoint");
    pointEqual(stroke.points()[0], {-7, 9});
    stroke.finishGesture();

    StrokeModel straight;
    straight.beginGesture({5, 6}, true);
    require(straight.points().size() == 2, "Shift press lost the fixed anchor");
    straight.drag({50, 60}, true);
    pointEqual(straight.points()[0], {5, 6});
    pointEqual(straight.points()[1], {50, 60});
    straight.drag({70, 80}, false);
    require(straight.points().size() == 3, "Releasing Shift did not resume freehand");
    straight.drag({90, 100}, true);
    require(straight.points().size() == 3, "Pressing Shift appended an unexpected vertex");
    pointEqual(straight.points()[1], {50, 60});
    pointEqual(straight.points()[2], {90, 100});
    straight.finishGesture();
    std::cout << "PASS stroke freehand, two-ended continuation, Shift anchor/replacement and gesture ending\n";
}

void edgeBandsAndIntersections()
{
    auto stroke = line();
    require(stroke.hitTest({0, 0}, 6, 40, 1) == PathHandle::Start, "Start handle was lost");
    require(stroke.hitTest({100, 0}, 6, 40, 1) == PathHandle::End, "End handle was lost");
    require(stroke.hitTest({50, 0}, 6, 40, 1) == PathHandle::None, "Thick stroke interior intercepted nested drawing");
    require(stroke.hitTest({50, 20}, 6, 40, 1) == PathHandle::Move, "Thick stroke edge band was lost");
    require(stroke.hitTest({50, 22}, 6, 40, 1) == PathHandle::None, "Stroke hit beyond its outer band");
    require(stroke.hitTest({50, 0}, 6, 2, 1) == PathHandle::Move, "Thin stroke lost its central hit target");

    StrokeModel crossing;
    crossing.beginGesture({-100, 0}, false);
    crossing.drag({100, 0}, false);
    crossing.drag({50, -100}, false);
    crossing.drag({50, 100}, false);
    crossing.finishGesture();
    require(crossing.hitTest({50, 20}, 6, 40, 1) == PathHandle::None,
        "One segment's edge took priority over an intersecting segment's interior");
    require(crossing.hitTest({0, 20}, 6, 40, 1) == PathHandle::Move,
        "A visible edge away from the intersection could not move the stroke");

    StrokeModel point;
    point.beginGesture({10, 10}, true); // Equal segment endpoints must not divide by zero.
    point.finishGesture();
    require(point.hitTest({10, 10}, 6, 20, 1) == PathHandle::Start, "Coincident handles changed their order");
    require(point.hitTest({20, 10}, 6, 20, 1) == PathHandle::Move, "Degenerate-segment distance was not finite");
    require(point.hitTest({10, 10}, 6, 20, 0) == PathHandle::None, "Invalid interaction scale was accepted");
    std::cout << "PASS thin/thick stroke edges, self-intersection interior, coincident points and handle priority\n";
}

void movementAndLongStroke()
{
    auto stroke = line();
    stroke.beginGesture({50, 1}, false, PathHandle::Move);
    stroke.drag({80, 41}, false);
    pointEqual(stroke.points()[0], {30, 40});
    pointEqual(stroke.points()[1], {130, 40});
    stroke.drag({20, -9}, true);
    pointEqual(stroke.points()[0], {-30, -10});
    pointEqual(stroke.points()[1], {70, -10});
    stroke.finishGesture();
    require(stroke.points().size() == 2, "Moving a stroke changed its vertex count");

    StrokeModel longStroke;
    longStroke.beginGesture({0, 0}, false);
    for (int index = 1; index <= 10000; ++index)
        longStroke.drag({static_cast<float>(index), static_cast<float>(index % 17)}, false);
    longStroke.finishGesture();
    require(longStroke.points().size() == 10001, "Long freehand input was decimated or truncated");
    pointEqual(longStroke.points().back(), {10000, 4});
    const std::vector<EditorPoint> before(longStroke.points().begin(), longStroke.points().end());
    longStroke.beginGesture({4000, 4}, false, PathHandle::Move);
    longStroke.drag({4500, -46}, false);
    longStroke.finishGesture();
    for (std::size_t index = 0; index < before.size(); ++index)
        pointEqual(longStroke.points()[index], {before[index].x + 500, before[index].y - 50});
    const auto count = longStroke.points().size();
    longStroke.beginGesture({10500, -46}, false, PathHandle::End);
    longStroke.drag({(std::numeric_limits<float>::quiet_NaN)(), 10}, false);
    require(longStroke.points().size() == count, "Nonfinite input entered the path");
    std::cout << "PASS whole-stroke translation, 10001 retained vertices and nonfinite input rejection\n";
}

} // namespace

int main()
{
    try {
        drawingAndContinuation();
        edgeBandsAndIntersections();
        movementAndLongStroke();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
