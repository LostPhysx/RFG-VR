#include "d3d11_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <safetyhook.hpp>

#include <atomic>
#include <chrono>

#include "autostart.h"
#include "camera_hook.h"
#include "gamestate.h"
#include "hud.h"
#include "log.h"
#include "mouse.h"
#include "xr.h"

namespace rfgvr::d3d11 {

static SafetyHookInline g_createDevice;  // inline hook on d3d11.dll!D3D11CreateDevice
static SafetyHookInline g_present;
static SafetyHookInline g_resizeBuffers;
static std::atomic<bool> g_swapchainHooksInstalled{false};

// Address space of this 32-bit process: used, and the largest free block (what a big allocation needs).
static void addressSpace(unsigned& usedMb, unsigned& largestFreeMb) {
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    usedMb = static_cast<unsigned>((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
    SIZE_T largest = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi); a += mbi.RegionSize) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        if (a + mbi.RegionSize < a) break;
    }
    largestFreeMb = static_cast<unsigned>(largest >> 20);
}

static void logSwapchain(IDXGISwapChain* sc, const char* why) {
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    LOG("%s: swapchain %p %ux%u fmt %d, buffers %u, swapEffect %d, windowed %d, flags 0x%X, hwnd %p", why, sc,
        d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.Windowed,
        d.Flags, d.OutputWindow);
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    using clock = std::chrono::steady_clock;
    static uint64_t frame = 0;
    static uint64_t windowFrames = 0;
    static auto windowStart = clock::now();

    if (frame++ == 0) logSwapchain(sc, "First Present");
    ++windowFrames;
    auto now = clock::now();
    if (now - windowStart >= std::chrono::seconds(10)) {
        double secs = std::chrono::duration<double>(now - windowStart).count();
        unsigned used = 0, freeBlock = 0;
        addressSpace(used, freeBlock);
        LOG("Present: frame %llu, %.1f fps, syncInterval %u, address space %u MB used, largest free block %u MB", frame,
            windowFrames / secs, syncInterval, used, freeBlock);
        windowFrames = 0;
        windowStart = now;
    }
    camera::onPresent();
    gamestate::update();
    autostart::onPresent();
    hud::onPresent(sc);
    mouse::onPresent(sc);
    xr::onPresent(sc);  // before Present: the backbuffer holds the finished frame
    return g_present.stdcall<HRESULT>(sc, syncInterval, flags);
}

static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                                 UINT flags) {
    LOG("ResizeBuffers: count %u, %ux%u, fmt %d, flags 0x%X", count, w, h, fmt, flags);
    HRESULT hr = g_resizeBuffers.stdcall<HRESULT>(sc, count, w, h, fmt, flags);
    logSwapchain(sc, "After ResizeBuffers");
    return hr;
}

// DXGI swapchain methods are shared by every swapchain of the implementation, so a throwaway
// swapchain on the game's own device gives us the exact Present/ResizeBuffers the game will call.
static void installSwapchainHooks(ID3D11Device* device) {
    if (g_swapchainHooksInstalled.exchange(true)) return;

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        LOG("installSwapchainHooks: could not reach the DXGI factory");
        if (adapter) adapter->Release();
        if (dxgiDevice) dxgiDevice->Release();
        return;
    }

    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    LOG("Game adapter: %ls", ad.Description);

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"rfgvr-dummy", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferDesc.Width = 8;
    d.BufferDesc.Height = 8;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 1;
    d.OutputWindow = hwnd;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dummy = nullptr;
    HRESULT hr = factory->CreateSwapChain(device, &d, &dummy);
    if (SUCCEEDED(hr)) {
        void** vtbl = *reinterpret_cast<void***>(dummy);
        void* present = vtbl[8];
        void* resize = vtbl[13];
        g_present = safetyhook::create_inline(present, reinterpret_cast<void*>(&hkPresent));
        g_resizeBuffers = safetyhook::create_inline(resize, reinterpret_cast<void*>(&hkResizeBuffers));
        LOG("Hooked IDXGISwapChain::Present %p (%s), ResizeBuffers %p (%s)", present,
            g_present ? "ok" : "FAILED", resize, g_resizeBuffers ? "ok" : "FAILED");
        dummy->Release();
    } else {
        LOG("installSwapchainHooks: dummy CreateSwapChain failed 0x%08lX", static_cast<unsigned long>(hr));
    }
    DestroyWindow(hwnd);
    factory->Release();
    adapter->Release();
    dxgiDevice->Release();
}

static HRESULT WINAPI hkD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                          const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk,
                                          ID3D11Device** device, D3D_FEATURE_LEVEL* gotLevel,
                                          ID3D11DeviceContext** ctx) {
    HRESULT hr = g_createDevice.stdcall<HRESULT>(adapter, type, software, flags, levels, numLevels, sdk, device, gotLevel, ctx);
    LOG("D3D11CreateDevice(adapter %p, type %d, flags 0x%X, %u levels) -> 0x%08lX, device %p, level 0x%X", adapter,
        type, flags, numLevels, static_cast<unsigned long>(hr), device ? *device : nullptr,
        gotLevel ? *gotLevel : 0);
    if (SUCCEEDED(hr) && device && *device) {
        installSwapchainHooks(*device);
        hud::installContextHooks(*device);
    }
    return hr;
}

bool install() {
    // Inline hook on the export itself rather than rfg.exe's import table: injected tools (overlays,
    // capture tools) re-patch the import table but still end up calling the real export.
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) d3d11 = LoadLibraryW(L"d3d11.dll");
    void* target = d3d11 ? reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    if (target) g_createDevice = safetyhook::create_inline(target, reinterpret_cast<void*>(&hkD3D11CreateDevice));
    LOG("Inline hook d3d11!D3D11CreateDevice at %p: %s", target, g_createDevice ? "ok" : "FAILED");
    return static_cast<bool>(g_createDevice);
}

}  // namespace rfgvr::d3d11
