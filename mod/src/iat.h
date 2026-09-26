#pragma once

#include <windows.h>

namespace rfgvr::iat {

// Replaces an import of `module` (e.g. "binkw32.dll" / "_BinkOpen@8"). Returns the previous
// pointer, or nullptr if the import was not found.
void* hook(HMODULE module, const char* dll, const char* function, void* replacement);

}  // namespace rfgvr::iat
