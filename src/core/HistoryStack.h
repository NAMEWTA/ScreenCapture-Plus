#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ScreenCapture::Core {

// An owning, UI-thread-only history. Active items form a prefix; the remaining
// suffix is the redo branch. It has no dependency on windows, drawing, or tools.
template<class T>
class HistoryStack {
    struct Lifetime {
        HistoryStack* owner;
    };

public:
    class Item {
        friend class HistoryStack;
    public:
        T* get() const noexcept { return value_.get(); }
        T* operator->() const noexcept { return get(); }

    private:
        Item(std::uint64_t id, std::unique_ptr<T> value) noexcept
            : id_(id), value_(std::move(value)) {}

        std::uint64_t id_;
        std::unique_ptr<T> value_;
    };

    // A deferred UI callback must resolve its ticket before using the owner or
    // item. IDs prevent a removed item's recycled address matching a new item.
    class Ticket {
        friend class HistoryStack;
    public:
        T* get() const noexcept {
            const auto lifetime = lifetime_.lock();
            return lifetime ? lifetime->owner->find(id_) : nullptr;
        }

    private:
        Ticket(std::weak_ptr<Lifetime> lifetime, std::uint64_t id) noexcept
            : lifetime_(std::move(lifetime)), id_(id) {}

        std::weak_ptr<Lifetime> lifetime_;
        std::uint64_t id_;
    };

    HistoryStack() : lifetime_(std::make_shared<Lifetime>(Lifetime{ this })) {}
    HistoryStack(const HistoryStack&) = delete;
    HistoryStack& operator=(const HistoryStack&) = delete;
    HistoryStack(HistoryStack&&) = delete;
    HistoryStack& operator=(HistoryStack&&) = delete;

    std::span<const Item> activeItems() const noexcept {
        return { items_.data(), activeCount_ };
    }

    std::size_t size() const noexcept { return items_.size(); }
    std::size_t activeCount() const noexcept { return activeCount_; }

    // beforeRemove releases UI borrows before their owned object is destroyed;
    // it must not mutate the history. Null input leaves the redo branch intact.
    template<class BeforeRemove>
    T* append(std::unique_ptr<T> value, BeforeRemove beforeRemove) {
        if (!value) return nullptr;
        // A nonempty redo suffix already provides capacity for the new item.
        // Otherwise vector keeps its geometric growth and strong exception safety.
        for (std::size_t i = activeCount_; i < items_.size(); ++i) {
            beforeRemove(items_[i].get());
        }
        items_.erase(items_.begin() + activeCount_, items_.end());
        auto* result = value.get();
        items_.push_back(Item(nextId_++, std::move(value)));
        ++activeCount_;
        return result;
    }

    T* append(std::unique_ptr<T> value) {
        return append(std::move(value), [](T*) {});
    }

    T* undo() noexcept {
        return activeCount_ == 0 ? nullptr : items_[--activeCount_].get();
    }

    T* redo() noexcept {
        return activeCount_ == items_.size() ? nullptr : items_[activeCount_++].get();
    }

    template<class BeforeRemove>
    bool erase(T* target, BeforeRemove beforeRemove) {
        const auto it = std::find_if(items_.begin(), items_.end(),
            [target](const Item& item) { return item.get() == target; });
        if (it == items_.end()) return false;
        const auto index = static_cast<std::size_t>(it - items_.begin());
        beforeRemove(it->get());
        items_.erase(it);
        if (index < activeCount_) --activeCount_;
        return true;
    }

    bool erase(T* target) { return erase(target, [](T*) {}); }

    Ticket ticket(T* target) const noexcept {
        const auto it = std::find_if(items_.begin(), items_.end(),
            [target](const Item& item) { return item.get() == target; });
        return Ticket(lifetime_, it == items_.end() ? 0 : it->id_);
    }

private:
    T* find(std::uint64_t id) const noexcept {
        const auto it = std::find_if(items_.begin(), items_.end(),
            [id](const Item& item) { return item.id_ == id; });
        return it == items_.end() ? nullptr : it->get();
    }

    std::vector<Item> items_;
    std::size_t activeCount_{ 0 };
    std::uint64_t nextId_{ 1 };
    // Declared last so pending tickets expire before owned items are destroyed.
    std::shared_ptr<Lifetime> lifetime_;
};

} // namespace ScreenCapture::Core
