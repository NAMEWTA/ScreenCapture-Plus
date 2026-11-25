#include "editor/BoxModel.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace HuhuCapture::Editor;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void boundsEqual(const EditorBounds& actual, const EditorBounds& expected)
{
    require(std::abs(actual.left - expected.left) < 0.0001f &&
        std::abs(actual.top - expected.top) < 0.0001f &&
        std::abs(actual.right - expected.right) < 0.0001f &&
        std::abs(actual.bottom - expected.bottom) < 0.0001f,
        "Original-image box geometry changed");
}

BoxModel fixture(BoxKind kind = BoxKind::Rectangle)
{
    BoxModel model(kind);
    require(model.beginGesture({20, 30}) == EditorHandle::BottomRight, "Creation did not start a corner gesture");
    model.drag({80, 70}, false);
    model.finishGesture();
    return model;
}

void creationAndShift()
{
    constexpr std::array<EditorPoint, 4> ends{{{40, 50}, {0, 50}, {40, 10}, {0, 10}}};
    constexpr std::array<EditorBounds, 4> expected{{{20, 30, 40, 50}, {0, 30, 20, 50},
        {20, 10, 40, 30}, {0, 10, 20, 30}}};
    for (std::size_t index = 0; index < ends.size(); ++index) {
        BoxModel box;
        require(box.hitTest({0, 0}, 6, 2) == EditorHandle::None, "An unplaced tool hit the origin");
        box.beginGesture({20, 30});
        boundsEqual(box.bounds(), {20, 30, 20, 30});
        box.drag(ends[index], false);
        boundsEqual(box.bounds(), expected[index]);
        box.finishGesture();
        box.drag({500, 500}, true);
        boundsEqual(box.bounds(), expected[index]);
    }
    BoxModel negative;
    negative.beginGesture({20, 30});
    negative.drag({-10, -20}, true);
    // Existing square semantics normalize the drag then grow right/down. They
    // do not anchor the original press or clamp ordinary shapes to the image.
    boundsEqual(negative.bounds(), {-10, -20, 40, 30});
    auto ellipse = fixture(BoxKind::Ellipse);
    ellipse.beginGesture({20, 30}, EditorHandle::TopLeft);
    ellipse.drag({100, 90}, false);
    boundsEqual(ellipse.bounds(), {80, 70, 100, 90});
    require(ellipse.center().x == 90 && ellipse.center().y == 80 &&
        ellipse.radii().x == 10 && ellipse.radii().y == 10, "Ellipse radii diverged from its box");

    auto side = fixture();
    side.beginGesture({20, 50}, EditorHandle::Left);
    side.drag({10, 50}, true);
    boundsEqual(side.bounds(), {10, 30, 80, 100});
    side.drag({15, 50}, false);
    boundsEqual(side.bounds(), {15, 30, 80, 100});
    std::cout << "PASS box four-way creation, crossing, square/circle anchor, Shift release and original coordinates\n";
}

void resizingAndMovement()
{
    constexpr std::array<EditorPoint, 8> pointers{{{10, 5}, {45, 5}, {95, 5}, {95, 50},
        {95, 90}, {45, 90}, {5, 90}, {5, 50}}};
    constexpr std::array<EditorBounds, 8> expected{{{10, 5, 80, 70}, {20, 5, 80, 70},
        {20, 5, 95, 70}, {20, 30, 95, 70}, {20, 30, 95, 90}, {20, 30, 80, 90},
        {5, 30, 80, 90}, {5, 30, 80, 70}}};
    for (std::size_t index = 0; index < pointers.size(); ++index) {
        auto box = fixture();
        box.beginGesture({45, 50}, static_cast<EditorHandle>(index));
        box.drag(pointers[index], false);
        boundsEqual(box.bounds(), expected[index]);
    }
    auto box = fixture();
    box.beginGesture({30, 40}, EditorHandle::Move);
    box.drag({50, 15}, true);
    boundsEqual(box.bounds(), {40, 5, 100, 45}); // Shift must not resize a move.
    box.finishGesture();
    for (int index = 0; index < 2000; ++index) {
        const auto old = box.bounds();
        box.beginGesture({old.left + 7, old.top + 9}, EditorHandle::Move);
        box.drag({float(index % 2 == 0 ? -2000 : 16000), float(index % 3 == 0 ? 16000 : -2000)}, false);
        box.finishGesture();
        require(box.bounds().right - box.bounds().left == 60 &&
            box.bounds().bottom - box.bounds().top == 40, "Repeated moves changed the shape's extent");
    }
    const auto last = box.bounds();
    box.beginGesture({0, 0}, EditorHandle::None);
    box.drag({200, 300}, false);
    boundsEqual(box.bounds(), last);
    box.beginGesture({0, 0}, EditorHandle::Move);
    box.drag({(std::numeric_limits<float>::quiet_NaN)(), 1}, false);
    boundsEqual(box.bounds(), last);
    std::cout << "PASS eight box handles, active-gesture ownership and 2000 extent-preserving moves\n";
}

void hitTesting()
{
    auto rectangle = fixture();
    const auto handles = rectangle.handles(6);
    for (std::size_t index = 0; index < handles.size(); ++index) {
        const auto& bounds = handles[index];
        const EditorPoint center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        require(rectangle.hitTest(center, 6, 2) == static_cast<EditorHandle>(index), "Box handle hit priority changed");
    }
    require(rectangle.hitTest({50, 50}, 6, 2) == EditorHandle::None, "Box interior intercepted a nested mark");
    require(rectangle.hitTest({30, 30}, 6, 2) == EditorHandle::Move, "Visible rectangle edge could not move");
    require(rectangle.hitTest({53, 32}, 6, 1) == EditorHandle::None, "Handle boundary became inclusive");
    require(rectangle.hitTest({19, 50}, 6, 2) == EditorHandle::Left, "Near-edge handle was not selected");
    require(rectangle.hitTest({19, 40}, 6, 2) == EditorHandle::Move, "Filled rectangle's fixed edge band disappeared");

    BoxModel small;
    small.beginGesture({10, 10});
    small.drag({14, 14}, false);
    small.finishGesture();
    require(small.hitTest({13.5f, 10}, 6, 2) == EditorHandle::Top,
        "Ordinary boxes incorrectly adopted annotation's corner-first rule");

    auto ellipse = fixture(BoxKind::Ellipse);
    require(ellipse.hitTest({50, 50}, 6, 2) == EditorHandle::None, "Ellipse interior intercepted a nested mark");
    require(ellipse.hitTest({60, 31}, 6, 2) == EditorHandle::Move, "Ellipse edge annulus was lost");
    require(ellipse.hitTest({24, 32}, 6, 2) == EditorHandle::None, "Ellipse hit its invisible bounding-box corner");
    require(ellipse.hitTest({20, 30}, 6, 2) == EditorHandle::TopLeft, "Ellipse corner resize handle was lost");
    require(ellipse.hitTest({20, 30}, 6, (std::numeric_limits<float>::infinity)()) == EditorHandle::None,
        "Nonfinite hit-test geometry was accepted");
    std::cout << "PASS rectangle/ellipse edge bands, strict handles, overlap order and transparent interior\n";
}

} // namespace

int main()
{
    try {
        creationAndShift();
        resizingAndMovement();
        hitTesting();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
