#pragma once

#include <openxr/openxr.h>

#include <cstdint>

struct IDXGISwapChain;

namespace rfgvr::xr {

// OpenXR session and frame submission. One headset frame spans two game frames: the main view is
// rendered for the left eye, then the right eye, from the same located views; both images are then
// submitted together, with the captured UI as a panel. Outside gameplay the game's flat image is
// shown on a virtual screen.

void onPresent(IDXGISwapChain* sc);  // Present thread, before the original Present

struct RenderPose {
    int eye;       // 0 = left, 1 = right
    uint32_t set;  // headset frame this eye belongs to
    XrPosef pose;  // LOCAL space, OpenXR conventions
    XrFovf fov;
};

// Game thread: the eye to render the main view for, if a headset frame is open.
bool renderPose(RenderPose& out);

// Present thread: the backbuffer holds `eye` of headset frame `set`, rendered with `fov`.
void markEyeRendered(int eye, uint32_t set, const XrPosef& pose, const XrFovf& fov);

bool headOrientation(XrQuaternionf& out);  // game thread, LOCAL space
bool stereoActive();                       // the headset shows the game
bool hudCaptureWanted();                   // Present thread: capture the UI into the HUD panel

// Engine render size: a symmetric frustum covering both eyes at SteamVR's pixel density.
bool eyeRenderSize(uint32_t& w, uint32_t& h);

}  // namespace rfgvr::xr
