#include "core/AnnotationRegion.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace Core = ScreenCapture::Core;
using Bounds = Core::AnnotationRegionBounds;
using Point = Core::AnnotationRegionPoint;
using Handle = Core::AnnotationRegionHandle;

namespace {

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "annotation_region_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

bool same(Bounds a, Bounds b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void creationTracksTheExactRectangle()
{
    const Bounds expected{20.f, 30.f, 180.f, 130.f};
    for (const auto start : {Point{20, 30}, Point{180, 30}, Point{20, 130}, Point{180, 130}}) {
        const Point end{200.f - start.x, 160.f - start.y};
        CHECK(same(Core::annotationRegionFromDrag(start, end, 320, 200), expected));
    }
    CHECK(same(Core::annotationRegionFromDrag({-8, -12}, {350, 240}, 320, 200), {0, 0, 320, 200}));
    CHECK(same(Core::annotationRegionFromDrag({350, 240}, {-8, -12}, 320, 200), {0, 0, 320, 200}));
    CHECK(!Core::validAnnotationRegion({80, 50, 80, 50}, 3));
    CHECK(!Core::validAnnotationRegion({80, 50, 80, 150}, 3));
    CHECK(!Core::validAnnotationRegion({80, 50, 180, 50}, 3));
    CHECK(!Core::validAnnotationRegion({80, 50, 82, 80}, 3));
    CHECK(Core::validAnnotationRegion({80, 50, 83, 53}, 3));
    CHECK(!Core::validAnnotationRegion({0, 0, std::numeric_limits<float>::infinity(), 8}, 3));
    CHECK(!Core::validAnnotationRegion({0, 0, 8, std::numeric_limits<float>::quiet_NaN()}, 3));
}

void movementPreservesSizeAtEveryImageBoundary()
{
    const Bounds origin{30, 40, 130, 100};
    CHECK(same(Core::moveAnnotationRegion(origin, {-1000, -1000}, 320, 200), {0, 0, 100, 60}));
    CHECK(same(Core::moveAnnotationRegion(origin, {1000, 1000}, 320, 200), {220, 140, 320, 200}));
    CHECK(same(Core::moveAnnotationRegion(origin, {20, -10}, 320, 200), {50, 30, 150, 90}));
    const Bounds entireImage{0, 0, 320, 200};
    CHECK(same(Core::moveAnnotationRegion(entireImage, {250, -150}, 320, 200), entireImage));
    for (int dx = -400; dx <= 400; dx += 7) {
        for (int dy = -250; dy <= 250; dy += 9) {
            const auto moved = Core::moveAnnotationRegion(origin,
                {static_cast<float>(dx), static_cast<float>(dy)}, 320, 200);
            CHECK(moved.left >= 0 && moved.top >= 0 && moved.right <= 320 && moved.bottom <= 200);
            CHECK(moved.right - moved.left == 100 && moved.bottom - moved.top == 60);
        }
    }
}

void resizeKeepsTheOppositeSidesAndMinimumExtent()
{
    const Bounds origin{30, 40, 130, 100};
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::TopLeft, {10, 20}, 320, 200, 3), {10, 20, 130, 100}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::Top, {10, 20}, 320, 200, 3), {30, 20, 130, 100}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::TopRight, {150, 20}, 320, 200, 3), {30, 20, 150, 100}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::Right, {150, 20}, 320, 200, 3), {30, 40, 150, 100}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::BottomRight, {150, 120}, 320, 200, 3), {30, 40, 150, 120}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::Bottom, {150, 120}, 320, 200, 3), {30, 40, 130, 120}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::BottomLeft, {10, 120}, 320, 200, 3), {10, 40, 130, 120}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::Left, {10, 120}, 320, 200, 3), {10, 40, 130, 100}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::TopLeft, {900, 900}, 320, 200, 3), {127, 97, 130, 100}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::BottomRight, {-900, -900}, 320, 200, 3), {30, 40, 33, 43}));
    CHECK(same(Core::resizeAnnotationRegion(origin, Handle::BottomRight, {900, 900}, 320, 200, 3), {30, 40, 320, 200}));
    for (int handle = 0; handle < 8; ++handle) {
        for (const auto target : {Point{-50, -50}, Point{0, 0}, Point{80, 70}, Point{500, 400}}) {
            const auto resized = Core::resizeAnnotationRegion(origin, static_cast<Handle>(handle), target, 320, 200, 3);
            CHECK(Core::validAnnotationRegion(resized, 3));
            CHECK(resized.left >= 0 && resized.top >= 0 && resized.right <= 320 && resized.bottom <= 200);
        }
    }
    // DPI/scale can request a fractional minimum, but real pointer coordinates
    // are integer source pixels. Use the production rounding rule and confirm
    // validity survives moving a minimum-size rectangle across the full image.
    for (const float markerScale : {1.f / 1.1f, 1.f / .333f, 1.f / .7f, 1.25f / .8f, 1.75f / .3f}) {
        const float minimum = Core::annotationRegionMinimumExtent(markerScale);
        CHECK(minimum >= 3.f * markerScale && minimum - 3.f * markerScale < 1.f);
        for (const float offset : {0.f, 130.f, 8142.f, 8192.f, 15950.f, 16300.f}) {
            const Bounds large{offset, offset, offset + 50, offset + 50};
            for (const auto handle : {Handle::TopLeft, Handle::BottomRight}) {
                const auto target = handle == Handle::TopLeft ? Point{20000, 20000} : Point{0, 0};
                auto resized = Core::resizeAnnotationRegion(large, handle, target, 16384, 16384, minimum);
                CHECK(Core::validAnnotationRegion(resized, minimum));
                CHECK(resized.right - resized.left == minimum && resized.bottom - resized.top == minimum);
                for (int trip = 0; trip < 10; ++trip) {
                    resized = Core::moveAnnotationRegion(resized, {-20000, -20000}, 16384, 16384);
                    CHECK(resized.left == 0 && resized.top == 0);
                    CHECK(Core::validAnnotationRegion(resized, minimum));
                    CHECK(resized.right - resized.left == minimum && resized.bottom - resized.top == minimum);
                    resized = Core::moveAnnotationRegion(resized, {20000, 20000}, 16384, 16384);
                    CHECK(resized.right == 16384 && resized.bottom == 16384);
                    CHECK(Core::validAnnotationRegion(resized, minimum));
                    CHECK(resized.right - resized.left == minimum && resized.bottom - resized.top == minimum);
                }
            }
        }
    }
}

