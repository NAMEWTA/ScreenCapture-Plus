#include "pch.h"
#include "editor/ShapeFactory.h"
#include "editor/shapes/ShapeAnnotation.h"
#include "editor/shapes/ShapeArrow.h"
#include "editor/shapes/ShapeEllipse.h"
#include "editor/shapes/ShapeEraser.h"
#include "editor/shapes/ShapeLine.h"
#include "editor/shapes/ShapeMosaic.h"
#include "editor/shapes/ShapeNumber.h"
#include "editor/shapes/ShapeRect.h"
#include "editor/shapes/ShapeText.h"

std::unique_ptr<ShapeBase> ShapeFactory::create(std::wstring_view toolId, WinPin* window)
{
    if (toolId == L"rect") return std::make_unique<ShapeRect>(window);
    if (toolId == L"ellipse") return std::make_unique<ShapeEllipse>(window);
    if (toolId == L"arrow") return std::make_unique<ShapeArrow>(window);
    if (toolId == L"number") {
        auto number = std::make_unique<ShapeNumber>(window);
        if (number->number() == 0) return nullptr;
        return number;
    }
    if (toolId == L"annotation") {
        auto annotation = std::make_unique<ShapeAnnotation>(window);
        if (annotation->number() == 0) return nullptr;
        return annotation;
    }
    if (toolId == L"line") return std::make_unique<ShapeLine>(window);
    if (toolId == L"text") return std::make_unique<ShapeText>(window);
    if (toolId == L"mosaic") return std::make_unique<ShapeMosaic>(window);
    if (toolId == L"eraser") return std::make_unique<ShapeEraser>(window);
    return nullptr;
}
