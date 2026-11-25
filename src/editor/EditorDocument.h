#pragma once

#include "core/Annotation.h"
#include "core/HistoryStack.h"
#include "editor/AnnotationModel.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace HuhuCapture::Editor {

// One owning document for the mature active-prefix history. Shape supplies two
// read-only semantic queries: annotationModel() -> const AnnotationModel*, and
// ordinaryNumber() -> optional<uint64_t>. It need not inherit a UI or platform
// class. The retained native adapter and the shared Qt editor use this same
// ownership/identity/selection logic while supplying their renderable records.
//
// Move, style and text changes remain in-place changes to a shape, matching the
// existing product. This deliberately does not invent a second command stack.
template<class Shape>
class EditorDocument {
public:
    using History = Core::HistoryStack<Shape>;
    using Ticket = typename History::Ticket;

    EditorDocument() = default;
    EditorDocument(const EditorDocument&) = delete;
    EditorDocument& operator=(const EditorDocument&) = delete;
    EditorDocument(EditorDocument&&) = delete;
    EditorDocument& operator=(EditorDocument&&) = delete;

    auto activeItems() const noexcept { return shapes_.activeItems(); }
    std::size_t size() const noexcept { return shapes_.size(); }
    std::size_t activeCount() const noexcept { return shapes_.activeCount(); }
    Ticket ticket(Shape* shape) const noexcept { return shapes_.ticket(shape); }

    // Consumed IDs are never returned by erase/undo/redo/cancel. Each document
    // starts its own sequence; an exhausted document does not wrap to zero.
    std::optional<std::uint64_t> allocateAnnotationId() noexcept { return annotations_.allocate(); }

    std::optional<std::uint64_t> nextOrdinaryNumber() const noexcept
    {
        std::uint64_t maximum = 0;
        for (const auto& shape : shapes_.activeItems()) {
            const auto number = shape->ordinaryNumber();
            if (number) maximum = (std::max)(maximum, *number);
        }
        return maximum == (std::numeric_limits<std::uint64_t>::max)()
            ? std::nullopt : std::optional<std::uint64_t>(maximum + 1);
    }

    // beforeRemove/beforeDeactivate release presentation borrows before an item
    // is destroyed or hidden. As in HistoryStack, they must not mutate history.
    template<class BeforeRemove>
    Shape* append(std::unique_ptr<Shape> shape, BeforeRemove beforeRemove)
    {
        if (!shape) return nullptr;
        const auto* annotation = shape->annotationModel();
        const auto number = shape->ordinaryNumber();
        if ((annotation && annotation->number() == 0) || (number && *number == 0)) return nullptr;
        const auto annotationId = annotation ? annotation->number() : 0;
        if (annotationId && findAnnotation(annotationId)) return nullptr;
        auto* result = shapes_.append(std::move(shape), std::move(beforeRemove));
        if (annotationId) annotations_.observe(annotationId);
        reconcileSelection();
        return result;
    }

    Shape* append(std::unique_ptr<Shape> shape)
    {
        return append(std::move(shape), [](Shape*) {});
    }

    template<class BeforeDeactivate>
    Shape* undo(BeforeDeactivate beforeDeactivate)
    {
        auto* shape = shapes_.undo();
        if (shape) beforeDeactivate(shape);
        reconcileSelection();
        return shape;
    }

    Shape* undo() { return undo([](Shape*) {}); }

    Shape* redo() noexcept
    {
        auto* shape = shapes_.redo();
        reconcileSelection();
        return shape;
    }

    template<class BeforeRemove>
    bool erase(Shape* shape, BeforeRemove beforeRemove)
    {
        if (!shapes_.erase(shape, std::move(beforeRemove))) return false;
        reconcileSelection();
        return true;
    }

    bool erase(Shape* shape) { return erase(shape, [](Shape*) {}); }

    Shape* findAnnotation(std::uint64_t number) const noexcept
    {
        if (number == 0) return nullptr;
        for (const auto& shape : shapes_.activeItems()) {
            const auto* annotation = shape->annotationModel();
            if (annotation && annotation->number() == number) return shape.get();
        }
        return nullptr;
    }

    bool selectAnnotation(std::uint64_t number) noexcept
    {
        if (!findAnnotation(number)) return false;
        selectedAnnotation_ = number;
        return true;
    }

    std::uint64_t selectedAnnotation() const noexcept { return selectedAnnotation_; }

    std::vector<Core::AnnotationNote> annotationNotes() const
    {
        std::vector<Core::AnnotationNote> result;
        for (const auto& shape : shapes_.activeItems())
            if (const auto* annotation = shape->annotationModel())
                result.push_back({annotation->number(), annotation->note()});
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.number < b.number; });
        return result;
    }

private:
    void reconcileSelection() noexcept
    {
        std::uint64_t maximum = 0;
        bool selectedExists = false;
        for (const auto& shape : shapes_.activeItems()) {
            if (const auto* annotation = shape->annotationModel()) {
                const auto number = annotation->number();
                maximum = (std::max)(maximum, number);
                selectedExists = selectedExists || selectedAnnotation_ == number;
            }
        }
        if (!selectedExists) selectedAnnotation_ = maximum;
    }

    Core::AnnotationSequence annotations_;
    std::uint64_t selectedAnnotation_{};
    History shapes_;
};

} // namespace HuhuCapture::Editor
