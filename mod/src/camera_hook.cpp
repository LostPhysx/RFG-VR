#include "camera_hook.h"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "log.h"
#include "vrmath.h"
#include "xr.h"

namespace rfgvr::camera {
namespace {

using namespace vrmath;

// Steam rfg.exe (PE timestamp 0x5B9B718A) virtual addresses, image base 0x400000, rebased at runtime.
// Findings and decompiler references: research/00-local-findings.md.

// FUN_007cfbd0, per-frame main view setup (game thread). Copies the game camera's render_pos,
// render_orient and real_fov into the main rl_camera, then calls rl_camera::render_begin. Culling,
// the GPU view and the projection all derive from those three fields.
constexpr uintptr_t kMainViewSetupVa = 0x7CFBD0;
constexpr uintptr_t kRfgCameraVa = 0x01DE4B50;   // game camera (RFGR_Types rfg_camera)
constexpr uintptr_t kRealFovOff = 0xBC;          // float real_fov (vertical, degrees)
constexpr uintptr_t kRenderPosOff = 0x10C;       // vector render_pos
constexpr uintptr_t kRenderOrientOff = 0x118;    // matrix render_orient (rows: right, up, forward)
constexpr uintptr_t kMainCameraPtrVa = 0x1DE48E4;  // rl_camera* main camera (FUN_006c95d0 returns it)

// rl_camera::render_begin(this, rl_renderer*), __thiscall. The Present thread calls it for the main
// camera right before drawing the frame it is about to present.
constexpr uintptr_t kRenderBeginVa = 0x537660;

// keen render swapchain resize, bool __cdecl(RenderSwapChain*, width, height). FUN_00c6dd00 calls it
// every frame with the window's client size before beginFrame; it only acts on a size change, then
// resizes the DXGI buffers and recreates the engine's render targets from them.
constexpr uintptr_t kSwapchainResizeVa = 0xC6AB20;

// Partial rl_camera (sizeof 0x5C0), offsets from FUN_00520d70 (perspective setup).
struct rl_camera {
    uint8_t pad00[0x2C];
    float pos[3];           // 0x02C
    uint8_t pad38[0xC0 - 0x38];
    float proj[16];         // 0x0C0  row-major, left-handed, z 0..1
    uint8_t pad100[0x594 - 0x100];
    float far_clip;         // 0x594
    uint8_t pad598[0x59C - 0x598];
    int32_t type;           // 0x59C  0 = perspective
};
static_assert(offsetof(rl_camera, pos) == 0x2C);
static_assert(offsetof(rl_camera, proj) == 0xC0);
static_assert(offsetof(rl_camera, far_clip) == 0x594);
static_assert(offsetof(rl_camera, type) == 0x59C);

SafetyHookInline g_mainViewSetup;
SafetyHookInline g_renderBegin;
SafetyHookInline g_swapchainResize;
uintptr_t g_base = 0;
DWORD g_presentThread = 0;

// Recent eye setups (game thread). The Present thread identifies the eye it is drawing by the
// camera position that setup produced, independent of the engine's thread latency.
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

template <typename T>
T* at(uintptr_t va) {
    return reinterpret_cast<T*>(g_base + (va - 0x400000));
}

rl_camera* mainCamera() { return *at<rl_camera*>(kMainCameraPtrVa); }

// Eye pose in the game world: orientation = game camera yaw * head orientation; position = game
// camera position + head/eye offset in the yaw frame.
void applyEye(float* pos, float* orient, const xr::RenderPose& rp) {
    Basis game{{orient[0], orient[1], orient[2]}, {orient[3], orient[4], orient[5]}, {orient[6], orient[7], orient[8]}};
    Basis yaw = yawOnly(game);
    Basis world = compose(basisFromQuat(orientationToLh(rp.pose.orientation)), yaw);
    Vec3 offset = toParent(yaw, positionToLh(rp.pose.position));
    const Vec3 rows[3] = {world.r, world.u, world.f};
    for (int i = 0; i < 3; ++i) {
        orient[i * 3 + 0] = rows[i].x;
        orient[i * 3 + 1] = rows[i].y;
        orient[i * 3 + 2] = rows[i].z;
    }
    pos[0] += offset.x;
    pos[1] += offset.y;
    pos[2] += offset.z;
}

void __cdecl hkMainViewSetup(void* arg) {
    xr::RenderPose rp{};
    if (!xr::renderPose(rp)) {
        g_mainViewSetup.ccall<void>(arg);
        return;
    }

    auto base = at<uint8_t>(kRfgCameraVa);
    auto pos = reinterpret_cast<float*>(base + kRenderPosOff);
    auto orient = reinterpret_cast<float*>(base + kRenderOrientOff);
    auto fov = reinterpret_cast<float*>(base + kRealFovOff);
    float savedPos[3], savedOrient[9], savedFov = *fov;
    memcpy(savedPos, pos, sizeof savedPos);
    memcpy(savedOrient, orient, sizeof savedOrient);

    applyEye(pos, orient, rp);
    *fov = coveringVerticalFovDeg(rp.fov);

    g_mainViewSetup.ccall<void>(arg);

    if (rl_camera* c = mainCamera()) {
        std::scoped_lock lock(g_setupMutex);
        Setup& e = g_ring[g_ringNext++ % 8];
        e.valid = true;
        e.eye = rp.eye;
        e.set = rp.set;
        e.pose = rp.pose;
        memcpy(e.camPos, c->pos, sizeof e.camPos);
    }

    // Restore the game camera so gameplay (aiming, camera smoothing) never sees the head pose.
    memcpy(pos, savedPos, sizeof savedPos);
    memcpy(orient, savedOrient, sizeof savedOrient);
    *fov = savedFov;
}

void __fastcall hkRenderBegin(rl_camera* cam, void* /*edx*/, void* renderer) {
    g_renderBegin.thiscall<void>(cam, renderer);
    if (GetCurrentThreadId() != g_presentThread || cam != mainCamera() || cam->type != 0 || cam->far_clip < 100.f)
        return;

    Setup found{};
    {
        std::scoped_lock lock(g_setupMutex);
        for (const Setup& e : g_ring)
            if (e.valid && std::fabs(e.camPos[0] - cam->pos[0]) < 1e-4f && std::fabs(e.camPos[1] - cam->pos[1]) < 1e-4f &&
                std::fabs(e.camPos[2] - cam->pos[2]) < 1e-4f && (!found.valid || e.set > found.set))
                found = e;  // identical positions (head still): the newest setup wins
    }
    if (!found.valid || cam->proj[0] <= 0.f || cam->proj[5] <= 0.f) return;

    // Submit the fov the engine actually rendered (its symmetric projection).
    float tanH = 1.f / cam->proj[0], tanV = 1.f / cam->proj[5];
    xr::markEyeRendered(found.eye, found.set, found.pose,
                        XrFovf{-std::atan(tanH), std::atan(tanH), std::atan(tanV), -std::atan(tanV)});
}

// Render at the headset's per-eye resolution instead of the window's client size.
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

template <size_t N>
SafetyHookInline hookChecked(uintptr_t va, const uint8_t (&expect)[N], void* detour, const char* what) {
    if (memcmp(at<uint8_t>(va), expect, N) != 0) {
        LOG("%s at %p does not match the expected bytes; not hooked", what, at<void>(va));
        return {};
    }
    return safetyhook::create_inline(at<void>(va), detour);
}

}  // namespace

bool install() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    // push ebp ; mov ebp,esp ; and esp,-16 ; sub esp,0xF4
    static const uint8_t mainView[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00};
    // mov eax,[esp+4] ; mov edx,[eax+0x478] ; mov [edx+0x70],ecx
    static const uint8_t renderBegin[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x90, 0x78, 0x04, 0x00, 0x00, 0x89, 0x4A, 0x70};
    // push ebx ; mov ebx,[esp+0xC] ; push edi ; test ebx,ebx
    static const uint8_t resize[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x57, 0x85, 0xDB};

    g_mainViewSetup = hookChecked(kMainViewSetupVa, mainView, reinterpret_cast<void*>(&hkMainViewSetup), "main view setup");
    g_renderBegin = hookChecked(kRenderBeginVa, renderBegin, reinterpret_cast<void*>(&hkRenderBegin), "render_begin");
    g_swapchainResize = hookChecked(kSwapchainResizeVa, resize, reinterpret_cast<void*>(&hkSwapchainResize), "swapchain resize");

    LOG("Engine hooks: main view setup %s, render_begin %s, swapchain resize %s", g_mainViewSetup ? "ok" : "FAILED",
        g_renderBegin ? "ok" : "FAILED", g_swapchainResize ? "ok" : "FAILED");
    return g_mainViewSetup && g_renderBegin && g_swapchainResize;
}

void onPresent() {
    if (!g_presentThread) g_presentThread = GetCurrentThreadId();
}

}  // namespace rfgvr::camera
