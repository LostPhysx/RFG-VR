// xr-probe: validates the SteamVR 32-bit OpenXR path with D3D11 before we touch rfg.exe.
// Logs runtime/system info, address-space use and frame timing; renders a ray-traced
// floor grid + reference spheres + controller spheres so stereo and 6-DOF can be judged in-headset.
// Usage: xr-probe.exe [seconds=60]   Output: console + xr-probe.log
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static XrInstance g_instance = XR_NULL_HANDLE;

static void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

static void logMem(const char* tag) {
    MEMORYSTATUSEX m{sizeof(m)};
    GlobalMemoryStatusEx(&m);
    log("[mem] %-34s VA used %7.1f MB / %7.1f MB", tag,
        (m.ullTotalVirtual - m.ullAvailVirtual) / 1048576.0, m.ullTotalVirtual / 1048576.0);
}

static std::string xrStr(XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf))) return buf;
    return std::to_string(static_cast<int>(r));
}

#define XR_CHECK(x)                                                                  \
    do {                                                                             \
        XrResult r_ = (x);                                                           \
        if (XR_FAILED(r_)) {                                                         \
            log("FATAL %s -> %s (line %d)", #x, xrStr(r_).c_str(), __LINE__);        \
            return 1;                                                                \
        }                                                                            \
    } while (0)

#define HR_CHECK(x)                                                                  \
    do {                                                                             \
        HRESULT h_ = (x);                                                            \
        if (FAILED(h_)) {                                                            \
            log("FATAL %s -> HRESULT 0x%08lX (line %d)", #x, (unsigned long)h_, __LINE__); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

static const char* kShader = R"(
cbuffer CB : register(b0) {
    float4 tanLRDU;   // tan of fov angles: left, right, down, up
    float4 q;         // eye orientation quaternion (xyzw), OpenXR space
    float4 p;         // eye position
    float4 hand[2];   // xyz = position, w = radius (0 = not tracked)
    float4 misc;      // x = eye index
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 qrot(float4 r, float3 v) { return v + 2.0 * cross(r.xyz, cross(r.xyz, v) + r.w * v); }
float hitSphere(float3 ro, float3 rd, float3 c, float r) {
    float3 oc = ro - c; float b = dot(oc, rd); float h = b * b - dot(oc, oc) + r * r;
    return h < 0 ? -1 : -b - sqrt(h);
}
float shade(float3 n) { return 0.3 + 0.7 * saturate(dot(n, normalize(float3(0.3, 1, 0.2)))); }
float4 ps(VSOut i) : SV_Target {
    float3 rd = normalize(qrot(q, float3(lerp(tanLRDU.x, tanLRDU.y, i.uv.x), lerp(tanLRDU.w, tanLRDU.z, i.uv.y), -1)));
    float3 ro = p.xyz;
    float3 col = lerp(float3(0.55, 0.35, 0.25), float3(0.12, 0.16, 0.3), saturate(rd.y * 2 + 0.3));
    float best = 1e9;
    if (rd.y < -1e-4) {                       // floor y = 0 (stage space): 1 m checkerboard
        float t = -ro.y / rd.y; float3 h = ro + rd * t;
        float2 c = floor(h.xz);
        float3 g = (fmod(abs(c.x + c.y), 2) > 0.5) ? float3(0.6, 0.3, 0.2) : float3(0.35, 0.15, 0.1);
        float2 f = abs(frac(h.xz) - 0.5);
        if (max(f.x, f.y) > 0.485) g = float3(0.9, 0.9, 0.9);
        if (length(h.xz) < 0.1) g = float3(0, 1, 0);   // origin marker
        col = g * exp(-t * 0.03); best = t;
    }
    for (int j = 0; j < 8; j++) {             // ring of spheres, radius 2 m, eye height
        float a = j * 0.785398; float3 c = float3(sin(a) * 2, 1.5, -cos(a) * 2);
        float t = hitSphere(ro, rd, c, 0.15);
        if (t > 0 && t < best) { best = t; col = (j == 0 ? float3(0.2, 1, 0.2) : float3(0.9, 0.9, 0.3)) * shade(normalize(ro + rd * t - c)); }
    }
    for (int k = 0; k < 2; k++) {             // controllers: left blue, right red
        if (hand[k].w <= 0) continue;
        float t = hitSphere(ro, rd, hand[k].xyz, hand[k].w);
        if (t > 0 && t < best) { best = t; col = (k == 0 ? float3(0.2, 0.6, 1) : float3(1, 0.3, 0.3)) * shade(normalize(ro + rd * t - hand[k].xyz)); }
    }
    return float4(col, 1);
}
)";

struct CB {
    float tanLRDU[4], q[4], p[4], hand[2][4], misc[4];
};

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t w = 0, h = 0;
    std::vector<ID3D11RenderTargetView*> rtvs;
};

static XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

int main(int argc, char** argv) {
    const double runSeconds = argc > 1 ? atof(argv[1]) : 60.0;
    g_log = fopen("xr-probe.log", "w");
    log("xr-probe: pointer size %zu bytes (%s), run %.0f s", sizeof(void*), sizeof(void*) == 4 ? "x86" : "x64", runSeconds);
    logMem("startup");

    // --- Runtime discovery -------------------------------------------------------------
    uint32_t n = 0;
    xrEnumerateApiLayerProperties(0, &n, nullptr);
    std::vector<XrApiLayerProperties> layers(n, {XR_TYPE_API_LAYER_PROPERTIES});
    xrEnumerateApiLayerProperties(n, &n, layers.data());
    log("API layers: %u", n);
    for (auto& l : layers) log("  layer %s", l.layerName);

    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
    bool hasD3D11 = false;
    log("Instance extensions: %u", n);
    for (auto& e : exts) {
        log("  %s v%u", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
    }
    if (!hasD3D11) { log("FATAL: runtime lacks XR_KHR_D3D11_enable"); return 1; }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "rfg-vr xr-probe");
    strcpy_s(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    XR_CHECK(xrCreateInstance(&ici, &g_instance));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(g_instance, &ip));
    log("Runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    logMem("after xrCreateInstance");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    for (int tries = 0;; ++tries) {
        XrResult r = xrGetSystem(g_instance, &sgi, &systemId);
        if (XR_SUCCEEDED(r)) break;
        if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE || tries > 20) { log("FATAL xrGetSystem -> %s", xrStr(r).c_str()); return 1; }
        log("HMD not available yet (is it on / SteamVR ready?), retrying...");
        Sleep(1000);
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XR_CHECK(xrGetSystemProperties(g_instance, systemId, &sp));
    log("System: %s vendor 0x%X, max swapchain %ux%u, layers %u, orientTracking %d posTracking %d",
        sp.systemName, sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
        sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount,
        sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking);

    XR_CHECK(xrEnumerateViewConfigurationViews(g_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
    std::vector<XrViewConfigurationView> vcv(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(g_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vcv.data()));
    for (uint32_t i = 0; i < n; ++i)
        log("View %u: recommended %ux%u (max %ux%u), samples %u", i, vcv[i].recommendedImageRectWidth,
            vcv[i].recommendedImageRectHeight, vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight,
            vcv[i].recommendedSwapchainSampleCount);

    // --- D3D11 device on the adapter the runtime wants ----------------------------------
    PFN_xrGetD3D11GraphicsRequirementsKHR pGetReqs = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&pGetReqs)));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(pGetReqs(g_instance, systemId, &reqs));
    log("D3D11 requirements: min feature level 0x%X, adapter LUID %08lX:%08lX", reqs.minFeatureLevel,
        reqs.adapterLuid.HighPart, reqs.adapterLuid.LowPart);

    IDXGIFactory1* factory = nullptr;
    HR_CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)));
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!memcmp(&d.AdapterLuid, &reqs.adapterLuid, sizeof(LUID))) {
            log("Adapter: %ls (%zu MB VRAM)", d.Description, d.DedicatedVideoMemory / 1048576);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    if (!adapter) { log("FATAL: runtime adapter LUID not found"); return 1; }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{};
    HR_CHECK(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, 2, D3D11_SDK_VERSION, &device, &got, &ctx));
    log("D3D11 device created, feature level 0x%X", got);
    logMem("after D3D11 device");

    // --- Session --------------------------------------------------------------------------
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = systemId;
    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(g_instance, &sci, &session));
    logMem("after xrCreateSession");

    XR_CHECK(xrEnumerateReferenceSpaces(session, 0, &n, nullptr));
    std::vector<XrReferenceSpaceType> spaces(n);
    XR_CHECK(xrEnumerateReferenceSpaces(session, n, &n, spaces.data()));
    bool hasStage = false;
    for (auto s : spaces) hasStage |= (s == XR_REFERENCE_SPACE_TYPE_STAGE);
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1;
    XrSpace appSpace = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &appSpace));
    log("Reference space: %s", hasStage ? "STAGE" : "LOCAL (no STAGE; floor will be at head height)");

    // --- Actions (Index controllers + generic fallback) ------------------------------------
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asci.actionSetName, "probe");
    strcpy_s(asci.localizedActionSetName, "Probe");
    XrActionSet actionSet = XR_NULL_HANDLE;
    XR_CHECK(xrCreateActionSet(g_instance, &asci, &actionSet));
    XrPath hands[2] = {path("/user/hand/left"), path("/user/hand/right")};
    auto makeAction = [&](XrActionType type, const char* name, XrAction* out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, name);
        aci.countSubactionPaths = 2;
        aci.subactionPaths = hands;
        return xrCreateAction(actionSet, &aci, out);
    };
    XrAction poseAction, triggerAction, hapticAction;
    XR_CHECK(makeAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", &poseAction));
    XR_CHECK(makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", &triggerAction));
    XR_CHECK(makeAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", &hapticAction));
    auto suggest = [&](const char* profile, const char* triggerSuffix) {
        std::vector<XrActionSuggestedBinding> b;
        for (const char* side : {"left", "right"}) {
            std::string base = std::string("/user/hand/") + side;
            b.push_back({poseAction, path((base + "/input/grip/pose").c_str())});
            b.push_back({triggerAction, path((base + triggerSuffix).c_str())});
            b.push_back({hapticAction, path((base + "/output/haptic").c_str())});
        }
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = path(profile);
        s.countSuggestedBindings = static_cast<uint32_t>(b.size());
        s.suggestedBindings = b.data();
        XrResult r = xrSuggestInteractionProfileBindings(g_instance, &s);
        log("Suggest bindings %s -> %s", profile, xrStr(r).c_str());
    };
    suggest("/interaction_profiles/valve/index_controller", "/input/trigger/value");
    suggest("/interaction_profiles/khr/simple_controller", "/input/select/click");
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(session, &attach));
    XrSpace handSpace[2];
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo a{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        a.action = poseAction;
        a.subactionPath = hands[i];
        a.poseInActionSpace.orientation.w = 1;
        XR_CHECK(xrCreateActionSpace(session, &a, &handSpace[i]));
    }

    // --- Swapchains --------------------------------------------------------------------------
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &n, nullptr));
    std::vector<int64_t> formats(n);
    XR_CHECK(xrEnumerateSwapchainFormats(session, n, &n, formats.data()));
    std::string fmtList;
    for (auto f : formats) fmtList += std::to_string(f) + " ";
    log("Swapchain formats (DXGI, runtime order): %s", fmtList.c_str());
    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT want : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                             DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        for (auto f : formats)
            if (f == want) { chosen = want; break; }
        if (chosen != DXGI_FORMAT_UNKNOWN) break;
    }
    if (chosen == DXGI_FORMAT_UNKNOWN) { log("FATAL: no usable swapchain format"); return 1; }
    log("Chosen swapchain format: %d", chosen);

    EyeSwapchain eyes[2];
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = chosen;
        ci.sampleCount = 1;
        ci.width = vcv[e].recommendedImageRectWidth;
        ci.height = vcv[e].recommendedImageRectHeight;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(session, &ci, &eyes[e].handle));
        eyes[e].w = ci.width;
        eyes[e].h = ci.height;
        XR_CHECK(xrEnumerateSwapchainImages(eyes[e].handle, 0, &n, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(eyes[e].handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
        D3D11_TEXTURE2D_DESC td;
        imgs[0].texture->GetDesc(&td);
        log("Eye %d swapchain: %ux%u, %u images, texture format %d, bind 0x%X, misc 0x%X", e, td.Width,
            td.Height, n, td.Format, td.BindFlags, td.MiscFlags);
        for (auto& img : imgs) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = chosen;  // images may be typeless; the view must name the format
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            HR_CHECK(device->CreateRenderTargetView(img.texture, &rd, &rtv));
            eyes[e].rtvs.push_back(rtv);
        }
    }
    logMem("after swapchains");

    // --- Pipeline ------------------------------------------------------------------------------
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "probe", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "probe", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        log("FATAL shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return 1;
    }
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    HR_CHECK(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs));
    HR_CHECK(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cb = nullptr;
    HR_CHECK(device->CreateBuffer(&bd, nullptr, &cb));

    // --- Frame loop ----------------------------------------------------------------------------
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const auto start = clock::now();
    auto statStart = start;
    double tWait = 0, tBegin = 0, tRender = 0, tEnd = 0;
    int frames = 0, rendered = 0, totalFrames = 0;
    bool running = false, quit = false, exitRequested = false;
    bool triggerWasDown[2] = {};
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;

    while (!quit) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged&>(ev).state;
                log("Session state -> %d  (t=%.1fs)", state, ms(clock::now() - start) / 1000);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xrBeginSession(session, &bi));
                    running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(xrEndSession(session));
                    running = false;
                    quit = true;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    running = false;
                    quit = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                for (int i = 0; i < 2; ++i) {
                    XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
                    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, hands[i], &ps)) && ps.interactionProfile) {
                        char buf[XR_MAX_PATH_LENGTH];
                        uint32_t len;
                        xrPathToString(g_instance, ps.interactionProfile, sizeof(buf), &len, buf);
                        log("Hand %d interaction profile: %s", i, buf);
                    }
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                log("Instance loss pending");
                quit = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!exitRequested && ms(clock::now() - start) / 1000 > runSeconds) {
            log("Run time reached, requesting exit");
            exitRequested = true;
            if (running) xrRequestExitSession(session); else quit = true;
        }
        if (!running) { Sleep(10); continue; }

        auto t0 = clock::now();
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, nullptr, &fs));
        auto t1 = clock::now();
        XR_CHECK(xrBeginFrame(session, nullptr));
        auto t2 = clock::now();

        XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        uint32_t layerCount = 0;

        if (fs.shouldRender) {
            CB c{};
            // Input
            XrActiveActionSet aas{actionSet, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &aas;
            xrSyncActions(session, &sync);
            for (int i = 0; i < 2; ++i) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = triggerAction;
                gi.subactionPath = hands[i];
                XrActionStateFloat trig{XR_TYPE_ACTION_STATE_FLOAT};
                xrGetActionStateFloat(session, &gi, &trig);
                XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
                xrLocateSpace(handSpace[i], appSpace, fs.predictedDisplayTime, &loc);
                if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                    c.hand[i][0] = loc.pose.position.x;
                    c.hand[i][1] = loc.pose.position.y;
                    c.hand[i][2] = loc.pose.position.z;
                    c.hand[i][3] = 0.04f + 0.06f * (trig.isActive ? trig.currentState : 0.f);
                }
                bool down = trig.isActive && trig.currentState > 0.9f;
                if (down && !triggerWasDown[i]) {
                    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
                    vib.duration = 100000000;  // 100 ms
                    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
                    vib.amplitude = 0.8f;
                    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                    hi.action = hapticAction;
                    hi.subactionPath = hands[i];
                    XrResult r = xrApplyHapticFeedback(session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
                    log("Hand %d trigger pulled -> haptic %s", i, xrStr(r).c_str());
                }
                triggerWasDown[i] = down;
            }
            // Views
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = appSpace;
            XrViewState vs2{XR_TYPE_VIEW_STATE};
            XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t viewCount = 0;
            XR_CHECK(xrLocateViews(session, &vli, &vs2, 2, &viewCount, views));
            if ((vs2.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && viewCount == 2) {
                for (int e = 0; e < 2; ++e) {
                    uint32_t idx = 0;
                    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XR_CHECK(xrAcquireSwapchainImage(eyes[e].handle, &ai, &idx));
                    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wi.timeout = XR_INFINITE_DURATION;
                    XR_CHECK(xrWaitSwapchainImage(eyes[e].handle, &wi));

                    const XrFovf& f = views[e].fov;
                    c.tanLRDU[0] = tanf(f.angleLeft);
                    c.tanLRDU[1] = tanf(f.angleRight);
                    c.tanLRDU[2] = tanf(f.angleDown);
                    c.tanLRDU[3] = tanf(f.angleUp);
                    const auto& o = views[e].pose.orientation;
                    const auto& p = views[e].pose.position;
                    c.q[0] = o.x; c.q[1] = o.y; c.q[2] = o.z; c.q[3] = o.w;
                    c.p[0] = p.x; c.p[1] = p.y; c.p[2] = p.z;
                    c.misc[0] = static_cast<float>(e);
                    ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);

                    D3D11_VIEWPORT vp{0, 0, static_cast<float>(eyes[e].w), static_cast<float>(eyes[e].h), 0, 1};
                    ctx->RSSetViewports(1, &vp);
                    ctx->OMSetRenderTargets(1, &eyes[e].rtvs[idx], nullptr);
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx->IASetInputLayout(nullptr);
                    ctx->VSSetShader(vs, nullptr, 0);
                    ctx->PSSetShader(ps, nullptr, 0);
                    ctx->PSSetConstantBuffers(0, 1, &cb);
                    ctx->Draw(3, 0);

                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    XR_CHECK(xrReleaseSwapchainImage(eyes[e].handle, &ri));

                    pv[e].pose = views[e].pose;
                    pv[e].fov = views[e].fov;
                    pv[e].subImage.swapchain = eyes[e].handle;
                    pv[e].subImage.imageRect = {{0, 0}, {eyes[e].w, eyes[e].h}};
                }
                layer.space = appSpace;
                layer.viewCount = 2;
                layer.views = pv;
                layerCount = 1;
                ++rendered;
            }
        }
        auto t3 = clock::now();
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = layerCount;
        fe.layers = layerCount ? layers : nullptr;
        XR_CHECK(xrEndFrame(session, &fe));
        auto t4 = clock::now();

        tWait += ms(t1 - t0); tBegin += ms(t2 - t1); tRender += ms(t3 - t2); tEnd += ms(t4 - t3);
        ++frames; ++totalFrames;
        if (ms(t4 - statStart) >= 5000) {
            double secs = ms(t4 - statStart) / 1000;
            log("[frame] %.1f fps (%d rendered) | avg ms: wait %.2f begin %.2f render %.2f end %.2f | period %.2f ms",
                frames / secs, rendered, tWait / frames, tBegin / frames, tRender / frames, tEnd / frames,
                fs.predictedDisplayPeriod / 1e6);
            logMem("steady state");
            frames = rendered = 0;
            tWait = tBegin = tRender = tEnd = 0;
            statStart = t4;
        }
    }

    log("Done: %d frames total. Shutting down.", totalFrames);
    for (auto& e : eyes) {
        for (auto* r : e.rtvs) r->Release();
        xrDestroySwapchain(e.handle);
    }
    xrDestroySpace(handSpace[0]);
    xrDestroySpace(handSpace[1]);
    xrDestroySpace(appSpace);
    xrDestroyActionSet(actionSet);
    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    cb->Release(); vs->Release(); ps->Release(); vsb->Release(); psb->Release();
    ctx->Release(); device->Release(); adapter->Release(); factory->Release();
    log("RESULT: OK");
    fclose(g_log);
    return 0;
}
