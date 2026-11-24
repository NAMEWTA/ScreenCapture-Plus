#include "core/HistoryStack.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

namespace {

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "history_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

struct Shape {
    explicit Shape(int value) : value(value) {}
    int value;
};
using History = HuhuCapture::Core::HistoryStack<Shape>;

std::vector<int> values(const History& history)
{
    std::vector<int> result;
    for (const auto& shape : history.activeItems()) result.push_back(shape->value);
    return result;
}

void emptyAndUnknownOperations()
{
    History history;
    Shape unknown(99);
    CHECK(history.undo() == nullptr);
    CHECK(history.redo() == nullptr);
    CHECK(!history.erase(nullptr));
    CHECK(!history.erase(&unknown));
    CHECK(history.ticket(&unknown).get() == nullptr);
    CHECK(history.append(nullptr) == nullptr);
    CHECK(history.activeItems().empty());
}

void undoRedoAndPreciseDeletion()
{
    History history;
    auto* first = history.append(std::make_unique<Shape>(1));
    auto* second = history.append(std::make_unique<Shape>(2));
    auto* third = history.append(std::make_unique<Shape>(3));
    CHECK(history.undo() == third);
    CHECK(history.undo() == second);
    CHECK(history.undo() == first);
    CHECK(history.undo() == nullptr);
    CHECK(history.activeItems().empty());
    CHECK(history.redo() == first);
    CHECK(history.redo() == second);
    CHECK(history.redo() == third);
    CHECK(history.redo() == nullptr);
    CHECK(history.erase(second));
    CHECK(values(history) == (std::vector<int>{ 1, 3 }));
    CHECK(history.undo() == third);
    CHECK(history.redo() == third);
    CHECK(values(history) == (std::vector<int>{ 1, 3 }));
}

void deletingAroundUndoBoundary()
{
    History history;
    auto* first = history.append(std::make_unique<Shape>(1));
    auto* second = history.append(std::make_unique<Shape>(2));
    auto* third = history.append(std::make_unique<Shape>(3));
    auto* fourth = history.append(std::make_unique<Shape>(4));
    CHECK(history.undo() == fourth);
    CHECK(history.undo() == third);
    CHECK(history.erase(first));
    CHECK(values(history) == (std::vector<int>{ 2 }));
    CHECK(history.erase(fourth));
    CHECK(history.redo() == third);
    CHECK(history.redo() == nullptr);
    CHECK(values(history) == (std::vector<int>{ 2, 3 }));
    CHECK(history.undo() == third);
    CHECK(history.undo() == second);
    CHECK(history.erase(second));
    CHECK(history.redo() == third);
}

void branchAndDeferredRemoval()
{
    History history;
    auto* first = history.append(std::make_unique<Shape>(1));
    auto* second = history.append(std::make_unique<Shape>(2));
    const auto firstTicket = history.ticket(first);
    const auto secondTicket = history.ticket(second);
    CHECK(history.undo() == second);
    CHECK(history.append(nullptr) == nullptr);
    CHECK(secondTicket.get() == second);
    CHECK(history.redo() == second);
    CHECK(history.undo() == second);
    Shape* hovered = second;
    Shape* creating = second;
    auto* third = history.append(std::make_unique<Shape>(3), [&](Shape* discarded) {
        // Borrow invalidation happens while the target is still alive.
        CHECK(discarded->value == 2);
        if (hovered == discarded) hovered = nullptr;
        if (creating == discarded) creating = nullptr;
    });
    CHECK(hovered == nullptr);
    CHECK(creating == nullptr);
    CHECK(secondTicket.get() == nullptr);
    CHECK(firstTicket.get() == first);
    CHECK(history.redo() == nullptr);
    CHECK(values(history) == (std::vector<int>{ 1, 3 }));
    CHECK(history.erase(third));
    CHECK(history.size() == 1);
}

void removalInvalidatesOnlyMatchingBorrows()
{
    History history;
    auto* first = history.append(std::make_unique<Shape>(1));
    auto* second = history.append(std::make_unique<Shape>(2));
    Shape* hovered = first;
    Shape* creating = second;
    int callbacks = 0;
    const auto release = [&](Shape* removed) {
        ++callbacks;
        CHECK(removed == second);
        CHECK(removed->value == 2);
        if (hovered == removed) hovered = nullptr;
        if (creating == removed) creating = nullptr;
    };
    CHECK(!history.erase(nullptr, release));
    CHECK(history.erase(second, release));
    CHECK(callbacks == 1);
    CHECK(hovered == first);
    CHECK(creating == nullptr);
    CHECK(values(history) == (std::vector<int>{ 1 }));
}

void queuedWorkAfterOwnerDestruction()
{
    std::function<void()> queued;
    int invocations = 0;
    {
        auto history = std::make_unique<History>();
        auto* shape = history->append(std::make_unique<Shape>(1));
        const auto ticket = history->ticket(shape);
        queued = [owner = history.get(), ticket, &invocations]() {
            if (auto* target = ticket.get()) {
                ++invocations;
                owner->erase(target);
            }
        };
    }
    queued();
    CHECK(invocations == 0);
}

// Force the allocator to reuse an address: pointer equality alone cannot safely
// identify the item requested by an earlier asynchronous text-edit callback.
struct ReusedShape {
    explicit ReusedShape(int value) : value(value) {}
    int value;
    static void* operator new(std::size_t);
    static void operator delete(void*) noexcept {}
};
alignas(ReusedShape) unsigned char reusedStorage[sizeof(ReusedShape)];
void* ReusedShape::operator new(std::size_t) { return reusedStorage; }

void recycledAddressesDoNotReviveTickets()
{
    HuhuCapture::Core::HistoryStack<ReusedShape> history;
    auto* first = history.append(std::make_unique<ReusedShape>(1));
    const auto stale = history.ticket(first);
    CHECK(history.erase(first));
    auto* replacement = history.append(std::make_unique<ReusedShape>(2));
    CHECK(stale.get() == nullptr);
    CHECK(history.ticket(replacement).get() == replacement);
    CHECK(history.activeItems()[0]->value == 2);
}

void growthPreservesTicketsAndOrder()
{
    History history;
    auto* first = history.append(std::make_unique<Shape>(0));
    const auto ticket = history.ticket(first);
    for (int i = 1; i < 10000; ++i) history.append(std::make_unique<Shape>(i));
    CHECK(ticket.get() == first);
    for (int i = 9999; i >= 0; --i) CHECK(history.undo()->value == i);
    CHECK(history.undo() == nullptr);
    for (int i = 0; i < 10000; ++i) CHECK(history.redo()->value == i);
    CHECK(history.redo() == nullptr);
    CHECK(history.activeCount() == 10000);
}

} // namespace

int main()
{
    emptyAndUnknownOperations();
    undoRedoAndPreciseDeletion();
    deletingAroundUndoBoundary();
    branchAndDeferredRemoval();
    removalInvalidatesOnlyMatchingBorrows();
    queuedWorkAfterOwnerDestruction();
    recycledAddressesDoNotReviveTickets();
    growthPreservesTicketsAndOrder();
    std::cout << "history_tests: 8 scenarios passed\n";
}
