#include "camera_hook.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <mutex>

#include "config.h"
#include "controllers.h"
#include "game.h"
#include "gamestate.h"
#include "input.h"
#include "log.h"
#include "vrmath.h"
#include "xr.h"

namespace rfgvr::camera {
namespace {

using namespace vrmath;
using game::at;

// Game camera (RFGR_Types rfg_camera).
constexpr uintptr_t kRfgCameraVa = 0x01DE4B50;
constexpr uintptr_t kRealOrientOff = 0x50;     // matrix, rows: right, up, forward
constexpr uintptr_t kIdealOrientOff = 0x74;
constexpr uintptr_t kRealPosOff = 0x2C;
constexpr uintptr_t kRealFovOff = 0xBC;        // float, vertical degrees
constexpr uintptr_t kRenderPosOff = 0x10C;
constexpr uintptr_t kRenderOrientOff = 0x118;
constexpr uintptr_t kMainCameraPtrVa = 0x1DE48E4;  // rl_camera* of the main view

// Main view setup (game thread): copies render_pos/orient and real_fov into the main rl_camera.
constexpr uintptr_t kMainViewSetupVa = 0x7CFBD0;
// rl_camera::render_begin (__thiscall): the Present thread calls it before drawing a view.
constexpr uintptr_t kRenderBeginVa = 0x537660;
// keen swapchain resize, bool __cdecl(swapchain, w, h), called every frame with the window size.
constexpr uintptr_t kSwapchainResizeVa = 0xC6AB20;
// Camera update (game thread): mode update, real_pos/orient, shake. Aiming uses real_orient.
constexpr uintptr_t kCameraUpdateVa = 0x6DFFA0;
// Shake evaluation: sums the 5 active shake slots into real_orient.
constexpr uintptr_t kShakeEvalVa = 0x6CCDF0;
constexpr uintptr_t kShakeSlotsVa = 0x01DE4554;  // camera_shake*[5]
// On-foot camera (CAMERA_THIRD_PERSON_MODE) update; orbits at lookaround pitch/heading.
constexpr uintptr_t kThirdPersonUpdateVa = 0x6DD250;
constexpr uintptr_t kLookPitchVa = 0x01DE4D70;
// Vehicle camera (CAMERA_FREE_MODE): call into its core update, which applies the look input.
constexpr uintptr_t kFreeCamCoreCallVa = 0x6DA006;
constexpr uintptr_t kFreeCamUserElevVa = 0x01DE4CC0;    // mouse / stick pitch input
constexpr uintptr_t kFreeCamMousePitchVa = 0x01DE4D54;  // absolute-mouse pitch (vehicle_mouse_cam)
constexpr int kThirdPersonMode = 10;
// Local player (human*), and the eye position: void __cdecl(human*, vector* pos, matrix* orient or null),
// the midpoint of the two eye bones.
constexpr uintptr_t kLocalPlayerVa = 0x03023874;
constexpr uintptr_t kHumanEyePosVa = 0xAB2260;
constexpr uintptr_t kObjectPosOff = 0x4;  // object::pos (vector)
// Human render update (__fastcall(human*)): stores whether its skin instances are shown (AL; the
// human is in ESI), then applies it to both instances (body, second mesh).
constexpr uintptr_t kHumanShowSkinVa = 0xA9BD0C;

struct rl_camera {  // partial, sizeof 0x5C0
    uint8_t pad00[0x2C];
    float pos[3];  // 0x02C
    uint8_t pad38[0xC0 - 0x38];
    float proj[16];  // 0x0C0, row-major, left-handed
    uint8_t pad100[0x594 - 0x100];
    float far_clip;  // 0x594
    uint8_t pad598[0x59C - 0x598];
    int32_t type;  // 0x59C, 0 = perspective
};
static_assert(offsetof(rl_camera, pos) == 0x2C);
static_assert(offsetof(rl_camera, proj) == 0xC0);
static_assert(offsetof(rl_camera, far_clip) == 0x594);
static_assert(offsetof(rl_camera, type) == 0x59C);

SafetyHookInline g_mainViewSetup, g_renderBegin, g_swapchainResize, g_cameraUpdate, g_shakeEval, g_thirdPerson;
SafetyHookMid g_freeCamPitch, g_humanShowSkin;
DWORD g_presentThread = 0;

// Head aim: between camera updates real_orient holds game yaw * head; the game's own value is kept.
bool g_headAimed = false;
float g_gameOrient[9] = {};
bool g_idealAimed = false;
bool g_handAimed = false;
std::atomic<bool> g_handAimActive{false};  // read on the Present thread
float g_gamePos[3] = {};
float g_gameIdeal[9] = {};

// Recent eye setups. The Present thread draws a view one frame after its setup and identifies the
// eye by the camera position that setup produced.
struct Setup {
    bool valid = false;
    int eye = 0;
    uint32_t set = 0;
    XrPosef pose{};
    float camPos[3] = {};
};
std::mutex g_setupMutex;
Setup g_ring[8];
uint32_t g_ringNext = 0;

rl_camera* mainCamera() { return at<rl_camera*>(kMainCameraPtrVa); }

Basis basisOf(const float* m) { return {{m[0], m[1], m[2]}, {m[3], m[4], m[5]}, {m[6], m[7], m[8]}}; }

void store(float* m, const Basis& b) {
    const Vec3 rows[3] = {b.r, b.u, b.f};
    for (int i = 0; i < 3; ++i) {
        m[i * 3 + 0] = rows[i].x;
        m[i * 3 + 1] = rows[i].y;
        m[i * 3 + 2] = rows[i].z;
    }
}

// Eye pose in the game: game yaw * eye orientation; camera position + eye offset (scaled by
// 1/WorldScale) in the yaw frame.
void applyEye(float* pos, float* orient, const xr::RenderPose& rp) {
    Basis yaw = yawOnly(basisOf(orient));
    store(orient, compose(basisFromQuat(orientationToLh(rp.pose.orientation)), yaw));
    Vec3 offset = toParent(yaw, positionToLh(rp.pose.position)) * (1.f / config::worldScale());
    pos[0] += offset.x;
    pos[1] += offset.y;
    pos[2] += offset.z;
}

// FirstPerson, on foot: the camera is at the character's eyes horizontally; vertically at its feet
// plus the LOCAL origin's real height above the floor, so the head offset gives the real eye height.
bool firstPersonOnFoot() {
    return config::firstPerson() && gamestate::gameplay() && at<int>(kRfgCameraVa) == kThirdPersonMode;
}

// In first person the player's own character is not drawn (the camera is inside it). Hiding the
// object itself (human_hide, or only its visible bit) makes the player fall through the ground.
bool g_hidePlayer = false;

// Clothing (e.g. the jacket) is an attached item (object type 1, no subtype), hidden through its
// visible bit with object_set_visible (void __cdecl(object*, bool), recursive). Weapons stay.
constexpr uintptr_t kObjectSetVisibleVa = 0xA8C440;
constexpr uint8_t kItemType = 1, kWeaponSubtype = 7;
uint8_t* g_hiddenItems[8] = {};

template <typename Fn>
void forEachChild(uint8_t* obj, Fn fn) {
    auto first = *reinterpret_cast<uint8_t**>(obj + 0x34);  // object::child_ptr, then child_next (+0x38)
    int n = 0;
    for (uint8_t* c = first; c && n < 32; ++n) {
        fn(c);
        c = *reinterpret_cast<uint8_t**>(c + 0x38);
        if (c == first) break;
    }
}

void hideClothing(uint8_t* player, bool hide) {
    using SetVisible = void(__cdecl*)(void*, bool);
    auto setVisible = reinterpret_cast<SetVisible>(game::runtime(kObjectSetVisibleVa));
    forEachChild(player, [&](uint8_t* c) {
        bool known = false;
        for (auto& h : g_hiddenItems)
            if (h == c) {
                known = true;
                if (!hide) {
                    setVisible(c, true);
                    h = nullptr;
                }
            }
        if (!hide || known || c[0x7E] != kItemType || c[0x7F] == kWeaponSubtype || !(c[0x56] & 0x80)) return;
        for (auto& h : g_hiddenItems)
            if (!h) {
                setVisible(c, false);
                h = c;
                break;
            }
    });
    if (!hide)
        for (auto& h : g_hiddenItems) h = nullptr;  // no longer attached: not ours to restore
}

void humanShowSkin(safetyhook::Context& ctx) {
    if (g_hidePlayer && ctx.esi == reinterpret_cast<uintptr_t>(at<void*>(kLocalPlayerVa))) ctx.eax &= ~0xFFu;
}

// Room-scale, first person on foot. `ref` is the point of the room (LOCAL, game handedness, ground
// plane) that corresponds to the character's position: the camera is the character's position plus
// the head's offset from `ref`. When the head gets away from the character, walk input towards it is
// added; the distance the character covers because of that moves `ref` along, so it catches up.
// The offset is limited, so the head cannot go far through walls.
constexpr float kFollowStart = 0.12f, kFollowStop = 0.04f;  // metres of offset
constexpr float kFullSpeedAt = 0.6f, kMinStick = 0.3f;
constexpr float kMaxOffset = 0.6f;

struct RoomScale {
    bool valid = false;
    Vec3 ref;
    float lastRoot[3] = {};
    bool following = false;
} g_rs;

void roomScaleUpdate(const Basis& yaw, const float* ideal) {
    auto player = at<uint8_t*>(kLocalPlayerVa);
    XrVector3f h{};
    if (!player || !firstPersonOnFoot() || !xr::stereoActive() || !xr::headPosition(h)) {
        g_rs = {};
        input::setExtraWalk(0.f, 0.f);
        return;
    }
    const float scale = config::worldScale();
    Vec3 head = positionToLh(h);
    head.y = 0;
    auto root = reinterpret_cast<const float*>(player + kObjectPosOff);
    if (!g_rs.valid) {
        g_rs = {};
        g_rs.valid = true;
        g_rs.ref = head;
        memcpy(g_rs.lastRoot, root, sizeof g_rs.lastRoot);
    }
    Vec3 moved{root[0] - g_rs.lastRoot[0], 0, root[2] - g_rs.lastRoot[2]};
    memcpy(g_rs.lastRoot, root, sizeof g_rs.lastRoot);
    if (g_rs.following && !input::userWalking() && length(moved) < 2.f) {
        Vec3 local = toLocal(yaw, moved) * scale;
        g_rs.ref = g_rs.ref + Vec3{local.x, 0, local.z};
    }
    Vec3 off = head - g_rs.ref;
    if (length(off) > kMaxOffset * scale) {
        g_rs.ref = head - normalize(off) * (kMaxOffset * scale);
        off = head - g_rs.ref;
    }
    Vec3 world = toParent(yaw, off) * (1.f / scale);
    float d = length(world);
    g_rs.following = d > (g_rs.following ? kFollowStop : kFollowStart);
    if (!g_rs.following) {
        input::setExtraWalk(0.f, 0.f);
        return;
    }
    Basis move = yawOnly(basisOf(ideal));  // the frame walk input is relative to
    float speed = std::clamp(d / kFullSpeedAt, kMinStick, 1.f) / d;
    float right = dot(world, move.r) * speed, forward = dot(world, move.f) * speed;
    input::setExtraWalk(right, forward);

    static DWORD lastLog = 0;
    if (DWORD now = GetTickCount(); now - lastLog > 1000) {
        lastLog = now;
        LOG("room-scale: offset %.2f m, walk %.2f %.2f, user walking %d", d, right, forward, input::userWalking());
    }
}

bool firstPersonEye(float* pos, const float* orient, const xr::RenderPose& rp) {
    if (!firstPersonOnFoot()) return false;
    auto player = at<uint8_t*>(kLocalPlayerVa);
    if (!player) return false;
    using EyePos = void(__cdecl*)(void*, float*, float*);
    reinterpret_cast<EyePos>(game::runtime(kHumanEyePosVa))(player, pos, nullptr);
    float eyeY = pos[1];
    auto feet = reinterpret_cast<const float*>(player + kObjectPosOff);
    if (rp.haveFloor) pos[1] = feet[1] + rp.localHeight / config::worldScale();
    if (g_rs.valid) {
        Vec3 w = toParent(yawOnly(basisOf(orient)), g_rs.ref) * (1.f / config::worldScale());
        pos[0] = feet[0] - w.x;
        pos[2] = feet[2] - w.z;
    }

    static DWORD lastLog = 0;
    if (DWORD now = GetTickCount(); now - lastLog > 2000) {
        lastLog = now;
        LOG("first person: feet y %.3f, eye bones y %.3f (+%.3f), LOCAL origin %.3f above floor (%s), head y %.3f",
            feet[1], eyeY, eyeY - feet[1], rp.localHeight, rp.haveFloor ? "stage" : "no stage", rp.pose.position.y);
    }
    return true;
}

void __cdecl hkMainViewSetup(void* arg) {
    static int lastMode = -1;  // camera_mode: 10 on foot, 0 in vehicles
    if (int mode = at<int>(kRfgCameraVa); mode != lastMode) LOG("camera mode -> %d", lastMode = mode);
    config::poll();

    xr::RenderPose rp{};
    if (!xr::renderPose(rp)) {
        // No eye this frame: drop old setups so an unchanged camera is not taken for an eye image.
        std::scoped_lock lock(g_setupMutex);
        for (Setup& e : g_ring) e.valid = false;
    } else {
        auto cam = &at<uint8_t>(kRfgCameraVa);
        auto pos = reinterpret_cast<float*>(cam + kRenderPosOff);
        auto orient = reinterpret_cast<float*>(cam + kRenderOrientOff);
        auto fov = reinterpret_cast<float*>(cam + kRealFovOff);
        float savedPos[3], savedOrient[9], savedFov = *fov;
        memcpy(savedPos, pos, sizeof savedPos);
        memcpy(savedOrient, orient, sizeof savedOrient);
        if (g_headAimed) memcpy(orient, g_gameOrient, sizeof g_gameOrient);  // head is applied per eye

        firstPersonEye(pos, orient, rp);
        applyEye(pos, orient, rp);
        *fov = coveringVerticalFovDeg(rp.fov);
        g_mainViewSetup.ccall<void>(arg);

        if (rl_camera* c = mainCamera()) {
            std::scoped_lock lock(g_setupMutex);
            g_ring[g_ringNext++ % 8] = {true, rp.eye, rp.set, rp.pose, {c->pos[0], c->pos[1], c->pos[2]}};
        }
        memcpy(pos, savedPos, sizeof savedPos);
        memcpy(orient, savedOrient, sizeof savedOrient);
        *fov = savedFov;
        return;
    }
    g_mainViewSetup.ccall<void>(arg);
}

void __fastcall hkRenderBegin(rl_camera* cam, void* /*edx*/, void* renderer) {
    g_renderBegin.thiscall<void>(cam, renderer);
    if (GetCurrentThreadId() != g_presentThread || cam != mainCamera() || cam->type != 0 || cam->far_clip < 100.f)
        return;

    Setup found{};
    {
        std::scoped_lock lock(g_setupMutex);
        for (const Setup& e : g_ring)
            if (e.valid && std::fabs(e.camPos[0] - cam->pos[0]) < 1e-4f &&
                std::fabs(e.camPos[1] - cam->pos[1]) < 1e-4f && std::fabs(e.camPos[2] - cam->pos[2]) < 1e-4f &&
                (!found.valid || e.set > found.set))
                found = e;  // head still, same position: the newest wins
    }
    if (!found.valid || cam->proj[0] <= 0.f || cam->proj[5] <= 0.f) return;

    // Submit the fov the engine rendered (its symmetric projection).
    float tanH = 1.f / cam->proj[0], tanV = 1.f / cam->proj[5];
    xr::markEyeRendered(found.eye, found.set, found.pose,
                        XrFovf{-std::atan(tanH), std::atan(tanH), std::atan(tanV), -std::atan(tanV)});
}

// Render at the headset eye size instead of the window size.
uint8_t __cdecl hkSwapchainResize(void* swapchain, int w, int h) {
    uint32_t vw = 0, vh = 0;
    if (w > 0 && h > 0 && xr::eyeRenderSize(vw, vh)) {
        static uint32_t lastW = 0, lastH = 0;
        if (vw != lastW || vh != lastH) {
            LOG("Render size: window %dx%d -> headset eye %ux%u", w, h, vw, vh);
            lastW = vw;
            lastH = vh;
        }
        w = static_cast<int>(vw);
        h = static_cast<int>(vh);
    }
    return g_swapchainResize.ccall<uint8_t>(swapchain, w, h);
}

// Head aim: real_orient = head orientation in the game's yaw frame (the game's value is restored
// before the next update).
void headAim(float* real, float* ideal, const XrQuaternionf& head) {
    memcpy(g_gameOrient, real, sizeof g_gameOrient);
    store(real, compose(basisFromQuat(orientationToLh(head)), yawOnly(basisOf(real))));
    g_headAimed = true;
    if (config::firstPerson()) {  // walking follows ideal_orient: in first person, where the head looks
        memcpy(g_gameIdeal, ideal, sizeof g_gameIdeal);
        memcpy(ideal, real, sizeof g_gameIdeal);
        g_idealAimed = true;
    }
}

// First person: where the room (LOCAL space) is in the world this frame. A LOCAL point p is at
// origin + yaw * p / WorldScale (p in game handedness).
struct RoomPose {
    bool valid = false;
    Vec3 origin;
    Basis yaw;
} g_room;

void roomPoseUpdate(const Basis& yaw) {
    g_room.valid = false;
    float height = 0.f;
    auto player = at<uint8_t*>(kLocalPlayerVa);
    if (!g_rs.valid || !player || !xr::localHeight(height)) return;
    const float inv = 1.f / config::worldScale();
    auto feet = reinterpret_cast<const float*>(player + kObjectPosOff);
    g_room.origin = Vec3{feet[0], feet[1] + height * inv, feet[2]} - toParent(yaw, g_rs.ref) * inv;
    g_room.yaw = yaw;
    g_room.valid = true;
}

Vec3 roomToWorld(const XrVector3f& p) {
    return g_room.origin + toParent(g_room.yaw, positionToLh(p)) * (1.f / config::worldScale());
}
Basis roomToWorld(const XrQuaternionf& q) { return compose(basisFromQuat(orientationToLh(q)), g_room.yaw); }

// First person with controllers: aiming (real_pos / real_orient) follows the right controller's aim
// pose; walking (ideal_orient) still follows the head.
void handAim(float* realPos, float* real) {
    controllers::State cs;
    if (!g_room.valid || !controllers::state(cs) || !cs.hand[controllers::kRight].active) return;
    const controllers::HandState& r = cs.hand[controllers::kRight];
    Vec3 hand = roomToWorld(r.aim.position);
    if (!g_handAimed) memcpy(g_gamePos, realPos, sizeof g_gamePos);
    if (!g_headAimed) memcpy(g_gameOrient, real, sizeof g_gameOrient);
    realPos[0] = hand.x;
    realPos[1] = hand.y;
    realPos[2] = hand.z;
    store(real, roomToWorld(r.aim.orientation));
    g_handAimed = g_headAimed = true;
}

// First person with controllers: the equipped weapon is drawn at the right hand (grip position,
// barrel along the aim ray). The item render update (__thiscall(item, arg)) copies the object's
// pos (+4) / orient (+0x10) into its render instance; they are swapped for the call only.
constexpr uintptr_t kItemRenderUpdateVa = 0xA65190;
SafetyHookInline g_itemRenderUpdate;
uint8_t* g_handWeapon = nullptr;
float g_handWeaponPose[12] = {};  // pos, orient rows

void weaponInHandUpdate() {
    g_handWeapon = nullptr;
    controllers::State cs;
    auto player = at<uint8_t*>(kLocalPlayerVa);
    if (!g_room.valid || !player || !controllers::state(cs) || !cs.hand[controllers::kRight].active) return;
    forEachChild(player, [](uint8_t* c) {
        if (!g_handWeapon && c[0x7E] == kItemType && c[0x7F] == kWeaponSubtype) g_handWeapon = c;
    });
    const controllers::HandState& r = cs.hand[controllers::kRight];
    Vec3 p = roomToWorld(r.grip.position);
    g_handWeaponPose[0] = p.x;
    g_handWeaponPose[1] = p.y;
    g_handWeaponPose[2] = p.z;
    store(g_handWeaponPose + 3, roomToWorld(r.aim.orientation));
}

void __fastcall hkItemRenderUpdate(uint8_t* item, void* /*edx*/, void* arg) {
    if (!item || item != g_handWeapon) return g_itemRenderUpdate.thiscall<void>(item, arg);
    float saved[12];
    memcpy(saved, item + 4, sizeof saved);
    memcpy(item + 4, g_handWeaponPose, sizeof saved);
    g_itemRenderUpdate.thiscall<void>(item, arg);
    memcpy(item + 4, saved, sizeof saved);
}

void __cdecl hkCameraUpdate() {
    auto cam = &at<uint8_t>(kRfgCameraVa);
    auto real = reinterpret_cast<float*>(cam + kRealOrientOff);
    auto ideal = reinterpret_cast<float*>(cam + kIdealOrientOff);
    auto realPos = reinterpret_cast<float*>(cam + kRealPosOff);
    if (g_headAimed) memcpy(real, g_gameOrient, sizeof g_gameOrient);
    if (g_idealAimed) memcpy(ideal, g_gameIdeal, sizeof g_gameIdeal);
    if (g_handAimed) memcpy(realPos, g_gamePos, sizeof g_gamePos);
    g_headAimed = g_idealAimed = g_handAimed = false;
    input::update();
    g_cameraUpdate.ccall<void>();
    bool hide = firstPersonOnFoot() && xr::stereoActive();
    static uint8_t* clothed = nullptr;  // player whose clothing is hidden
    auto player = at<uint8_t*>(kLocalPlayerVa);
    if (clothed && (!hide || clothed != player)) {
        if (clothed == player) hideClothing(player, false);
        for (auto& h : g_hiddenItems) h = nullptr;
        clothed = nullptr;
    }
    if (hide && player) {
        hideClothing(player, true);
        clothed = player;
    }
    g_hidePlayer = hide;

    Basis gameYaw = yawOnly(basisOf(real));
    XrQuaternionf head{};
    if (config::headAim() && gamestate::gameplay() && xr::headOrientation(head)) headAim(real, ideal, head);
    roomScaleUpdate(gameYaw, ideal);
    roomPoseUpdate(gameYaw);
    if (firstPersonOnFoot() && xr::stereoActive()) handAim(realPos, real);
    weaponInHandUpdate();
    g_handAimActive = g_handAimed;
}


// CameraShake=0: empty the active shake slots (sounds still play).
void __cdecl hkShakeEval() {
    if (!config::cameraShake()) memset(&at<void*>(kShakeSlotsVa), 0, 5 * sizeof(void*));
    g_shakeEval.ccall<void>();
}

// Head aim in VR: no vertical look input for the on-foot and vehicle cameras.
bool pitchLocked() { return config::headAim() && xr::stereoActive(); }

void __cdecl hkThirdPerson() {
    if (pitchLocked()) at<float>(kLookPitchVa) = 0.f;
    g_thirdPerson.ccall<void>();
}

void freeCamPitch(safetyhook::Context&) {
    if (!pitchLocked()) return;
    at<float>(kFreeCamUserElevVa) = 0.f;
    at<float>(kFreeCamMousePitchVa) = 0.f;
}

template <typename Hook>
const char* ok(const Hook& h) {
    return h ? "ok" : "FAILED";
}

}  // namespace

bool install() {
    // The expected bytes are the functions' prologues.
    g_mainViewSetup = game::hook(kMainViewSetupVa, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xF4, 0x00},
                                 &hkMainViewSetup, "main view setup");
    g_renderBegin = game::hook(kRenderBeginVa, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x90, 0x78, 0x04, 0x00, 0x00},
                               &hkRenderBegin, "render_begin");
    g_swapchainResize = game::hook(kSwapchainResizeVa, {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x57, 0x85, 0xDB},
                                   &hkSwapchainResize, "swapchain resize");
    g_cameraUpdate = game::hook(kCameraUpdateVa, {0x83, 0xEC, 0x6C, 0x56, 0x50}, &hkCameraUpdate, "camera update",
                                5);  // after mov eax,[relocated address]
    g_shakeEval = game::hook(kShakeEvalVa, {0x81, 0xEC, 0xEC, 0x02, 0x00, 0x00}, &hkShakeEval, "camera shake");
    g_thirdPerson = game::hook(kThirdPersonUpdateVa, {0x83, 0xEC, 0x70, 0xE8}, &hkThirdPerson, "third-person camera");
    g_humanShowSkin = game::hookMid(kHumanShowSkinVa, {0x88, 0x44, 0x24, 0x24, 0x3A, 0xC2}, &humanShowSkin,
                                    "player model");
    g_itemRenderUpdate = game::hook(kItemRenderUpdateVa, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54},
                                    &hkItemRenderUpdate, "weapon in hand");
    g_freeCamPitch = game::hookMid(kFreeCamCoreCallVa, {0xE8, 0x75, 0xD1, 0xFF, 0xFF}, &freeCamPitch, "vehicle camera");

    LOG("Camera hooks: main view setup %s, render_begin %s, swapchain resize %s, camera update %s, shake %s, "
        "third-person %s, vehicle %s, player model %s, weapon in hand %s",
        ok(g_mainViewSetup), ok(g_renderBegin), ok(g_swapchainResize), ok(g_cameraUpdate), ok(g_shakeEval),
        ok(g_thirdPerson), ok(g_freeCamPitch), ok(g_humanShowSkin), ok(g_itemRenderUpdate));
    return g_mainViewSetup && g_renderBegin && g_swapchainResize;
}

bool handAimActive() { return g_handAimActive; }

void onPresent() {
    if (!g_presentThread) g_presentThread = GetCurrentThreadId();
}

}  // namespace rfgvr::camera
