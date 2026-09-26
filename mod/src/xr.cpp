#include "xr.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "camera_hook.h"
#include "config.h"
#include "controllers.h"
#include "game.h"
#include "gamestate.h"
#include "hud.h"
#include "log.h"

namespace rfgvr::xr {
namespace {

// Virtual screen in LOCAL space (seated origin; recenter via the SteamVR dashboard), metres.
constexpr float kScreenDistance = 2.5f;
constexpr float kScreenWidth = 3.2f;
constexpr uint64_t kRetryPresents = 600;  // between xrGetSystem retries while no headset is found

enum class Phase { Uninit, Retry, Ready, Disabled };
Phase g_phase = Phase::Uninit;
uint64_t g_presentCount = 0;
uint64_t g_retryAt = 0;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_systemId = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;  // head-locked (HUD panel with head aim)
XrSpace g_stageSpace = XR_NULL_HANDLE;  // floor level (first-person eye height)
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
std::vector<int64_t> g_formats;

ID3D11Device* g_device = nullptr;      // the game's device (AddRef'd via GetDevice)
ID3D11DeviceContext* g_ctx = nullptr;  // its immediate context (AddRef'd)

// Runtime swapchain matching a game texture, so it can be filled with CopyResource.
struct Swap {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;  // owned by the runtime; valid while handle lives
    uint32_t w = 0, h = 0;
    DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN;  // backbuffer format this swapchain was built for
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;     // swapchain format
    bool failed = false;                          // creation failed for this size/format; don't retry
};
Swap g_mirror;  // virtual screen (menus, videos, loading)
Swap g_hud;     // UI panel over the stereo view
Swap g_reticle;  // aim marker on the right controller's ray (hand aim)
ID3D11Texture2D* g_reticleTex = nullptr;
constexpr float kReticleDistance = 15.f;  // metres along the ray
constexpr float kReticleAngle = 0.03f;    // size / distance

struct Eye {
    Swap swap;
    bool have = false;  // holds this headset frame's image, rendered with `pose`/`fov`
    XrPosef pose{};
    XrFovf fov{};
};
Eye g_eyes[2];

// One headset frame spans two game frames (left eye, then right eye, same located views).
bool g_frameOpen = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};
uint32_t g_presentsInFrame = 0;  // Presents since the headset frame was opened
uint32_t g_idlePresents = 0;     // consecutive Presents without an eye image

std::mutex g_viewMutex;  // the views are read on the game thread
XrView g_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_viewsValid = false;
float g_localHeight = 0.f;  // height of the LOCAL origin above the STAGE floor
bool g_haveFloor = false;
uint32_t g_viewSet = 0;       // id of the open headset frame's views
uint32_t g_requests = 0;      // eye setups handed out for the current view set

// Eye image in the backbuffer at this Present, reported by the camera hook.
bool g_eyeRendered = false;
int g_renderedEye = 0;
uint32_t g_renderedSet = 0;
XrPosef g_renderedPose{};
XrFovf g_renderedFov{};
uint64_t g_staleImages = 0;

// SteamVR's recommended eye size (pixel density) and the eye fov.
uint32_t g_recW[2] = {}, g_recH[2] = {};
bool g_haveFov = false;
XrFovf g_eyeFov[2] = {};

uint64_t g_xrFrames = 0;
uint64_t g_stereoFrames = 0;

const char* str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(g_instance, r, buf)))
        snprintf(buf, sizeof buf, "XrResult %d", static_cast<int>(r));
    return buf;
}

bool check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG("%s -> %s", what, str(r));
    return false;
}
#define XR_OK(expr) check((expr), #expr)

