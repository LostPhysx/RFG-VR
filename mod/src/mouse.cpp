#include "mouse.h"

#include <windows.h>
#include <dxgi.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cstdint>

#include "gamestate.h"
#include "log.h"
#include "xr.h"

namespace rfgvr::mouse {
namespace {

// Steam rfg.exe: the engine's window procedure (subclass), LRESULT __stdcall(HWND, UINT, WPARAM,
// LPARAM). For WM_MOUSEMOVE it hands the client coordinates in lParam to the input system as the
// pointer position (FUN_00c7cb70). In gameplay the cursor is hidden and kept at the window centre
// (FUN_00c75430), and look input is its offset from that centre: coordinates are only scaled
// outside gameplay, or the camera spins.
constexpr uintptr_t kWndProcVa = 0xC7D070;

SafetyHookInline g_wndProc;
HWND g_hwnd = nullptr;
std::atomic<uint32_t> g_renderW{0}, g_renderH{0};  // backbuffer size (the size menus are laid out for)

bool carriesClientPoint(UINT msg) {
    // WM_MOUSEMOVE and the button messages; the wheel messages carry screen coordinates.
    return msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK;
}

LRESULT __stdcall hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    uint32_t rw = g_renderW.load(), rh = g_renderH.load();
    RECT rc{};
    if (carriesClientPoint(msg) && !gamestate::gameplay() && rw && rh && GetClientRect(hwnd, &rc) && rc.right > 0 &&
        rc.bottom > 0 && (static_cast<uint32_t>(rc.right) != rw || static_cast<uint32_t>(rc.bottom) != rh)) {
        int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        x = MulDiv(x, static_cast<int>(rw), rc.right);
        y = MulDiv(y, static_cast<int>(rh), rc.bottom);
        lp = MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
    }
    return g_wndProc.stdcall<LRESULT>(hwnd, msg, wp, lp);
}

}  // namespace

bool install() {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* p = reinterpret_cast<uint8_t*>(base + kWndProcVa - 0x400000);
    // sub esp,0x2C ; mov eax,[__security_cookie]
    if (p[0] != 0x83 || p[1] != 0xEC || p[2] != 0x2C || p[3] != 0xA1) {
        LOG("mouse: window procedure at %p does not match the expected bytes; not hooked", p);
        return false;
    }
    g_wndProc = safetyhook::create_inline(p, reinterpret_cast<void*>(&hkWndProc));
    return static_cast<bool>(g_wndProc);
}

void onPresent(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    g_hwnd = d.OutputWindow;
    g_renderW = d.BufferDesc.Width;
    g_renderH = d.BufferDesc.Height;

    // Keep the cursor inside the window while the headset shows the game and the window has focus.
    static bool clipped = false;
    bool want = xr::stereoActive() && g_hwnd && GetForegroundWindow() == g_hwnd;
    if (want) {
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
    } else if (clipped) {
        ClipCursor(nullptr);
    }
    clipped = want;
}

}  // namespace rfgvr::mouse