void hitTestingLeavesTheInteriorAvailable()
{
    const Bounds region{20, 30, 180, 130};
    const auto handles = Core::annotationRegionHandles(region);
    for (std::size_t index = 0; index < handles.size(); ++index)
        CHECK(Core::hitAnnotationRegion(region, handles[index], 4) == static_cast<Handle>(index));
    CHECK(Core::hitAnnotationRegion(region, {60, 30}, 4) == Handle::Move);
    CHECK(Core::hitAnnotationRegion(region, {60, 133}, 4) == Handle::Move);
    CHECK(Core::hitAnnotationRegion(region, {60, 70}, 4) == Handle::None);
    CHECK(Core::hitAnnotationRegion(region, {60, 139}, 4) == Handle::None);
    CHECK(Core::hitAnnotationRegion(region, {5, 5}, 4) == Handle::None);
    CHECK(Core::hitAnnotationRegion(region, {12, 22}, 8) == Handle::TopLeft);
}

void badgesStayAtTheUpperLeftAndInsideTheImage()
{
    const auto point = Core::annotationBadgeCenter({20, 30, 180, 130}, 12, 320, 200);
    CHECK(point.x == 32 && point.y == 42);
    const auto edge = Core::annotationBadgeCenter({0, 0, 320, 200}, 12, 320, 200);
    CHECK(edge.x == 12 && edge.y == 12);
    const auto tinyEdge = Core::annotationBadgeCenter({315, 195, 320, 200}, 12, 320, 200);
    CHECK(tinyEdge.x == 308 && tinyEdge.y == 188);
    const auto tinyImage = Core::annotationBadgeCenter({0, 0, 6, 6}, 3, 6, 6);
    CHECK(tinyImage.x == 3 && tinyImage.y == 3);
    // At creation-time 200% DPI and a 50% viewport, all stored dimensions
    // remain original-image coordinates; a 24-logical-pixel badge is 96 px.
    const float creationScale = 2.f / .5f;
    const auto scaled = Core::annotationBadgeCenter({0, 0, 200, 200}, 12 * creationScale, 800, 600);
    CHECK(scaled.x == 48 && scaled.y == 48);
    CHECK(!Core::validAnnotationRegion({0, 0, 11, 100}, 3 * creationScale));
    CHECK(Core::validAnnotationRegion({0, 0, 12, 100}, 3 * creationScale));
}

} // namespace

int main()
{
    creationTracksTheExactRectangle();
    movementPreservesSizeAtEveryImageBoundary();
    resizeKeepsTheOppositeSidesAndMinimumExtent();
    hitTestingLeavesTheInteriorAvailable();
    badgesStayAtTheUpperLeftAndInsideTheImage();
    std::cout << "annotation_region_tests: creation, translation, resize, hit areas and badge anchoring passed\n";
}