bool disabledByFile() {
    return GetFileAttributesW(game::pathNextToDll(L"rfg-vr-novr.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
}

void destroySwap(Swap& s) {
    if (s.handle != XR_NULL_HANDLE) xrDestroySwapchain(s.handle);
    s = {};
}

void teardown(const char* why) {
    LOG("OpenXR teardown: %s", why);
    destroySwap(g_mirror);
    destroySwap(g_hud);
    destroySwap(g_reticle);
    if (g_reticleTex) g_reticleTex->Release();
    g_reticleTex = nullptr;
    for (auto& e : g_eyes) {
        destroySwap(e.swap);
        e.have = false;
    }
    g_frameOpen = false;
    controllers::destroy();
    if (g_viewSpace != XR_NULL_HANDLE) xrDestroySpace(g_viewSpace);
    if (g_stageSpace != XR_NULL_HANDLE) xrDestroySpace(g_stageSpace);
    g_stageSpace = XR_NULL_HANDLE;
    if (g_space != XR_NULL_HANDLE) xrDestroySpace(g_space);
    g_viewSpace = XR_NULL_HANDLE;
    if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
    if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
    g_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
    if (g_ctx) g_ctx->Release();
    if (g_device) g_device->Release();
    g_ctx = nullptr;
    g_device = nullptr;
    g_running = false;
    g_phase = Phase::Disabled;
}

bool createInstance() {
    uint32_t n = 0;
    if (!XR_OK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) return false;
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!XR_OK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()))) return false;
    bool hasD3D11 = false;
    for (auto& e : exts) hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    if (!hasD3D11) {
        LOG("OpenXR runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "Red Faction Guerrilla VR");
    strcpy_s(ici.applicationInfo.engineName, "rfg-vr");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    if (!XR_OK(xrCreateInstance(&ici, &g_instance))) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_OK(xrGetInstanceProperties(g_instance, &ip)))
        LOG("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

// Ready: session up. Retry: no HMD yet, try again later. Disabled: give up.
Phase createSession(IDXGISwapChain* sc) {
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_instance, &sgi, &g_systemId);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        LOG("No HMD available yet (SteamVR running? headset awake?); retrying later");
        return Phase::Retry;
    }
    if (!check(r, "xrGetSystem")) return Phase::Disabled;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_OK(xrGetSystemProperties(g_instance, g_systemId, &sp)))
        LOG("OpenXR system: %s, max layers %u", sp.systemName, sp.graphicsProperties.maxLayerCount);
    {
        uint32_t nv = 0;
        XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (XR_OK(xrEnumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    2, &nv, vcv)) &&
            nv == 2)
            for (int i = 0; i < 2; ++i) {
                g_recW[i] = vcv[i].recommendedImageRectWidth;
                g_recH[i] = vcv[i].recommendedImageRectHeight;
            }
        LOG("Recommended eye size %ux%u / %ux%u", g_recW[0], g_recH[0], g_recW[1], g_recH[1]);
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    if (!XR_OK(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getReqs))))
        return Phase::Disabled;
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!XR_OK(getReqs(g_instance, g_systemId, &reqs))) return Phase::Disabled;

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) {
        LOG("swapchain GetDevice failed");
        return Phase::Disabled;
    }
    g_device->GetImmediateContext(&g_ctx);
    {
        IDXGIDevice* dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd))) &&
            SUCCEEDED(dd->GetAdapter(&ad))) {
            ad->GetDesc(&d);
            ad->Release();
        }
        if (dd) dd->Release();
        LOG("Game adapter LUID %08lX:%08lX, runtime wants %08lX:%08lX; min feature level 0x%X, game device 0x%X",
            d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, reqs.adapterLuid.HighPart, reqs.adapterLuid.LowPart,
            reqs.minFeatureLevel, g_device->GetFeatureLevel());
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_systemId;
    if (!XR_OK(xrCreateSession(g_instance, &sci, &g_session))) return Phase::Disabled;

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    if (!XR_OK(xrCreateReferenceSpace(g_session, &rsci, &g_space))) return Phase::Disabled;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!XR_OK(xrCreateReferenceSpace(g_session, &rsci, &g_viewSpace))) return Phase::Disabled;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (!XR_OK(xrCreateReferenceSpace(g_session, &rsci, &g_stageSpace))) g_stageSpace = XR_NULL_HANDLE;
    controllers::create(g_instance, g_session);

    uint32_t n = 0;
    if (!XR_OK(xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr))) return Phase::Disabled;
    g_formats.resize(n);
    if (!XR_OK(xrEnumerateSwapchainFormats(g_session, n, &n, g_formats.data()))) return Phase::Disabled;
    std::string list;
    for (auto f : g_formats) list += std::to_string(f) + ' ';
    LOG("OpenXR session created; swapchain formats: %s", list.c_str());
    return Phase::Ready;
}

