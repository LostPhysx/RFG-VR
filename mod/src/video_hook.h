#pragma once

namespace rfgvr::video {

// Logs every Bink video the game opens and, in auto-start mode (rfg-vr-autostart.txt), skips the
// New Game intro cinematic by jumping it to its last frame.
bool install();

}  // namespace rfgvr::video
