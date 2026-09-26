#pragma once

struct IDXGISwapChain;

namespace rfgvr::mouse {

// Mouse in menus. The engine renders at the headset eye size while the window stays small, but its
// menus take the pointer position straight from WM_MOUSEMOVE client coordinates; mouse messages are
// scaled from window to render size before the engine's window procedure sees them. While the
// headset shows the game, the cursor is also kept inside the window (the engine only does that
// during gameplay, when the cursor is hidden).

// Engine hook (Steam build), called by camera::install().
bool install();

// Present thread, once per frame.
void onPresent(IDXGISwapChain* sc);

}  // namespace rfgvr::mouse