bool ensureSwap(Swap& s, const D3D11_TEXTURE2D_DESC& bb, const char* name) {
    bool sameKey = s.w == bb.Width && s.h == bb.Height && s.srcFormat == bb.Format;
    if (sameKey && (s.handle != XR_NULL_HANDLE || s.failed)) return s.handle != XR_NULL_HANDLE;
    destroySwap(s);
    s.w = bb.Width;
    s.h = bb.Height;
    s.srcFormat = bb.Format;
    s.failed = true;  // cleared on success

    // Copying needs a format from the source's typeless family.
    std::vector<DXGI_FORMAT> candidates{bb.Format};
    switch (bb.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: candidates.push_back(DXGI_FORMAT_R8G8B8A8_UNORM); break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: candidates.push_back(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: candidates.push_back(DXGI_FORMAT_B8G8R8A8_UNORM); break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: candidates.push_back(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB); break;
        default: break;
    }
    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    for (auto c : candidates) {
        for (auto f : g_formats)
            if (f == c) { chosen = c; break; }
        if (chosen != DXGI_FORMAT_UNKNOWN) break;
    }
    if (chosen == DXGI_FORMAT_UNKNOWN) {
        LOG("%s: no runtime swapchain format is copy-compatible with backbuffer format %d", name, bb.Format);
        return false;
    }

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = chosen;
    ci.sampleCount = 1;
    ci.width = bb.Width;
    ci.height = bb.Height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!XR_OK(xrCreateSwapchain(g_session, &ci, &s.handle))) return false;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!XR_OK(xrEnumerateSwapchainImages(s.handle, n, &n,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())))) {
        xrDestroySwapchain(s.handle);
        s.handle = XR_NULL_HANDLE;
        return false;
    }
    for (auto& i : imgs) s.images.push_back(i.texture);
    s.format = chosen;
    s.failed = false;
    LOG("%s swapchain %ux%u fmt %d (backbuffer fmt %d, %u samples), %u images", name, bb.Width, bb.Height, chosen,
        bb.Format, bb.SampleDesc.Count, n);
    return true;
}

