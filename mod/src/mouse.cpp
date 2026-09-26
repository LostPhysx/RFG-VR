#include "mouse.h"

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>

#include "game.h"
#include "gamestate.h"
#include "log.h"
#include "xr.h"

namespace rfgvr::mouse {
namespace {

// The engine's window procedure (subclass), LRESULT __stdcall(HWND, UINT, WPARAM, LPARAM). In menus
// it takes WM_MOUSEMOVE client coordinates as the pointer position.
constexpr uintptr_t kWndProcVa = 0xC7D070;

SafetyHookInline g_wndProc;
std::atomic<uint32_t> g_renderW{0}, g_renderH{0};  // backbuffer size, which menus are laid out for

LRESULT __stdcall hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Scale client coordinates (mouse move and buttons) from window to render size. Not in gameplay:
    // there look input is the offset from the recentred cursor, and scaling it spins the camera.
    uint32_t rw = g_renderW.load(), rh = g_renderH.load();
    RECT rc{};
    if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK && !gamestate::gameplay() && rw && rh &&
        GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0 &&
        (static_cast<uint32_t>(rc.right) != rw || static_cast<uint32_t>(rc.bottom) != rh)) {
        int x = MulDiv(static_cast<short>(LOWORD(lp)), static_cast<int>(rw), rc.right);
        int y = MulDiv(static_cast<short>(HIWORD(lp)), static_cast<int>(rh), rc.bottom);
        lp = MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
    }
    return g_wndProc.stdcall<LRESULT>(hwnd, msg, wp, lp);
}

}  // namespace

bool install() {
    // sub esp,0x2C ; mov eax,[__security_cookie]
    g_wndProc = game::hook(kWndProcVa, {0x83, 0xEC, 0x2C, 0xA1}, &hkWndProc, "window procedure");
    LOG("Mouse: window procedure hook %s", g_wndProc ? "ok" : "FAILED");
    return static_cast<bool>(g_wndProc);
}

void onPresent(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    g_renderW = d.BufferDesc.Width;
    g_renderH = d.BufferDesc.Height;

    // In VR the cursor is invisible: keep it inside the focused window (the engine only does so while
    // the cursor is hidden, in gameplay).
    static bool clipped = false;
    bool want = xr::stereoActive() && d.OutputWindow && GetForegroundWindow() == d.OutputWindow;
    if (want) {
        RECT rc{};
        GetClientRect(d.OutputWindow, &rc);
        MapWindowPoints(d.OutputWindow, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
    } else if (clipped) {
        ClipCursor(nullptr);
    }
    clipped = want;
}

}  // namespace rfgvr::mouse
