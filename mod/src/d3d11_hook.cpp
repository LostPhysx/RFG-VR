#include "d3d11_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <safetyhook.hpp>

#include <atomic>
#include <chrono>

#include "camera_hook.h"
#include "gamestate.h"
#include "hud.h"
#include "log.h"
#include "mouse.h"
#include "xr.h"
#ifdef RFGVR_DEV
#include "autostart.h"
#endif

namespace rfgvr::d3d11 {
namespace {

SafetyHookInline g_createDevice;
SafetyHookInline g_present;
std::atomic<bool> g_presentHooked{false};

// Used address space of this 32-bit process and its largest free block.
void addressSpace(unsigned& usedMb, unsigned& largestFreeMb) {
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

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    using clock = std::chrono::steady_clock;
    static uint64_t frame = 0, windowFrames = 0;
    static auto windowStart = clock::now();

    if (frame++ == 0) {
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        LOG("First Present: %ux%u fmt %d, swap effect %d", d.BufferDesc.Width, d.BufferDesc.Height,
            d.BufferDesc.Format, d.SwapEffect);
    }
    ++windowFrames;
    auto now = clock::now();
    if (now - windowStart >= std::chrono::seconds(10)) {
        unsigned used = 0, freeBlock = 0;
        addressSpace(used, freeBlock);
        LOG("Present: %.1f fps, address space %u MB used, largest free block %u MB",
            windowFrames / std::chrono::duration<double>(now - windowStart).count(), used, freeBlock);
        windowFrames = 0;
        windowStart = now;
    }
    camera::onPresent();
    gamestate::update();
#ifdef RFGVR_DEV
    autostart::onPresent();
#endif
    hud::onPresent(sc);
    mouse::onPresent(sc);
    xr::onPresent(sc);  // before Present: the backbuffer holds the finished frame
    return g_present.stdcall<HRESULT>(sc, syncInterval, flags);
}

// IDXGISwapChain::Present is shared by all swapchains of the implementation, so a throwaway
// swapchain on the game's device gives the address the game will call.
void hookPresent(ID3D11Device* device) {
    if (g_presentHooked.exchange(true)) return;
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        LOG("Present hook: could not reach the DXGI factory");
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
    if (SUCCEEDED(factory->CreateSwapChain(device, &d, &dummy))) {
        void* present = (*reinterpret_cast<void***>(dummy))[8];
        g_present = safetyhook::create_inline(present, reinterpret_cast<void*>(&hkPresent));
        LOG("Present hook: %s", g_present ? "ok" : "FAILED");
        dummy->Release();
    } else {
        LOG("Present hook: dummy swapchain creation failed");
    }
    DestroyWindow(hwnd);
    factory->Release();
    adapter->Release();
    dxgiDevice->Release();
}

HRESULT WINAPI hkD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                   const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk, ID3D11Device** device,
                                   D3D_FEATURE_LEVEL* gotLevel, ID3D11DeviceContext** ctx) {
    HRESULT hr = g_createDevice.stdcall<HRESULT>(adapter, type, software, flags, levels, numLevels, sdk, device,
                                                 gotLevel, ctx);
    LOG("D3D11CreateDevice -> 0x%08lX, feature level 0x%X", static_cast<unsigned long>(hr), gotLevel ? *gotLevel : 0);
    if (SUCCEEDED(hr) && device && *device) {
        hookPresent(*device);
        hud::installContextHooks(*device);
    }
    return hr;
}

}  // namespace

bool install() {
    // Inline hook on the export: overlays and capture tools may re-patch rfg.exe's import table.
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) d3d11 = LoadLibraryW(L"d3d11.dll");
    void* target = d3d11 ? reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    if (target) g_createDevice = safetyhook::create_inline(target, reinterpret_cast<void*>(&hkD3D11CreateDevice));
    LOG("D3D11CreateDevice hook: %s", g_createDevice ? "ok" : "FAILED");
    return static_cast<bool>(g_createDevice);
}

}  // namespace rfgvr::d3d11
