#pragma once

namespace rfgvr::config {

// Settings from <dll dir>\rfg-vr.ini, re-read when the file changes (checked about once a second),
// so values can be tuned while the game runs. Missing file or key = default.
//
//   [VR]
//   WorldScale=1.0   ; apparent size of the world: 1 = life-size, 1.5 = everything looks 1.5x bigger
//   CameraShake=0    ; 1 = keep the game's camera shake (explosions, hammer hits); 0 = none

// Game thread, once per frame.
void poll();

float worldScale();
bool cameraShake();

}  // namespace rfgvr::config
