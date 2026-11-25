#include "capture/SelectionModel.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace HuhuCapture::Capture;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void same(const SelectionBounds& actual, const SelectionBounds& expected)
{
    require(actual.left == expected.left && actual.top == expected.top &&
        actual.right == expected.right && actual.bottom == expected.bottom,
        "the original capture selection has the expected source-pixel bounds");
}
SelectionModel selected(SelectionBounds bounds = {20, 30, 60, 60})
{
    SelectionModel model(100, 80);
    require(model.setSelection(bounds), "select the real starting rectangle");
    return model;
}

void windowSelection()
{
    SelectionModel model(100, 80);
    model.setWindowCandidates({{20, 20, 60, 60}, {-20, -10, 90, 70}, {0, 0, 100, 80}});
    require(model.highlight({40, 40}), "the foremost matching window highlights");
    same(model.bounds(), {20, 20, 60, 60});
    require(!model.highlight({40, 40}), "unchanged hover does not invalidate cached label/rendering");
    require(model.highlight({20, 40}), "the exact front-window edge belongs to the next containing window");
    same(model.bounds(), {0, 0, 90, 70});
    require(model.highlight({95, 75}), "the desktop window remains selectable behind other windows");
    same(model.bounds(), {0, 0, 100, 80});
    require(!model.highlight({0, 0}), "a strict boundary miss preserves the last actual highlight");
    require(model.beginCreation({20, 25}), "press starts a selection gesture");
    model.finishGesture();
    same(model.bounds(), {0, 0, 100, 80});
    require(model.hasSelection(), "click without motion accepts the existing window selection");
    model.setExtent(200, 120);
    require(!model.hasSelection() && !model.highlight({40, 40}) && !model.gestureActive(),
        "a new image clears the old selection, candidates and gesture");
    std::cout << "PASS native-window z order, clipped bounds, strict edges, stable hover and click acceptance\n";
}

void creation()
{
    constexpr std::array<SelectionPoint, 4> ends{{{80, 65}, {10, 65}, {80, 15}, {10, 15}}};
    constexpr std::array<SelectionBounds, 4> bounds{{{50, 40, 80, 65}, {10, 40, 50, 65},
        {50, 15, 80, 40}, {10, 15, 50, 40}}};
    for (std::size_t index = 0; index < ends.size(); ++index) {
        SelectionModel model(100, 80);
        require(model.beginCreation({50, 40}) && !model.hasSelection(), "press alone creates no invented image rectangle");
        require(model.createTo(ends[index]), "each drag direction changes the selection");
        same(model.bounds(), bounds[index]);
        model.finishGesture();
        require(!model.createTo({90, 75}), "released gestures cannot mutate a finished selection");
    }
    SelectionModel model(100, 80);
    model.beginCreation({50, 40});
    model.createTo({-12, 90});
    same(model.bounds(), {0, 40, 50, 80});
    model.createTo({50, 75});
    require(!model.hasSelection(), "a zero-width drag does not count as an accepted region");
    model.createTo({51, 41});
    same(model.bounds(), {50, 40, 51, 41});
    require(model.hasSelection(), "creation preserves the original one-pixel selection capability");
    std::cout << "PASS four-direction creation, outside drag clipping, zero-width and one-pixel behavior\n";
}

