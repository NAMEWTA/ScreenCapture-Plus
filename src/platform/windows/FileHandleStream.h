#pragma once

#include <objidl.h>

namespace HuhuCapture::Platform::Windows {

// Copies ownership of the same open file object with DuplicateHandle; never
// reopens a pathname or closes the caller's CRT descriptor. The caller must not
// concurrently use the borrowed descriptor's seek position. The returned stream
// is seekable, supports 64-bit sizes, and is serialized for MF worker callbacks.
// It intentionally has direct, nontransactional file semantics.
HRESULT createFileHandleStream(int descriptor, IStream** stream) noexcept;

} // namespace HuhuCapture::Platform::Windows
