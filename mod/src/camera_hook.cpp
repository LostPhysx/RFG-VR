#include "camera_hook.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "config.h"
#include "game.h"
#include "gamestate.h"
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
SafetyHookMid g_freeCamPitch;
DWORD g_presentThread = 0;

// Head aim: between camera updates real_orient holds game yaw * head; the game's own value is kept.
bool g_headAimed = false;
float g_gameOrient[9] = {};

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

void __cdecl hkCameraUpdate() {
    auto real = reinterpret_cast<float*>(&at<uint8_t>(kRfgCameraVa) + kRealOrientOff);
    if (g_headAimed) memcpy(real, g_gameOrient, sizeof g_gameOrient);
    g_headAimed = false;
    g_cameraUpdate.ccall<void>();

    XrQuaternionf head{};
    if (!config::headAim() || !gamestate::gameplay() || !xr::headOrientation(head)) return;
    memcpy(g_gameOrient, real, sizeof g_gameOrient);
    store(real, compose(basisFromQuat(orientationToLh(head)), yawOnly(basisOf(real))));
    g_headAimed = true;
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
    g_freeCamPitch = game::hookMid(kFreeCamCoreCallVa, {0xE8, 0x75, 0xD1, 0xFF, 0xFF}, &freeCamPitch, "vehicle camera");

    LOG("Camera hooks: main view setup %s, render_begin %s, swapchain resize %s, camera update %s, shake %s, "
        "third-person %s, vehicle %s",
        ok(g_mainViewSetup), ok(g_renderBegin), ok(g_swapchainResize), ok(g_cameraUpdate), ok(g_shakeEval),
        ok(g_thirdPerson), ok(g_freeCamPitch));
    return g_mainViewSetup && g_renderBegin && g_swapchainResize;
}

void onPresent() {
    if (!g_presentThread) g_presentThread = GetCurrentThreadId();
}

}  // namespace rfgvr::camera
