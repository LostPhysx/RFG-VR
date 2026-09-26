#pragma once

#include <openxr/openxr.h>

namespace rfgvr::controllers {

// Motion controllers through OpenXR actions (Valve Index bindings, Touch and simple-controller
// fallbacks). The xr module creates, syncs and destroys them; the game thread reads the state.

enum Hand { kLeft = 0, kRight = 1 };

struct HandState {
    bool active = false;      // pose tracked
    XrPosef grip{}, aim{};    // LOCAL space, OpenXR conventions
    XrVector3f velocity{};    // grip linear velocity, LOCAL space, m/s
    float trigger = 0.f, squeeze = 0.f;
    bool a = false, b = false, stickClick = false;
    XrVector2f stick{};
};

struct State {
    HandState hand[2];
};

bool create(XrInstance instance, XrSession session);  // after the session is created
void sync(XrSession session, XrSpace base, XrTime time);  // Present thread, once per headset frame
void destroy();                                        // before the session is destroyed

bool state(State& out);  // any thread: the latest synced state; false if none

}  // namespace rfgvr::controllers
