#include "hud.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <safetyhook.hpp>

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "config.h"
#include "log.h"
#include "xr.h"

namespace rfgvr::hud {
namespace {

// Steam rfg.exe: int __thiscall FUN_00550830(queue, renderer), called only from the main view render
// FUN_007cf730 on the Present thread. It walks the frame's 2D primitive queue; every UI quad is
// drawn through FUN_00543b60 -> FUN_0052d690 onto the backbuffer (research/00-local-findings.md).
constexpr uintptr_t kUiPassVa = 0x550830;

SafetyHookInline g_uiPass, g_omSetRT, g_omSetBlend;

DWORD g_presentThread = 0;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Resource* g_backbuffer = nullptr;  // current backbuffer (not AddRef'd; compared only)

ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
D3D11_TEXTURE2D_DESC g_texDesc{};

bool g_capturing = false;
bool g_redirected = false;  // the game asked for the backbuffer and got the HUD target instead
ID3D11RenderTargetView* g_requestedRtv = nullptr;  // the backbuffer view the game asked for
ID3D11DepthStencilView* g_requestedDsv = nullptr;
ID3D11BlendState* g_requestedBlend = nullptr;
FLOAT g_requestedFactor[4] = {};
UINT g_requestedMask = 0xFFFFFFFF;
uint64_t g_presents = 0, g_capturedAt = 0;
std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_blendClones;

bool isBackbuffer(ID3D11RenderTargetView* v) {
    if (!v || !g_backbuffer) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (r) r->Release();
    return r == g_backbuffer;
}

// Same blend as the game's, but the alpha channel accumulates coverage (ONE, INV_SRC_ALPHA) and is
// written whenever colour is, so drawing onto a transparent target yields premultiplied RGBA. Draws
// that write no colour (stencil masks clipping subtitle and notification text) stay invisible.
ID3D11BlendState* coverageBlend(ID3D11BlendState* s) {
    if (!s) return nullptr;
    auto it = g_blendClones.find(s);
    if (it != g_blendClones.end()) return it->second;
    D3D11_BLEND_DESC d{};
    s->GetDesc(&d);
    int n = d.IndependentBlendEnable ? 8 : 1;
    for (int i = 0; i < n; ++i) {
        auto& rt = d.RenderTarget[i];
        if (rt.RenderTargetWriteMask & (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE))
            rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (rt.BlendEnable) {
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
    }
    ID3D11BlendState* clone = nullptr;
    if (FAILED(g_device->CreateBlendState(&d, &clone))) clone = nullptr;
    s->AddRef();  // keep the key alive so the pointer cannot be reused for another state
    g_blendClones[s] = clone;
    return clone;
}

bool ensureTarget() {
    if (!g_backbuffer || !g_device) return false;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_backbuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    bb->Release();
    if (g_tex && d.Width == g_texDesc.Width && d.Height == g_texDesc.Height && d.Format == g_texDesc.Format) return true;
    if (g_rtv) g_rtv->Release();
    if (g_tex) g_tex->Release();
    g_rtv = nullptr;
    g_tex = nullptr;
    D3D11_TEXTURE2D_DESC t{};
    t.Width = d.Width;
    t.Height = d.Height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.Format = d.Format;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&t, nullptr, &g_tex)) || FAILED(g_device->CreateRenderTargetView(g_tex, nullptr, &g_rtv))) {
        LOG("HUD: could not create the %ux%u capture target (fmt %d)", t.Width, t.Height, t.Format);
        if (g_tex) g_tex->Release();
        g_tex = nullptr;
        return false;
    }
    g_texDesc = t;
    LOG("HUD: capture target %ux%u fmt %d", t.Width, t.Height, t.Format);
    return true;
}

void STDMETHODCALLTYPE hkOMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (g_capturing && c == g_ctx && GetCurrentThreadId() == g_presentThread) {
        g_redirected = n >= 1 && rtvs && isBackbuffer(rtvs[0]);
        g_requestedDsv = dsv;
        if (g_redirected) {
            g_requestedRtv = rtvs[0];
            ID3D11RenderTargetView* v[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            for (UINT i = 0; i < n && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) v[i] = rtvs[i];
            v[0] = g_rtv;
            g_omSetRT.stdcall<void>(c, n, v, dsv);
            return;
        }
    }
    g_omSetRT.stdcall<void>(c, n, rtvs, dsv);
}

