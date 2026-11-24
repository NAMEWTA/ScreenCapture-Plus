#pragma once

#include <memory>
#include <string_view>

class ShapeBase;
class WinPin;

namespace ShapeFactory {
// Add a drawing tool in this factory, its ShapeBase implementation, and the
// toolbar. History and rendering remain independent of concrete shape types.
std::unique_ptr<ShapeBase> create(std::wstring_view toolId, WinPin* window);
}
