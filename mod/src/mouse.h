#pragma once

struct IDXGISwapChain;

namespace rfgvr::mouse {

// Menu mouse: pointer coordinates scaled to the render size; cursor kept in the window in VR.
bool install();                      // Steam build
void onPresent(IDXGISwapChain* sc);  // Present thread

}  // namespace rfgvr::mouse