void STDMETHODCALLTYPE hkOMSetBlend(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) {
    if (g_capturing && c == g_ctx && GetCurrentThreadId() == g_presentThread) {
        g_requestedBlend = b;
        if (f) memcpy(g_requestedFactor, f, sizeof g_requestedFactor);
        g_requestedMask = m;
        g_omSetBlend.stdcall<void>(c, coverageBlend(b), f, m);
        return;
    }
    g_omSetBlend.stdcall<void>(c, b, f, m);
}

int __fastcall hkUiPass(void* self, void* /*edx*/, void* renderer) {
    bool capture = GetCurrentThreadId() == g_presentThread && config::hudLayer() && xr::hudCaptureWanted() && ensureTarget();
    if (!capture) return g_uiPass.thiscall<int>(self, renderer);

    // Take over the current bindings as if the game had just set them.
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    g_ctx->OMGetBlendState(&blend, factor, &mask);

    static const FLOAT clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_capturing = true;
    UINT n = 0;
    while (n < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && rtv[n]) ++n;
    hkOMSetRT(g_ctx, n, rtv, dsv);
    hkOMSetBlend(g_ctx, blend, factor, mask);

    int r = g_uiPass.thiscall<int>(self, renderer);

    g_capturing = false;
    if (g_redirected) {  // hand back what the game last asked for
        g_omSetRT.stdcall<void>(g_ctx, 1, &g_requestedRtv, g_requestedDsv);
        g_redirected = false;
    }
    g_omSetBlend.stdcall<void>(g_ctx, g_requestedBlend, g_requestedFactor, g_requestedMask);
    g_capturedAt = g_presents;

    for (auto* v : rtv)
        if (v) v->Release();
    if (dsv) dsv->Release();
    if (blend) blend->Release();
    return r;
}

}  // namespace

void installContextHooks(ID3D11Device* device) {
    if (g_device) return;
    g_device = device;
    g_device->AddRef();
    g_device->GetImmediateContext(&g_ctx);
    void** vt = *reinterpret_cast<void***>(g_ctx);
    g_omSetRT = safetyhook::create_inline(vt[33], reinterpret_cast<void*>(&hkOMSetRT));        // OMSetRenderTargets
    g_omSetBlend = safetyhook::create_inline(vt[35], reinterpret_cast<void*>(&hkOMSetBlend));  // OMSetBlendState
    LOG("HUD: context hooks %s", g_omSetRT && g_omSetBlend ? "ok" : "FAILED");
}

bool installEngineHook() {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* p = reinterpret_cast<uint8_t*>(base + kUiPassVa - 0x400000);
    // mov eax,[_tls_index] ; sub esp,0x18 ; push esi ; mov esi,ecx
    static const uint8_t expect[] = {0x83, 0xEC, 0x18, 0x56, 0x8B, 0xF1};
    if (p[0] != 0xA1 || memcmp(p + 5, expect, sizeof expect) != 0) {
        LOG("HUD: UI pass at %p does not match the expected bytes; not hooked", p);
        return false;
    }
    g_uiPass = safetyhook::create_inline(p, reinterpret_cast<void*>(&hkUiPass));
    return static_cast<bool>(g_uiPass);
}

void onPresent(IDXGISwapChain* sc) {
    g_presentThread = GetCurrentThreadId();
    ++g_presents;
    ID3D11Resource* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Resource), reinterpret_cast<void**>(&bb)))) {
        g_backbuffer = bb;
        bb->Release();
    }
}

ID3D11Texture2D* latest() {
    // Captured during this frame or the previous one (each eye is one game frame).
    return g_tex && g_capturedAt && g_presents - g_capturedAt <= 2 ? g_tex : nullptr;
}

}  // namespace rfgvr::hud
