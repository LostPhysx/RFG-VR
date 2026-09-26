#pragma once

namespace rfgvr::d3d11 {

// Called from DllMain: only patches rfg.exe's import of D3D11CreateDevice (no D3D calls).
// The Present/ResizeBuffers hooks are installed later, from inside the game's first device creation.
bool install();

}  // namespace rfgvr::d3d11
