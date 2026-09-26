#pragma once

namespace rfgvr::camera {

// Engine hooks for stereo rendering (Steam build only):
//  - main view setup: renders the main camera from the current VR eye pose;
//  - rl_camera::render_begin: tells the OpenXR side which eye the Present thread is drawing;
//  - keen swapchain resize: renders at the headset's per-eye resolution.
bool install();

// Called at every Present, on the Present thread (before the original Present).
void onPresent();

}  // namespace rfgvr::camera
