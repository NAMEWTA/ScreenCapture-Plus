#include "editor/EditorDocument.h"
#include "editor/BoxModel.h"
#include "editor/NumberModel.h"
#include "editor/TextModel.h"

#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

using namespace HuhuCapture::Editor;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// An actual collection of the shared pure models, with no native window, Qt,
// brush or font resource. The production native ShapeBase exposes the same
// semantic queries while its derived adapter owns the identical model classes.
struct Shape {
    std::variant<BoxModel, NumberModel, AnnotationModel, TextModel> data;
    bool* destroyed{};

    template<class Model, class... Arguments>
    Shape(std::in_place_type_t<Model> type, Arguments&&... arguments)
        : data(type, std::forward<Arguments>(arguments)...) {}
    ~Shape() { if (destroyed) *destroyed = true; }

    const AnnotationModel* annotationModel() const noexcept { return std::get_if<AnnotationModel>(&data); }
    std::optional<std::uint64_t> ordinaryNumber() const noexcept
    {
        const auto* number = std::get_if<NumberModel>(&data);
        return number ? std::optional<std::uint64_t>(number->number()) : std::nullopt;
    }
};

using Document = EditorDocument<Shape>;

std::unique_ptr<Shape> annotation(std::uint64_t number, const std::wstring& text)
{
    auto shape = std::make_unique<Shape>(std::in_place_type<AnnotationModel>, number, 640u, 480u, 1.0f);
    auto& model = std::get<AnnotationModel>(shape->data);
    model.beginGesture({10, 20});
    model.drag({100, 80});
    model.finishGesture();
    model.setNote(text);
    return shape;
}

std::unique_ptr<Shape> ordinary(std::uint64_t number)
{
    return std::make_unique<Shape>(std::in_place_type<NumberModel>, number, 16.0f);
}

void ownershipAndActivePrefix()
{
    Document document;
    auto* first = document.append(ordinary(*document.nextOrdinaryNumber()));
    bool destroyedSecond = false;
    auto secondOwner = ordinary(*document.nextOrdinaryNumber());
    secondOwner->destroyed = &destroyedSecond;
    auto* second = document.append(std::move(secondOwner));
    const auto secondTicket = document.ticket(second);
    require(document.activeCount() == 2 && document.size() == 2 && *document.nextOrdinaryNumber() == 3,
        "Document did not own the ordinary active prefix");
    bool released = false;
    require(document.undo([&](Shape* shape) {
        require(shape == second && document.activeCount() == 1 && !destroyedSecond,
            "Undo callback ran after destruction or before the active prefix changed");
        released = true;
    }) == second && released, "Undo did not release presentation borrows");
    require(secondTicket.get() == second && !destroyedSecond, "Undo destroyed the reusable redo shape");
    require(*document.nextOrdinaryNumber() == 2, "Hidden numbers affected active-maximum numbering");
    require(document.append(nullptr) == nullptr && document.size() == 2, "Null append discarded redo");
    require(document.append(ordinary(0)) == nullptr && document.size() == 2, "Invalid append discarded redo");
    auto* replacement = document.append(ordinary(*document.nextOrdinaryNumber()), [&](Shape* removed) {
        require(removed == second && secondTicket.get() == second && !destroyedSecond,
            "Redo branch borrow release happened after item destruction");
    });
    require(replacement && replacement->ordinaryNumber() == 2 && destroyedSecond && secondTicket.get() == nullptr,
        "New branch did not replace only the inactive suffix");
    require(document.redo() == nullptr && document.activeCount() == 2, "Discarded redo branch returned");
    require(document.erase(first) && document.activeCount() == 1 && document.size() == 1,
        "Erasing an earlier active shape damaged the active prefix");
    require(!document.erase(nullptr) && document.activeCount() == 1, "Unknown erase changed the document");
    std::cout << "PASS owning document, active-prefix undo/redo, beforeRemove and invalid/null append atomicity\n";
}

