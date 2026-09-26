#pragma once

namespace rfgvr::input {

// Game input from the motion controllers: hooks on the engine's analog and button getters add
// virtual stick axes and button presses (engine control action numbers) to the keyboard/mouse input.

bool install();
void update();  // game thread, once per frame (camera update)

// Room-scale: walk input added on top, relative to the camera (x right, y forward, -1..1).
void setExtraWalk(float right, float forward);
bool userWalking();  // the player moved the stick or keys themselves during the last frame

}  // namespace rfgvr::input
