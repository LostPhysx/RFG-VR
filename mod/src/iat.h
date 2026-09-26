#pragma once

#include <windows.h>

namespace rfgvr::iat {

// Replaces an import-table entry of `module` (e.g. "d3d11.dll" / "D3D11CreateDevice").
// Returns the previous pointer, or nullptr if the import was not found.
void* hook(HMODULE module, const char* dll, const char* function, void* replacement);

}  // namespace rfgvr::iat
