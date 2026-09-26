#pragma once

namespace rfgvr::config {

// rfg-vr.ini next to the DLL ([VR] section, see mod/README.md), re-read when it changes.
void poll();  // game thread, once per frame (checks the file about once a second)

float worldScale();
bool cameraShake();
bool headAim();
bool hudLayer();
float hudDistance();
float hudWidth();

}  // namespace rfgvr::config
