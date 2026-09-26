#pragma once

namespace rfgvr::gamestate {

// The game's state (gameseq_get_state; values in RFGR_Types rfg/Game.h), read once per frame.
void update();  // Present thread
int current();  // -1 before the game runs / on other builds
bool gameplay();  // GS_GAMEPLAY; every other state (menus, videos, loading) shows the virtual screen

}  // namespace rfgvr::gamestate