void annotationIdentityAndSelection()
{
    Document document;
    const auto cancelled = document.allocateAnnotationId();
    require(cancelled && *cancelled == 1, "New document did not start annotation IDs at one");
    // The creation can be cancelled before append; its ID remains consumed.
    auto* first = document.append(annotation(*document.allocateAnnotationId(), L"第一条完整备注"));
    const auto id = first->annotationModel()->number();
    require(id == 2 && document.selectedAnnotation() == id, "Cancelled creation reused its ID");
    auto* second = document.append(annotation(*document.allocateAnnotationId(), L"Second full note"));
    require(second->annotationModel()->number() == 3 && document.selectedAnnotation() == id,
        "Append unexpectedly changed an existing annotation selection");
    require(document.selectAnnotation(3), "Active note could not be selected");
    require(!document.selectAnnotation(0) && !document.selectAnnotation(999) && document.selectedAnnotation() == 3,
        "Invalid selection changed the active note");
    require(document.undo() == second && document.findAnnotation(3) == nullptr && document.selectedAnnotation() == 2,
        "Undo did not select the remaining highest note");
    require(document.redo() == second && document.selectedAnnotation() == 2,
        "Redo changed an existing valid selection");
    const auto color = first->annotationModel()->color();
    require(document.erase(second) && *document.allocateAnnotationId() == 4,
        "Removing an annotation returned its ID to the sequence");
    require(first->annotationModel()->color() == color, "A surviving note was recolored by another note's removal");
    require(document.append(annotation(2, L"Duplicate identity")) == nullptr && document.activeCount() == 1,
        "An active document accepted duplicate annotation identity");
    require(document.erase(first) && document.selectedAnnotation() == 0 && document.annotationNotes().empty(),
        "Deleting the final note left stale selection or text");
    require(*document.allocateAnnotationId() == 5, "An empty document reused old annotation IDs");
    Document fresh;
    require(*fresh.allocateAnnotationId() == 1, "A new document inherited the previous document's high-water mark");
    std::cout << "PASS cancellation/deletion identity, per-document reset, stable colors and selection reconciliation\n";
}

void completeTextAndInPlaceEditing()
{
    Document document;
    auto* high = document.append(annotation(9, L"最后一条 / final note"));
    auto* low = document.append(annotation(3, L"中文\n\U0001F600\n" + std::wstring(10000, L'W')));
    document.append(std::make_unique<Shape>(std::in_place_type<BoxModel>, BoxKind::Rectangle));
    document.append(ordinary(100));
    auto notes = document.annotationNotes();
    require(notes.size() == 2 && notes[0].number == 3 && notes[1].number == 9 &&
        notes[0].note == low->annotationModel()->note() && notes[1].note == high->annotationModel()->note(),
        "Complete notes were not sorted independently from nonannotation shapes");
    require(*document.allocateAnnotationId() == 10 && *document.nextOrdinaryNumber() == 101,
        "Annotation and ordinary-number counters were mixed");

    auto& model = std::get<AnnotationModel>(high->data);
    const auto count = document.activeCount();
    model.setNote(L"Edited full text\nwithout adding a new undo command");
    model.beginGesture({10, 20}, EditorHandle::Move);
    model.drag({30, 50});
    model.finishGesture();
    require(document.activeCount() == count && document.size() == count,
        "In-place text/move edits became a new command history");
    require(document.annotationNotes()[1].note == model.note(), "Document exported a stale note snapshot");
    std::cout << "PASS complete Unicode/long notes, sorted export and preserved in-place edit history semantics\n";
}

void limitsAndLifetimeTickets()
{
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    Document document;
    auto* annotationMax = document.append(annotation(maximum, L"Final stable annotation ID"));
    auto* numberMax = document.append(ordinary(maximum));
    require(annotationMax && numberMax && !document.allocateAnnotationId() && !document.nextOrdinaryNumber(),
        "Exhausted identifiers wrapped around or mixed their overflow states");
    document.erase(annotationMax);
    document.erase(numberMax);
    require(!document.allocateAnnotationId() && document.nextOrdinaryNumber() == 1,
        "Stable annotation exhaustion and reusable ordinary maxima behaved the same");

    std::optional<Document::Ticket> ticket;
    bool destroyed = false;
    {
        auto owner = std::make_unique<Document>();
        auto value = std::make_unique<Shape>(std::in_place_type<TextModel>, 6.0f);
        value->destroyed = &destroyed;
        auto* text = owner->append(std::move(value));
        ticket = owner->ticket(text);
        require(ticket->get() == text, "Document ticket could not resolve its live item");
    }
    require(destroyed && ticket && ticket->get() == nullptr, "A deferred callback survived document destruction");
    std::cout << "PASS UINT64 overflow behavior and deferred lifetime after document destruction\n";
}

} // namespace

int main()
{
    try {
        ownershipAndActivePrefix();
        annotationIdentityAndSelection();
        completeTextAndInPlaceEditing();
        limitsAndLifetimeTickets();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
