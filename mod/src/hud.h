#pragma once

struct ID3D11Device;
struct ID3D11Texture2D;
struct IDXGISwapChain;

namespace rfgvr::hud {

// In-game UI capture: in stereo gameplay the engine's UI pass draws into a transparent texture
// instead of the backbuffer; the XR code shows it as a panel over the 3D view.

void installContextHooks(ID3D11Device* device);  // after the game's device is created
bool installEngineHook();                        // Steam build

void onPresent(IDXGISwapChain* sc);  // Present thread, before xr::onPresent

// Present thread: the latest captured UI (premultiplied, backbuffer size/format), or null.
ID3D11Texture2D* latest();

}  // namespace rfgvr::hud
