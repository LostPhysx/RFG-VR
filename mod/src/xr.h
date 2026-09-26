#pragma once

#include <openxr/openxr.h>

#include <cstdint>

struct IDXGISwapChain;

namespace rfgvr::xr {

// OpenXR session and frame submission.
//
// Frame model: one headset frame spans two game frames. The headset frame is opened once
// (xrWaitFrame, xrBeginFrame, xrLocateViews). The engine's main view setup (game thread) asks
// renderPose() and gets the left eye, then the right eye, of that same view set. At each Present
// the camera hook reports which eye/set the backbuffer holds; once both eyes of the open set have
// arrived they are submitted together, with the captured in-game UI as a quad over them. Outside
// gameplay (menus, loading) no eye poses are handed out; the game's flat image is shown on a quad
// in front of the player.

// Present thread, called before the original Present so the backbuffer holds the finished frame.
void onPresent(IDXGISwapChain* sc);

struct RenderPose {
    int eye;       // 0 = left, 1 = right
    uint32_t set;  // headset frame this eye belongs to
    XrPosef pose;  // eye pose in LOCAL space (right-handed OpenXR conventions)
    XrFovf fov;    // asymmetric tangents for that eye
};

// Game thread: true when a headset frame is open and the main view should be rendered from `out`.
bool renderPose(RenderPose& out);

// Present thread, before Present: the backbuffer about to be presented was rendered for `eye` of
// headset frame `set` with `pose`, using the engine's actual (symmetric) `renderedFov`.
void markEyeRendered(int eye, uint32_t set, const XrPosef& pose, const XrFovf& renderedFov);

// Game thread: the head orientation (LOCAL space) of the open headset frame.
bool headOrientation(XrQuaternionf& out);

// Present thread: true while stereo frames are being shown, i.e. the in-game UI should be captured
// into the HUD layer instead of the eye images.
bool hudCaptureWanted();

// Render size per eye for the engine: a symmetric frustum covering the eye fov at SteamVR's
// recommended pixel density. False until the session has located the eyes once.
bool eyeRenderSize(uint32_t& w, uint32_t& h);

}  // namespace rfgvr::xr
