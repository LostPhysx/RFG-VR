#include "input.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "controllers.h"
#include "game.h"
#include "gamestate.h"
#include "log.h"
#include "xr.h"

namespace rfgvr::input {
namespace {

// Analog input: float __cdecl(axis, user), in [-1, 1]. Axis 0 turns the camera, 1 raises it; on foot
// axis 2 moves right and axis 3 forward, relative to the camera's ideal_orient.
constexpr uintptr_t kAnalogVa = 0x6F8E30;
constexpr int kTurnAxis = 0, kWalkRightAxis = 2, kWalkForwardAxis = 3;

// Button getters: bool __cdecl(action, user). Held, pressed this frame, pressed this frame and
// consumed, released this frame.
constexpr uintptr_t kHeldVa = 0x6FE970, kPressedVa = 0x6FEA30, kPressedConsumeVa = 0x6FEAF0, kReleasedVa = 0x6FE9D0;

// Control actions (the engine's CBA_* table).
enum Action {
    kAttack = 20,
    kMelee = 21,
    kJump = 22,
    kSprint = 23,
    kCrouch = 24,
    kReload = 25,
    kZoom = 26,
    kAction = 28,
    kDetonate = 29,
    kPause = 86,
    kWeaponBack = 99,
    kWeaponForward = 100,
    kActionCount = 128,
};

SafetyHookInline g_analog, g_held, g_pressed, g_pressedConsume, g_released;

bool g_active = false;  // controllers drive the game this frame
bool g_down[kActionCount] = {}, g_prev[kActionCount] = {}, g_consumed[kActionCount] = {};
float g_axis[4] = {};
float g_extraWalk[2] = {};
bool g_userWalk = false, g_userWalkSeen = false;

float deadzone(float v, float dz = 0.15f) {
    if (std::fabs(v) < dz) return 0.f;
    return (v - std::copysign(dz, v)) / (1.f - dz);
}

float __cdecl hkAnalog(int axis, int user) {
    float v = g_analog.ccall<float>(axis, user);
    if (user != 0 || axis < 0 || axis > 3) return v;
    if ((axis == kWalkRightAxis || axis == kWalkForwardAxis) && std::fabs(v) > 0.2f) g_userWalkSeen = true;
    float add = g_active ? g_axis[axis] : 0.f;
    if ((axis == kWalkRightAxis || axis == kWalkForwardAxis) && std::fabs(add) > 0.2f) g_userWalkSeen = true;
    if (axis == kWalkRightAxis) add += g_extraWalk[0];
    if (axis == kWalkForwardAxis) add += g_extraWalk[1];
    return std::clamp(v + add, -1.f, 1.f);
}

bool mine(int action) { return g_active && action >= 0 && action < kActionCount; }

template <typename Pred>
uint32_t combine(uint32_t orig, int action, Pred pred) {
    if ((orig & 0xFF) || !mine(action) || !pred(action)) return orig;
    return (orig & ~0xFFu) | 1;
}

uint32_t __cdecl hkHeld(int action, int user) {
    return combine(g_held.ccall<uint32_t>(action, user), user == 0 ? action : -1, [](int a) { return g_down[a]; });
}
uint32_t __cdecl hkPressed(int action, int user) {
    return combine(g_pressed.ccall<uint32_t>(action, user), user == 0 ? action : -1,
                   [](int a) { return g_down[a] && !g_prev[a]; });
}
uint32_t __cdecl hkPressedConsume(int action, int user) {
    return combine(g_pressedConsume.ccall<uint32_t>(action, user), user == 0 ? action : -1, [](int a) {
        if (!g_down[a] || g_prev[a] || g_consumed[a]) return false;
        g_consumed[a] = true;
        return true;
    });
}
uint32_t __cdecl hkReleased(int action, int user) {
    return combine(g_released.ccall<uint32_t>(action, user), user == 0 ? action : -1,
                   [](int a) { return !g_down[a] && g_prev[a]; });
}

// A fast swing of the right controller is a melee attack (the hammer swings), held for a few frames.
constexpr float kSwingSpeed = 2.5f;       // m/s
constexpr DWORD kSwingCooldownMs = 600;
constexpr int kSwingFrames = 4;

bool swing(const controllers::HandState& r) {
    static DWORD last = 0;
    static int frames = 0;
    const XrVector3f& v = r.velocity;
    float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    DWORD now = GetTickCount();
    if (r.active && speed > kSwingSpeed && now - last > kSwingCooldownMs) {
        last = now;
        frames = kSwingFrames;
        LOG("swing: %.1f m/s", speed);
    }
    if (frames <= 0) return false;
    --frames;
    return true;
}

// Stick direction as a button, with hysteresis.
bool stickButton(bool was, float v) { return was ? v > 0.5f : v > 0.8f; }

}  // namespace

bool install() {
    const std::initializer_list<uint8_t> getter = {0x56, 0x8B, 0x74, 0x24, 0x0C, 0x83, 0xFE, 0x01};
    g_analog = game::hook(kAnalogVa, {0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x14}, &hkAnalog, "analog input");
    g_held = game::hook(kHeldVa, getter, &hkHeld, "button held");
    g_pressed = game::hook(kPressedVa, getter, &hkPressed, "button pressed");
    g_pressedConsume = game::hook(kPressedConsumeVa, getter, &hkPressedConsume, "button pressed (consume)");
    g_released = game::hook(kReleasedVa, getter, &hkReleased, "button released");
    bool all = g_analog && g_held && g_pressed && g_pressedConsume && g_released;
    LOG("Input hooks: %s", all ? "ok" : "FAILED");
    return all;
}

void update() {
    memcpy(g_prev, g_down, sizeof g_down);
    memset(g_down, 0, sizeof g_down);
    memset(g_axis, 0, sizeof g_axis);
    g_userWalk = g_userWalkSeen;
    g_userWalkSeen = false;

    controllers::State s;
    g_active = gamestate::gameplay() && xr::stereoActive() && controllers::state(s) &&
               (s.hand[0].active || s.hand[1].active);
    if (!g_active) {
        memset(g_consumed, 0, sizeof g_consumed);
        return;
    }
    const controllers::HandState &l = s.hand[controllers::kLeft], &r = s.hand[controllers::kRight];

    g_axis[kWalkRightAxis] = deadzone(l.stick.x);
    g_axis[kWalkForwardAxis] = deadzone(l.stick.y);
    g_axis[kTurnAxis] = deadzone(r.stick.x);

    g_down[kSprint] = l.stickClick;
    g_down[kDetonate] = l.trigger > 0.5f;
    g_down[kZoom] = l.squeeze > 0.7f;
    g_down[kAction] = l.a;
    g_down[kPause] = l.b;

    g_down[kAttack] = r.trigger > 0.5f;
    g_down[kMelee] = r.squeeze > 0.7f || swing(r);
    g_down[kJump] = r.a;
    g_down[kReload] = r.b;
    g_down[kCrouch] = r.stickClick;
    g_down[kWeaponForward] = stickButton(g_prev[kWeaponForward], r.stick.y);
    g_down[kWeaponBack] = stickButton(g_prev[kWeaponBack], -r.stick.y);

    for (int a = 0; a < kActionCount; ++a)
        if (!g_down[a]) g_consumed[a] = false;
}

void setExtraWalk(float right, float forward) {
    g_extraWalk[0] = right;
    g_extraWalk[1] = forward;
}

bool userWalking() { return g_userWalk; }

}  // namespace rfgvr::input