// Acquire an image, copy `bb` into it, release. False if the image could not be filled.
bool copyInto(Swap& s, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_OK(xrAcquireSwapchainImage(s.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;  // 100 ms
    XrResult wr = xrWaitSwapchainImage(s.handle, &wi);
    bool ok = wr == XR_SUCCESS;
    if (ok) {
        ID3D11Texture2D* dst = s.images[idx];
        if (bd.SampleDesc.Count > 1) g_ctx->ResolveSubresource(dst, 0, bb, 0, s.format);
        else g_ctx->CopyResource(dst, bb);
    } else {
        check(wr, "xrWaitSwapchainImage");
        if (wr == XR_TIMEOUT_EXPIRED) LOG("xrWaitSwapchainImage timed out");
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_OK(xrReleaseSwapchainImage(s.handle, &ri));
    return ok;
}

void pollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (g_phase == Phase::Ready && xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                g_state = reinterpret_cast<XrEventDataSessionStateChanged&>(ev).state;
                LOG("OpenXR session state -> %d", g_state);
                if (g_state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    g_running = XR_OK(xrBeginSession(g_session, &bi));
                } else if (g_state == XR_SESSION_STATE_STOPPING) {
                    if (g_frameOpen) {  // close the pending frame before ending the session
                        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
                        fe.displayTime = g_frameState.predictedDisplayTime;
                        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                        xrEndFrame(g_session, &fe);
                        g_frameOpen = false;
                    }
                    XR_OK(xrEndSession(g_session));
                    g_running = false;
                } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                    teardown("session exiting / loss pending");
                    return;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                teardown("instance loss pending");
                return;
            default:
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Wait for and begin the next headset frame, then locate the views both eyes will use.
void openFrame() {
    g_frameState = {XR_TYPE_FRAME_STATE};
    if (!XR_OK(xrWaitFrame(g_session, nullptr, &g_frameState))) { teardown("xrWaitFrame failed"); return; }
    if (!XR_OK(xrBeginFrame(g_session, nullptr))) { teardown("xrBeginFrame failed"); return; }
    g_frameOpen = true;
    g_presentsInFrame = 0;
    if (g_frameState.shouldRender) controllers::sync(g_session, g_space, g_frameState.predictedDisplayTime);
    g_eyes[0].have = g_eyes[1].have = false;

    std::scoped_lock lock(g_viewMutex);
    ++g_viewSet;
    g_requests = 0;
    g_viewsValid = false;
    if (!g_frameState.shouldRender) return;
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    g_views[0] = {XR_TYPE_VIEW};
    g_views[1] = {XR_TYPE_VIEW};
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_OK(xrLocateViews(g_session, &vli, &vs, 2, &n, g_views)) && n == 2 && (vs.viewStateFlags & kValid) == kValid)
        g_viewsValid = true;
    g_haveFloor = false;
    if (g_stageSpace != XR_NULL_HANDLE) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_OK(xrLocateSpace(g_space, g_stageSpace, g_frameState.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            g_localHeight = loc.pose.position.y;
            g_haveFloor = true;
        }
    }
    if (g_viewsValid && !g_haveFov) {
        g_eyeFov[0] = g_views[0].fov;
        g_eyeFov[1] = g_views[1].fov;
        g_haveFov = true;
    }
}

enum class Submit { Nothing, Stereo, Mono, Screen };

// The reticle image: a white ring with a dark outline and a centre dot, premultiplied alpha, in the
// backbuffer's format (copied into its swapchain every frame).
bool reticle(DXGI_FORMAT format) {
    constexpr UINT kSize = 64;
    if (g_reticleTex) {
        D3D11_TEXTURE2D_DESC d{};
        g_reticleTex->GetDesc(&d);
        if (d.Format != format) {
            g_reticleTex->Release();
            g_reticleTex = nullptr;
        }
    }
    if (!g_reticleTex) {
        std::vector<uint32_t> px(kSize * kSize);
        for (UINT y = 0; y < kSize; ++y)
            for (UINT x = 0; x < kSize; ++x) {
                float dx = x + 0.5f - kSize / 2.f, dy = y + 0.5f - kSize / 2.f;
                float r = std::sqrt(dx * dx + dy * dy);
                uint32_t a = 0, c = 0;
                if ((r > 18.f && r < 24.f) || r < 3.f) a = 255, c = 255;         // ring, dot
                else if ((r > 16.f && r < 26.f) || r < 5.f) a = 160, c = 0;     // outline
                px[y * kSize + x] = c | c << 8 | c << 16 | a << 24;               // RGBA or BGRA: grey
            }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kSize;
        d.MipLevels = d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px.data(), kSize * 4, 0};
        if (FAILED(g_device->CreateTexture2D(&d, &init, &g_reticleTex))) return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    g_reticleTex->GetDesc(&d);
    return ensureSwap(g_reticle, d, "reticle") && copyInto(g_reticle, g_reticleTex, d);
}

// The virtual screen is placed where the head looks when it appears (level, at head height) and
// stays there while it is shown.
bool g_screenPlaced = false;
XrPosef g_screenPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, -kScreenDistance}};

void placeScreen() {
    if (!g_viewsValid) return;
    const XrQuaternionf& q = g_views[0].pose.orientation;
    // Forward (-Z) rotated by the head orientation, projected onto the floor plane.
    float fx = -2.f * (q.x * q.z + q.w * q.y), fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    float yaw = std::atan2(-fx, -fz);  // rotation about +Y
    XrVector3f head{(g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f,
                    (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f,
                    (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f};
    g_screenPose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
    g_screenPose.position = {head.x - std::sin(yaw) * kScreenDistance, head.y, head.z - std::cos(yaw) * kScreenDistance};
    g_screenPlaced = true;
}

void endFrame(Submit what, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd) {
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[3] = {};
    XrCompositionLayerQuad reticleQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    uint32_t layerCount = 0;

    if (what == Submit::Stereo || what == Submit::Mono) {
        const Eye& only = g_eyes[0].have ? g_eyes[0] : g_eyes[1];
        for (int i = 0; i < 2; ++i) {
            const Eye& src = what == Submit::Stereo ? g_eyes[i] : only;
            pv[i].pose = src.pose;
            pv[i].fov = src.fov;
            pv[i].subImage.swapchain = src.swap.handle;
            pv[i].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(src.swap.w), static_cast<int32_t>(src.swap.h)}};
            pv[i].subImage.imageArrayIndex = 0;
        }
        proj.space = g_space;
        proj.viewCount = 2;
        proj.views = pv;
        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
        layerCount = 1;
        if (what == Submit::Stereo && ++g_stereoFrames == 1) LOG("First stereo frame submitted");

        // UI panel centred on the aim direction: ahead of the head with head aim, else ahead in the room.
        if (ID3D11Texture2D* ui = hud::latest()) {
            D3D11_TEXTURE2D_DESC ud{};
            ui->GetDesc(&ud);
            if (ensureSwap(g_hud, ud, "HUD") && copyInto(g_hud, ui, ud)) {
                float d = config::hudDistance(), w = config::hudWidth();
                hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudQuad.space = config::headAim() ? g_viewSpace : g_space;
                hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudQuad.subImage.swapchain = g_hud.handle;
                hudQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_hud.w), static_cast<int32_t>(g_hud.h)}};
                hudQuad.pose.orientation = {0.f, 0.f, 0.f, 1.f};
                hudQuad.pose.position = {0.f, 0.f, -d};
                hudQuad.size = {w, w * static_cast<float>(g_hud.h) / static_cast<float>(g_hud.w)};
                layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&hudQuad);
            }
        }

        controllers::State cs;
        if (camera::handAimActive() && controllers::state(cs) && cs.hand[controllers::kRight].active &&
            reticle(bd.Format)) {
            const XrPosef& aim = cs.hand[controllers::kRight].aim;
            const XrQuaternionf& q = aim.orientation;
            // -Z of the aim pose
            XrVector3f dir{-2.f * (q.x * q.z + q.w * q.y), -2.f * (q.y * q.z - q.w * q.x),
                           -(1.f - 2.f * (q.x * q.x + q.y * q.y))};
            reticleQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            reticleQuad.space = g_space;
            reticleQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            reticleQuad.subImage.swapchain = g_reticle.handle;
            reticleQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_reticle.w), static_cast<int32_t>(g_reticle.h)}};
            reticleQuad.pose.orientation = q;
            reticleQuad.pose.position = {aim.position.x + dir.x * kReticleDistance,
                                         aim.position.y + dir.y * kReticleDistance,
                                         aim.position.z + dir.z * kReticleDistance};
            reticleQuad.size = {kReticleDistance * kReticleAngle, kReticleDistance * kReticleAngle};
            layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&reticleQuad);
        }
    } else if (what == Submit::Screen && bb && ensureSwap(g_mirror, bd, "mirror") && copyInto(g_mirror, bb, bd)) {
        quad.space = g_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_mirror.handle;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_mirror.w), static_cast<int32_t>(g_mirror.h)}};
        if (!g_screenPlaced) placeScreen();
        quad.pose = g_screenPose;
        quad.size = {kScreenWidth, kScreenWidth * static_cast<float>(g_mirror.h) / static_cast<float>(g_mirror.w)};
        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
        layerCount = 1;
    }

    if (what == Submit::Stereo || what == Submit::Mono) g_screenPlaced = false;

    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = g_frameState.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = layerCount;
    fe.layers = layers;
    g_frameOpen = false;
    if (!XR_OK(xrEndFrame(g_session, &fe))) { teardown("xrEndFrame failed"); return; }

    static const char* names[] = {"nothing", "stereo", "mono", "screen"};
    if (++g_xrFrames == 1 || g_xrFrames % 600 == 0)
        LOG("OpenXR frame %llu: %s, %u game frames, state %d, period %.2f ms, stereo frames %llu, stale images %llu",
            static_cast<unsigned long long>(g_xrFrames), names[static_cast<int>(what)], g_presentsInFrame, g_state,
            g_frameState.predictedDisplayPeriod / 1e6, static_cast<unsigned long long>(g_stereoFrames),
            static_cast<unsigned long long>(g_staleImages));
}