void hitRegionsAndImmediateSnap()
{
    constexpr std::array<SelectionPoint, 9> points{{{10, 20}, {40, 20}, {70, 20},
        {10, 45}, {40, 45}, {70, 45}, {10, 70}, {40, 70}, {70, 70}}};
    constexpr std::array<SelectionHit, 9> hits{{SelectionHit::TopLeft, SelectionHit::Top, SelectionHit::TopRight,
        SelectionHit::Left, SelectionHit::Inside, SelectionHit::Right,
        SelectionHit::BottomLeft, SelectionHit::Bottom, SelectionHit::BottomRight}};
    constexpr std::array<SelectionBounds, 9> expected{{{10, 20, 60, 60}, {20, 20, 60, 60}, {20, 20, 70, 60},
        {10, 30, 60, 60}, {20, 30, 60, 60}, {20, 30, 70, 60},
        {10, 30, 60, 70}, {20, 30, 60, 70}, {20, 30, 70, 70}}};
    for (std::size_t index = 0; index < points.size(); ++index) {
        auto model = selected();
        require(model.hitTest(points[index]) == hits[index], "the extended-edge nine-region hit rule is retained");
        const bool changed = model.beginAdjustment(points[index]);
        require(changed == (hits[index] != SelectionHit::Inside), "outside press snaps immediately; interior press never jumps");
        require(model.adjustment() == hits[index] && model.gestureActive(), "the pressed direction stays fixed for this gesture");
        same(model.bounds(), expected[index]);
    }
    const auto model = selected();
    for (const SelectionPoint point : {SelectionPoint{20, 30}, {60, 60}, {20, 40}, {40, 60}})
        require(model.hitTest(point) == SelectionHit::Inside, "the exact border is an inclusive move hit");
    require(SelectionModel(100, 80).hitTest({20, 20}) == SelectionHit::None, "an empty selection does not intercept adjustment");
    std::cout << "PASS all nine hit regions, inclusive borders and immediate outside-edge snapping\n";
}

void stableAdjustment()
{
    auto move = selected();
    move.beginAdjustment({35, 45});
    move.adjustTo({45, 50});
    same(move.bounds(), {30, 35, 70, 65});
    move.adjustTo({50, 55});
    same(move.bounds(), {35, 40, 75, 70});
    move.adjustTo({200, 200});
    same(move.bounds(), {60, 50, 100, 80});
    move.adjustTo({-40, -40});
    same(move.bounds(), {0, 0, 40, 30});
    move.adjustTo({35, 45});
    same(move.bounds(), {20, 30, 60, 60});
    move.finishGesture();
    require(!move.adjustTo({90, 70}), "a finished move retains its final complete rectangle");

    auto crossed = selected();
    crossed.beginAdjustment({10, 45});
    crossed.adjustTo({75, 45});
    same(crossed.bounds(), {60, 30, 75, 60});
    crossed.adjustTo({61, 45});
    same(crossed.bounds(), {57, 30, 61, 60}); // Mature crossing/minimum-side semantics.
    crossed.adjustTo({10, 45});
    same(crossed.bounds(), {10, 30, 60, 60});
    crossed.adjustTo({-20, 45});
    same(crossed.bounds(), {0, 30, 60, 60});
    auto corner = selected();
    corner.beginAdjustment({70, 70});
    corner.adjustTo({5, 10});
    same(corner.bounds(), {5, 10, 20, 30});
    std::cout << "PASS press-based adjustment, extent-preserving bounded move and crossed-edge resizing\n";
}

void edgeMinimum()
{
    auto left = selected({0, 0, 1, 8});
    left.beginAdjustment({-1, 4});
    same(left.bounds(), {0, 0, 4, 8});
    auto right = selected({99, 10, 100, 20});
    right.beginAdjustment({101, 15});
    same(right.bounds(), {96, 10, 100, 20});
    auto top = selected({5, 0, 10, 1});
    top.beginAdjustment({7, -1});
    same(top.bounds(), {5, 0, 10, 4});
    auto bottom = selected({5, 79, 10, 80});
    bottom.beginAdjustment({7, 81});
    same(bottom.bounds(), {5, 76, 10, 80});
    SelectionModel tiny(2, 3);
    tiny.setSelection({0, 0, 2, 3});
    tiny.beginAdjustment({-1, -1});
    tiny.adjustTo({2, 3});
    same(tiny.bounds(), {0, 0, 2, 3});
    std::cout << "PASS four-pixel minimum fits every image edge and small authorized source\n";
}
}

int main()
{
    try {
        windowSelection(); creation(); hitRegionsAndImmediateSnap(); stableAdjustment(); edgeMinimum();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
