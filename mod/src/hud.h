#pragma once

struct ID3D11Device;
struct ID3D11Texture2D;
struct IDXGISwapChain;

namespace rfgvr::hud {

// HUD capture. The engine draws its 2D primitive queue (the whole in-game UI: HUD, crosshair, pause
// and map screens) in FUN_00550830, right after the 3D main view and onto the backbuffer. While VR
// is active that pass is redirected into a separate transparent texture, which the XR layer shows
// as a flat panel in front of the player; the eye images then contain only the 3D scene.

// d3d11 hook: after the game's device is created (hooks the context's render target / blend calls).
void installContextHooks(ID3D11Device* device);
// Engine hook, called by camera::install() on the Steam build.
bool installEngineHook();

// Present thread, before xr::onPresent: remembers the backbuffer the UI pass would draw into.
void onPresent(IDXGISwapChain* sc);

// Present thread. The captured HUD of the most recent 3D frame, or null when the last frames drew
// no UI pass (menus, loading) or capture is off. Premultiplied alpha; same size/format as the backbuffer.
ID3D11Texture2D* latest();

}  // namespace rfgvr::hud
