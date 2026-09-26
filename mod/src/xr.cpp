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

#include "config.h"
#include "gamestate.h"
#include "hud.h"
#include "log.h"

namespace rfgvr::xr {
namespace {

// Virtual screen placement in LOCAL space (SteamVR's seated origin; recenter via the dashboard).
constexpr float kScreenDistance = 2.5f;   // metres in front of the origin, at origin height
constexpr float kScreenWidth = 3.2f;      // metres; height follows the backbuffer aspect ratio
constexpr uint64_t kRetryPresents = 600;  // ~2.5-5 s between xrGetSystem retries while no HMD

enum class Phase { Uninit, Retry, Ready, Disabled };
Phase g_phase = Phase::Uninit;
uint64_t g_presentCount = 0;
uint64_t g_retryAt = 0;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_systemId = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;  // head-locked, for the HUD panel with head aim
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
std::vector<int64_t> g_formats;

ID3D11Device* g_device = nullptr;      // the game's device (AddRef'd via GetDevice)
ID3D11DeviceContext* g_ctx = nullptr;  // its immediate context (AddRef'd)

// A runtime swapchain sized like the game backbuffer so CopyResource works.
struct Swap {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;  // owned by the runtime; valid while handle lives
    uint32_t w = 0, h = 0;
    DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN;  // backbuffer format this swapchain was built for
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;     // swapchain format
    bool failed = false;                          // creation failed for this (w,h,srcFormat); don't retry
};
Swap g_mirror;  // quad layer (menus etc.)
Swap g_hud;     // quad layer over the stereo view: the captured in-game UI (premultiplied alpha)

struct Eye {
    Swap swap;
    bool have = false;  // holds this headset frame's image, rendered with `pose`/`fov`
    XrPosef pose{};
    XrFovf fov{};
};
Eye g_eyes[2];

// One headset frame spans two game frames: the game renders the left eye, then the right eye,
// both from the same located views (same head pose), and only then is the frame submitted.
bool g_frameOpen = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};
uint32_t g_presentsInFrame = 0;  // Presents since the headset frame was opened
uint32_t g_idlePresents = 0;     // consecutive Presents without a 3D eye image (menus, loading)

std::mutex g_viewMutex;  // guards the view set below: renderPose() runs on the game thread
XrView g_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_viewsValid = false;
uint32_t g_viewSet = 0;       // id of the open headset frame's views
uint32_t g_requests = 0;      // eye setups handed out for the current view set

// Image in the backbuffer at this Present, reported by the camera hook just before Present
// (matched by camera position, so the game's thread latency does not matter).
bool g_eyeRendered = false;
int g_renderedEye = 0;
uint32_t g_renderedSet = 0;
XrPosef g_renderedPose{};
XrFovf g_renderedFov{};
uint64_t g_staleImages = 0;

// Per-eye render size: SteamVR's recommended size (pixel density) and the eye fov (last located).
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
    // <dir of this dll>\rfg-vr-novr.txt switches the VR path off without removing the proxy.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&disabledByFile), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"rfg-vr-novr.txt";
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void destroySwap(Swap& s) {
    if (s.handle != XR_NULL_HANDLE) xrDestroySwapchain(s.handle);
    s = {};
}

void teardown(const char* why) {
    LOG("OpenXR teardown: %s", why);
    destroySwap(g_mirror);
    destroySwap(g_hud);
    for (auto& e : g_eyes) {
        destroySwap(e.swap);
        e.have = false;
    }
    g_frameOpen = false;
    if (g_viewSpace != XR_NULL_HANDLE) xrDestroySpace(g_viewSpace);
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
        if (XR_OK(xrEnumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv)) && nv == 2)
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

    // CopyResource / ResolveSubresource need a format from the backbuffer's typeless family.
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

// Acquire an image, copy the backbuffer into it, release. False if the image could not be filled.
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
    if (XR_OK(xrLocateViews(g_session, &vli, &vs, 2, &n, g_views)) && n == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
        g_viewsValid = true;
    if (g_viewsValid && !g_haveFov) {
        g_eyeFov[0] = g_views[0].fov;
        g_eyeFov[1] = g_views[1].fov;
        g_haveFov = true;
    }
}

enum class Submit { Nothing, Stereo, Mono, Screen };

void endFrame(Submit what, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd) {
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2] = {};
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
        if (what == Submit::Stereo && ++g_stereoFrames == 1) LOG("First stereo frame submitted (both eyes, same head pose)");