// Every Present, before the original (the backbuffer holds the finished frame).
void frame(IDXGISwapChain* sc) {
    if (!g_frameOpen) {
        openFrame();
        return;
    }
    ++g_presentsInFrame;

    ID3D11Texture2D* bb = nullptr;
    D3D11_TEXTURE2D_DESC bd{};
    if (g_frameState.shouldRender &&
        SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))))
        bb->GetDesc(&bd);

    bool image = g_eyeRendered && gamestate::gameplay();  // outside gameplay: always the virtual screen
    g_eyeRendered = false;
    if (image) {
        g_idlePresents = 0;
        if (g_renderedSet == g_viewSet && bb) {
            int eye = g_renderedEye;
            Eye& e = g_eyes[eye];
            if (ensureSwap(e.swap, bd, eye ? "right eye" : "left eye") && copyInto(e.swap, bb, bd)) {
                e.have = true;
                e.pose = g_renderedPose;
                e.fov = g_renderedFov;
            }
        } else {
            ++g_staleImages;  // rendered for an earlier headset frame
        }
    } else {
        ++g_idlePresents;
    }

    bool complete = g_eyes[0].have && g_eyes[1].have;
    bool menu = g_idlePresents >= 2;
    bool timeout = g_presentsInFrame >= 4;  // an eye went missing: don't stall the headset
    if (!g_frameState.shouldRender || complete || menu || timeout) {
        Submit what = !g_frameState.shouldRender ? Submit::Nothing
                      : complete                 ? Submit::Stereo
                      : menu                     ? Submit::Screen
                      : (g_eyes[0].have || g_eyes[1].have) ? Submit::Mono
                                                            : Submit::Nothing;
        endFrame(what, bb, bd);
        if (g_phase == Phase::Ready) openFrame();
    }
    if (bb) bb->Release();
}

}  // namespace

