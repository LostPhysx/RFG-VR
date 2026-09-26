#pragma once

namespace rfgvr::gamestate {

// The game's state machine (gameseq_get_state, RFGR_Types rfg/Game.h). Read once per frame on the
// Present thread; state changes are logged.
void update();

// Current state, or -1 before the game runs / on other builds.
int current();

// True in normal play (GS_GAMEPLAY). Menus shown over the world (pause, map, options, weapon
// cabinet, death screen, loading) are other states: VR then shows the flat game image as a screen.
bool gameplay();

}  // namespace rfgvr::gamestate
