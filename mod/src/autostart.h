#pragma once

namespace rfgvr::autostart {

// Dev convenience: when <dll dir>\rfg-vr-autostart.txt exists (Steam build), start a new game from
// the main menu by calling the game's own "New Game" handler with the follow-up dialogs (autosave
// notice, difficulty, start weapon) pre-answered. Game-state changes are logged either way.
void onPresent();  // Present thread (the game's UI thread), once per frame

}  // namespace rfgvr::autostart
