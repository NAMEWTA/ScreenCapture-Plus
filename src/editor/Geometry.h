#pragma once

#include "core/AnnotationRegion.h"

namespace HuhuCapture::Editor {

// The existing annotation geometry already uses original-image pixels. These
// aliases give the same value types a neutral name for the other editor tools.
using EditorPoint = Core::AnnotationRegionPoint;
using EditorBounds = Core::AnnotationRegionBounds;
using EditorHandle = Core::AnnotationRegionHandle;

// Endpoint-based paths retain the legacy event indices used by their adapters.
enum class PathHandle { None = -1, Start = 0, End = 1, Move = 8 };

inline bool containsEditorPoint(const EditorBounds& bounds, EditorPoint point) noexcept
{
    // Existing rectangular tool handles deliberately exclude their boundary.
    return point.x > bounds.left && point.x < bounds.right &&
        point.y > bounds.top && point.y < bounds.bottom;
}

inline EditorBounds editorHandleBounds(EditorPoint center, float size) noexcept
{
    const float half = size / 2;
    return {center.x - half, center.y - half, center.x + half, center.y + half};
}

} // namespace HuhuCapture::Editor