bool renderPose(RenderPose& out) {
    if (g_phase != Phase::Ready || !g_running || !gamestate::gameplay()) return false;
    std::scoped_lock lock(g_viewMutex);
    if (!g_frameOpen || !g_viewsValid) return false;
    int eye = static_cast<int>(g_requests++ & 1);  // left, then right
    out.eye = eye;
    out.set = g_viewSet;
    out.pose = g_views[eye].pose;
    out.fov = g_views[eye].fov;
    out.haveFloor = g_haveFloor;
    out.localHeight = g_localHeight;
    return true;
}

bool headOrientation(XrQuaternionf& out) {
    if (g_phase != Phase::Ready || !g_running) return false;
    std::scoped_lock lock(g_viewMutex);
    if (!g_frameOpen || !g_viewsValid) return false;
    out = g_views[0].pose.orientation;  // both eyes share the head's orientation
    return true;
}

bool headPosition(XrVector3f& out) {
    if (g_phase != Phase::Ready || !g_running) return false;
    std::scoped_lock lock(g_viewMutex);
    if (!g_frameOpen || !g_viewsValid) return false;
    const XrVector3f &a = g_views[0].pose.position, &b = g_views[1].pose.position;
    out = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    return true;
}

bool localHeight(float& out) {
    std::scoped_lock lock(g_viewMutex);
    if (!g_haveFloor) return false;
    out = g_localHeight;
    return true;
}

bool stereoActive() {  // shouldRender is false while the headset is off or shows something else
    return g_phase == Phase::Ready && g_running && g_frameOpen && g_frameState.shouldRender;
}

bool hudCaptureWanted() { return stereoActive() && g_idlePresents < 2 && gamestate::gameplay(); }

bool eyeRenderSize(uint32_t& w, uint32_t& h) {
    if (!g_haveFov || !g_recW[0] || !g_recH[0]) return false;
    // Symmetric frustum covering both eyes' fov at SteamVR's pixel density. (An exact off-centre
    // projection per eye breaks the engine's fog: the terrain turns orange.)
    float tanH = 0, tanV = 0, pptH = 0, pptV = 0;
    for (int i = 0; i < 2; ++i) {
        const XrFovf& f = g_eyeFov[i];
        float l = std::tan(f.angleLeft), r = std::tan(f.angleRight);
        float d = std::tan(f.angleDown), u = std::tan(f.angleUp);
        tanH = std::max(tanH, std::max(std::fabs(l), std::fabs(r)));
        tanV = std::max(tanV, std::max(std::fabs(d), std::fabs(u)));
        pptH = std::max(pptH, g_recW[i] / (r - l));
        pptV = std::max(pptV, g_recH[i] / (u - d));
    }
    constexpr float kMargin = 1.05f;  // must match the camera's covering fov margin
    w = std::min(4096u, static_cast<uint32_t>(pptH * 2.f * tanH * kMargin) & ~1u);
    h = std::min(4096u, static_cast<uint32_t>(pptV * 2.f * tanV * kMargin) & ~1u);
    return w >= 64 && h >= 64;
}

void markEyeRendered(int eye, uint32_t set, const XrPosef& pose, const XrFovf& fov) {
    g_renderedEye = eye;
    g_renderedSet = set;
    g_renderedPose = pose;
    g_renderedFov = fov;
    g_eyeRendered = true;
}

void onPresent(IDXGISwapChain* sc) {
    ++g_presentCount;
    if (g_phase == Phase::Disabled) return;

    if (g_phase == Phase::Uninit) {
        if (disabledByFile()) {
            LOG("rfg-vr-novr.txt found next to the dll: VR disabled for this run");
            g_phase = Phase::Disabled;
            return;
        }
        if (!createInstance()) { teardown("instance creation failed"); return; }
        g_phase = Phase::Retry;
        g_retryAt = 0;
    }
    if (g_phase == Phase::Retry) {
        if (g_presentCount < g_retryAt) return;
        g_phase = createSession(sc);
        if (g_phase == Phase::Retry) { g_retryAt = g_presentCount + kRetryPresents; return; }
        if (g_phase == Phase::Disabled) { teardown("session creation failed"); return; }
    }

    pollEvents();
    if (g_phase != Phase::Ready || !g_running) return;
    frame(sc);
}

}  // namespace rfgvr::xr
