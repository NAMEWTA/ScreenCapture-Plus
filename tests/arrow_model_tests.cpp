#include "editor/ArrowModel.h"

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

void pointEqual(EditorPoint actual, EditorPoint expected)
{
    require(std::abs(actual.x - expected.x) < 0.0001f && std::abs(actual.y - expected.y) < 0.0001f,
        "Arrow geometry or directional constraint changed");
}

void polygonAndSizing()
{
    ArrowModel arrow(12);
    require(arrow.polygon().empty(), "Unplaced arrow produced a polygon");
    arrow.beginGesture({10, 20});
    require(arrow.polygon().size() == 1, "Zero-length arrow did not retain its safe degenerate figure");
    arrow.drag({110, 20}, false);
    constexpr std::array<EditorPoint, 6> horizontal{{{10, 20}, {98, 17}, {95, 12}, {110, 20}, {95, 28}, {98, 23}}};
    require(arrow.polygon().size() == horizontal.size(), "Arrow polygon lost a vertex");
    for (std::size_t index = 0; index < horizontal.size(); ++index) pointEqual(arrow.polygon()[index], horizontal[index]);
    arrow.finishGesture();
    require(arrow.setSize(24), "Valid arrow size was rejected");
    pointEqual(arrow.polygon()[1], {86, 14});
    pointEqual(arrow.polygon()[2], {80, 4});
    pointEqual(arrow.polygon()[3], {110, 20});
    require(!arrow.setSize(0) && !arrow.setSize((std::numeric_limits<float>::infinity)()) && arrow.size() == 24,
        "Invalid size replaced a valid arrow");

    ArrowModel vertical(12);
    vertical.beginGesture({0, 0});
    vertical.drag({0, 100}, false);
    pointEqual(vertical.polygon()[1], {3, 88});
    pointEqual(vertical.polygon()[2], {8, 85});
    pointEqual(vertical.polygon()[4], {-8, 85});
    pointEqual(vertical.polygon()[5], {-3, 88});
    ArrowModel tiny;
    tiny.beginGesture({0, 0});
    tiny.drag({0.5f, 0}, false);
    require(tiny.polygon().size() == 1, "Subpixel arrow divided by an unusable length");
    std::cout << "PASS six-vertex arrow silhouette, shaft/head proportions, size changes and degenerate arrows\n";
}

void eightDirectionConstraint()
{
    constexpr std::array<EditorPoint, 8> pointer{{{10, 3}, {10, 5}, {3, 10}, {-5, 10},
        {-10, 3}, {-10, -5}, {3, -10}, {10, -5}}};
    constexpr std::array<EditorPoint, 8> snapped{{{10, 0}, {10, 10}, {0, 10}, {-10, 10},
        {-10, 0}, {-10, -10}, {0, -10}, {10, -10}}};
    for (std::size_t index = 0; index < pointer.size(); ++index) {
        ArrowModel arrow;
        arrow.beginGesture({0, 0});
        arrow.drag(pointer[index], true);
        pointEqual(arrow.end(), snapped[index]);
        arrow.drag(pointer[index], false);
        pointEqual(arrow.end(), pointer[index]);
    }
    ArrowModel boundary;
    boundary.beginGesture({0, 0});
    boundary.drag({100, 41.42f}, true);
    pointEqual(boundary.end(), {100, 0});
    boundary.drag({100, 41.43f}, true);
    pointEqual(boundary.end(), {100, 100});
    boundary.drag({100, 0}, false);
    boundary.finishGesture();
    boundary.beginGesture({0, 0}, PathHandle::Start);
    boundary.drag({95, 10}, true);
    pointEqual(boundary.start(), {90, 10});
    pointEqual(boundary.end(), {100, 0});
    std::cout << "PASS eight directional octants, threshold boundary, Shift release and start-endpoint constraint\n";
}

void movementAndHandles()
{
    ArrowModel arrow;
    arrow.beginGesture({10, 20});
    arrow.drag({110, 20}, false);
    arrow.finishGesture();
    require(arrow.hitHandles({10, 20}, 6) == PathHandle::Start &&
        arrow.hitHandles({110, 20}, 6) == PathHandle::End, "Arrow endpoint handles disappeared");
    require(arrow.hitHandles({13, 20}, 6) == PathHandle::None, "Arrow handle boundary became inclusive");
    require(arrow.hitHandles({60, 20}, 6) == PathHandle::None, "Model duplicated renderer polygon containment");
    arrow.beginGesture({60, 20}, PathHandle::Move);
    arrow.drag({80, 50}, true);
    pointEqual(arrow.start(), {30, 50});
    pointEqual(arrow.end(), {130, 50});
    arrow.drag({-20, -50}, false);
    pointEqual(arrow.start(), {-70, -50});
    pointEqual(arrow.end(), {30, -50});
    arrow.finishGesture();
    arrow.drag({500, 500}, true);
    pointEqual(arrow.end(), {30, -50});
    std::cout << "PASS arrow endpoint handles, whole-polygon movement and ended gestures\n";
}

} // namespace

int main()
{
    try {
        polygonAndSizing();
        eightDirectionConstraint();
        movementAndHandles();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
