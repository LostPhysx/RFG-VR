#pragma once

namespace rfgvr::autostart {

// Dev convenience, enabled by rfg-vr-autostart.txt next to the DLL: passes the title screen and
// loads the newest save ("new" in the file: starts a new game), through the game's own menu code.
void onPresent();  // Present thread

// True once autostart has started a new game itself (its intro cinematic may then be skipped).
bool startedNewGame();

}  // namespace rfgvr::autostart
