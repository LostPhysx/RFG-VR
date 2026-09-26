#pragma once

namespace rfgvr::config {

// Settings from <dll dir>\rfg-vr.ini, re-read when the file changes (checked about once a second),
// so values can be tuned while the game runs. Missing file or key = default.
//
//   [VR]
//   WorldScale=1.0   ; apparent size of the world: 1 = life-size, 1.5 = everything looks 1.5x bigger
//   CameraShake=0    ; 1 = keep the game's camera shake (explosions, hammer hits); 0 = none
//   LockCameraPitch=1 ; 1 = mouse/stick only turn the camera around the player; look up/down with the head
//   HeadAim=1        ; 1 = the game aims where the head looks (UI panel follows the head); 0 = mouse aim
//   HudLayer=1       ; 1 = in-game UI on a separate panel in front of the player; 0 = drawn into the 3D view
//   HudDistance=2.0  ; metres from the head to the UI panel
//   HudWidth=2.4     ; UI panel width in metres

// Game thread, once per frame.
void poll();

float worldScale();
bool cameraShake();
bool lockCameraPitch();
bool headAim();
bool hudLayer();
float hudDistance();
float hudWidth();

}  // namespace rfgvr::config
