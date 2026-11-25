#include "editor/AnnotationModel.h"
#include "core/HistoryStack.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

namespace Core = HuhuCapture::Core;
using Model = HuhuCapture::Editor::AnnotationModel;
using Point = Model::Point;
using Bounds = Model::Bounds;
using Handle = Model::Handle;

namespace {

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "annotation_model_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

bool same(const Bounds& a, const Bounds& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void create(Model& model, Point start = {20, 30}, Point end = {180, 130})
{
    CHECK(model.beginGesture(start) == Handle::BottomRight);
    CHECK(model.placed() && model.creating() && model.gestureActive());
    model.drag(end);
    CHECK(model.hasValidRegion());
    model.finishGesture();
    CHECK(!model.creating() && !model.gestureActive());
}

void creationAndMinimumExtent()
{
    for (const auto start : {Point{20, 30}, Point{180, 30}, Point{20, 130}, Point{180, 130}}) {
        Model model(7, 320, 200, 1);
        CHECK(model.number() == 7 && !model.placed() && !model.hasValidRegion());
        CHECK(model.hitTest(start, 4, 12) == Handle::None);
        create(model, start, {200 - start.x, 160 - start.y});
        CHECK(same(model.bounds(), {20, 30, 180, 130}));
        const auto completed = model.bounds();
        model.drag({-900, -900});
        CHECK(same(model.bounds(), completed));
    }

    Model clamped(8, 320, 200, 1);
    create(clamped, {-20, -30}, {900, 800});
    CHECK(same(clamped.bounds(), {0, 0, 320, 200}));
    for (const auto end : {Point{80, 50}, Point{80, 150}, Point{180, 50}, Point{82, 80}}) {
        Model empty(9, 320, 200, 1);
        empty.beginGesture({80, 50});
        empty.drag(end);
        CHECK(!empty.hasValidRegion());
        CHECK(empty.hitTest({80, 50}, 4, 12) == Handle::None);
        empty.finishGesture();
        CHECK(!empty.hasValidRegion());
    }

    // Creation-time scale is retained while later pointer tolerances change.
    // Fractional DPI/zoom must use the same outward integer rounding as the
    // current production region, including the smallest movable rectangle.
    for (const float scale : {1.f / 1.1f, 1.25f / .8f, 2.f / .5f}) {
        const float minimum = std::ceil(3.f * scale);
        Model model(10, 16384, 16384, scale);
        model.beginGesture({100, 100});
        model.drag({100 + minimum - 1, 100 + minimum});
        CHECK(!model.hasValidRegion());
        model.drag({100 + minimum, 100 + minimum});
        CHECK(model.hasValidRegion());
        model.finishGesture();
        for (int trip = 0; trip < 10; ++trip) {
            model.beginGesture({model.bounds().left, model.bounds().top}, Handle::Move);
            model.drag({-20000, -20000});
            model.finishGesture();
            CHECK(same(model.bounds(), {0, 0, minimum, minimum}));
            model.beginGesture({0, 0}, Handle::Move);
            model.drag({20000, 20000});
            model.finishGesture();
            CHECK(same(model.bounds(), {16384 - minimum, 16384 - minimum, 16384, 16384}));
            CHECK(model.hasValidRegion() && model.creationScale() == scale);
        }
    }
}

void badgeAndBorderHitTesting()
{
    Model model(15, 320, 200, 1);
    create(model);
    const auto badge = model.badgeCenter(12);
    CHECK(badge.x == 32 && badge.y == 42);
    CHECK(model.hitTest(badge, 4, 12) == Handle::Move);
    CHECK(model.hitTest({20, 30}, 4, 12) == Handle::TopLeft);
    CHECK(model.hitTest({60, 30}, 4, 12) == Handle::Move);
    CHECK(model.hitTest({100, 80}, 4, 12) == Handle::None);
    CHECK(model.hitTest({180, 130}, 4, 12) == Handle::BottomRight);
    CHECK(model.hitTest({100, 137}, 4, 12) == Handle::None);
    CHECK(model.hitTest({100, 137}, 8, 12) == Handle::Bottom);

    // A wide identifier can cover a side midpoint; the badge remains movable,
    // but the four corner handles still win over it on a small selection.
    Model small(999999, 320, 200, 1);
    create(small, {20, 30}, {44, 54});
    CHECK(small.hitTest({32, 30}, 4, 20) == Handle::Move);
    CHECK(small.hitTest({20, 30}, 4, 20) == Handle::TopLeft);
    CHECK(small.hitTest({44, 54}, 4, 20) == Handle::BottomRight);

    Model edge(16, 320, 200, 1);
    create(edge, {315, 195}, {320, 200});
    const auto edgeBadge = edge.badgeCenter(12);
    CHECK(edgeBadge.x == 308 && edgeBadge.y == 188);
    Model tiny(17, 6, 6, 1);
    create(tiny, {0, 0}, {6, 6});
    const auto tinyBadge = tiny.badgeCenter(3);
    CHECK(tinyBadge.x == 3 && tinyBadge.y == 3);
}

void existingGesturesKeepIdentityAndLastGeometry()
{
    const std::array<Point, 8> points{{{10, 20}, {10, 20}, {200, 20}, {200, 20},
        {200, 150}, {200, 150}, {10, 150}, {10, 150}}};
    const std::array<Bounds, 8> expected{{{10, 20, 180, 130}, {20, 20, 180, 130},
        {20, 20, 200, 130}, {20, 30, 200, 130}, {20, 30, 200, 150},
        {20, 30, 180, 150}, {10, 30, 180, 150}, {10, 30, 180, 130}}};
    const std::wstring text = L"\u4e2d\u6587\U0001f42f\n\nSecond line\r\nhttps://example.invalid/"
        + std::wstring(10000, L'W');
    for (std::size_t index = 0; index < expected.size(); ++index) {
        Model model(123, 320, 200, 1);
        model.setNote(text);
        create(model);
        const auto color = model.color();
        const auto handle = static_cast<Handle>(index);
        const auto start = Core::annotationRegionHandles(model.bounds())[index];
        CHECK(model.beginGesture(start, handle) == handle);
        CHECK(model.gestureActive() && !model.creating());
        model.drag(points[index]);
        CHECK(same(model.bounds(), expected[index]));
        // The production window uses the same finish operation on capture/focus
        // loss. A late move must not resume that interrupted existing gesture.
        model.finishGesture();
        model.drag({900, 900});
        CHECK(same(model.bounds(), expected[index]));
        CHECK(model.number() == 123 && model.note() == text && model.color() == color);
    }

    Model moved(124, 320, 200, 1);
    create(moved);
    moved.beginGesture({60, 30}, Handle::Move);
    moved.drag({-1000, -1000});
    CHECK(same(moved.bounds(), {0, 0, 160, 100}));
    moved.drag({1000, 1000});
    CHECK(same(moved.bounds(), {160, 100, 320, 200}));
    moved.drag({70, 40});
    CHECK(same(moved.bounds(), {30, 40, 190, 140}));
    moved.finishGesture();
    moved.beginGesture({100, 80}, Handle::None);
    CHECK(!moved.gestureActive());
    moved.drag({200, 150});
    CHECK(same(moved.bounds(), {30, 40, 190, 140}));
}

void historyAndInterruptedCreationKeepHighWaterMark()
{
    Core::AnnotationSequence sequence;
    Core::HistoryStack<Model> history;
    const auto add = [&]() {
        const auto number = sequence.allocate();
        CHECK(number.has_value());
        return history.append(std::make_unique<Model>(*number, 320, 200, 1.0f));
    };
    auto* first = add();
    create(*first);
    first->setNote(L"survivor");
    const auto firstColor = first->color();
    auto* cancelled = add();
    cancelled->beginGesture({20, 30});
    cancelled->drag({180, 130});
    CHECK(cancelled->creating() && cancelled->hasValidRegion());
    // Production removes an unfinished new region even if its current extent
    // happens to be valid. The allocated ID is deliberately not recovered.
    const auto ticket = history.ticket(cancelled);
    CHECK(history.erase(cancelled));
    CHECK(ticket.get() == nullptr);
    auto* third = add();
    CHECK(third->number() == 3);
    create(*third);
    third->setNote(L"redo text");
    CHECK(history.undo() == third);
    CHECK(history.redo() == third && third->note() == L"redo text");
    CHECK(history.undo() == third);
    auto* fourth = add();
    CHECK(fourth->number() == 4 && history.redo() == nullptr);
    CHECK(first->note() == L"survivor" && first->color() == firstColor && first->number() == 1);
    CHECK(history.erase(first));
    CHECK(fourth->number() == 4);
    sequence.observe((std::numeric_limits<std::uint64_t>::max)());
    CHECK(!sequence.allocate());
}

} // namespace

int main()
{
    creationAndMinimumExtent();
    badgeAndBorderHitTesting();
    existingGesturesKeepIdentityAndLastGeometry();
    historyAndInterruptedCreationKeepHighWaterMark();
    std::cout << "annotation_model_tests: production data/gestures, hit targets, interruption and history identities passed\n";
}