        // In-game UI on a transparent panel centred on the aim direction: straight ahead of the head
        // with head aim, else straight ahead in the room (the game camera's forward).
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
    } else if (what == Submit::Screen && bb && ensureSwap(g_mirror, bd, "mirror") && copyInto(g_mirror, bb, bd)) {
        quad.space = g_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_mirror.handle;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_mirror.w), static_cast<int32_t>(g_mirror.h)}};
        quad.pose.orientation = {0.f, 0.f, 0.f, 1.f};
        quad.pose.position = {0.f, 0.f, -kScreenDistance};
        quad.size = {kScreenWidth, kScreenWidth * static_cast<float>(g_mirror.h) / static_cast<float>(g_mirror.w)};
        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
        layerCount = 1;
    }

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

// Called at every game Present (before the original, so the backbuffer holds the finished frame).
void frame(IDXGISwapChain* sc) {
    if (!g_frameOpen) {
        openFrame();
        return;
    }
    ++g_presentsInFrame;

    ID3D11Texture2D* bb = nullptr;
    D3D11_TEXTURE2D_DESC bd{};
    if (g_frameState.shouldRender && SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))))
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
            ++g_staleImages;  // set up for an earlier headset frame; its pose no longer matches
        }
    } else {
        ++g_idlePresents;
    }

    bool complete = g_eyes[0].have && g_eyes[1].have;
    bool menu = g_idlePresents >= 2;
    bool timeout = g_presentsInFrame >= 4;  // an eye went missing: do not stall the headset
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
    // Outside gameplay (menus over the world, loading) the game renders its own flat view, shown on
    // the virtual screen.
    if (g_phase != Phase::Ready || !g_running || !gamestate::gameplay()) return false;
    std::scoped_lock lock(g_viewMutex);
    if (!g_frameOpen || !g_viewsValid) return false;
    int eye = static_cast<int>(g_requests++ & 1);  // first setup of a headset frame: left, then right
    out.eye = eye;
    out.set = g_viewSet;
    out.pose = g_views[eye].pose;
    out.fov = g_views[eye].fov;
    return true;
}

bool headOrientation(XrQuaternionf& out) {
    if (g_phase != Phase::Ready || !g_running) return false;
    std::scoped_lock lock(g_viewMutex);
    if (!g_frameOpen || !g_viewsValid) return false;
    out = g_views[0].pose.orientation;  // both eyes share the head's orientation
    return true;
}

bool stereoActive() {
    // shouldRender is false while the headset is not worn / the app is not visible in it.
    return g_phase == Phase::Ready && g_running && g_frameOpen && g_frameState.shouldRender;
}

bool hudCaptureWanted() { return stereoActive() && g_idlePresents < 2 && gamestate::gameplay(); }

bool eyeRenderSize(uint32_t& w, uint32_t& h) {
    if (!g_haveFov || !g_recW[0] || !g_recH[0]) return false;
    // Symmetric frustum that covers both eyes' asymmetric fov (what the engine renders), at the
    // pixel density SteamVR recommends for the asymmetric one. (An off-centre projection would save
    // ~25% of the pixels, but the engine's fog/atmosphere then renders wrong: the terrain turns orange.)
    float tanH = 0, tanV = 0, pptH = 0, pptV = 0;
    for (int i = 0; i < 2; ++i) {
        const XrFovf& f = g_eyeFov[i];
        float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), d = std::tan(f.angleDown), u = std::tan(f.angleUp);
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

void markEyeRendered(int eye, uint32_t set, const XrPosef& pose, const XrFovf& renderedFov) {
    g_renderedEye = eye;
    g_renderedSet = set;
    g_renderedPose = pose;
    g_renderedFov = renderedFov;
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
