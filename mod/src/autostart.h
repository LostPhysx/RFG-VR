#pragma once

namespace rfgvr::autostart {

// Dev convenience: when <dll dir>\rfg-vr-autostart.txt exists (Steam build), load the most recent
// save from the main menu (Load Game, then the first entry of the save list), or, if the file
// contains "new", start a new game with the follow-up dialogs (autosave notice, difficulty, start
// weapon) pre-answered. Both call the game's own menu code.
void onPresent();  // Present thread (the game's UI thread), once per frame

}  // namespace rfgvr::autostart
