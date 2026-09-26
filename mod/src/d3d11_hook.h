#pragma once

namespace rfgvr::d3d11 {

// Hooks d3d11!D3D11CreateDevice; on the game's device it hooks Present (VR, HUD, mouse, autostart)
// and the context calls the HUD capture needs.
bool install();

}  // namespace rfgvr::d3d11
