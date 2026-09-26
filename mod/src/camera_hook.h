#pragma once

namespace rfgvr::camera {

// Engine camera hooks (Steam build): per-eye rendering of the main view, eye identification at
// render time, render size, head aim, pitch lock (on foot and in vehicles), camera shake.
bool install();

// Present thread, every frame.
void onPresent();

// Aiming follows the right controller (first person with controllers).
bool handAimActive();

}  // namespace rfgvr::camera
