// rfg-vr: VR mod for Red Faction Guerrilla Re-Mars-tered (Steam, x86), loaded as a dinput8.dll
// proxy from the game folder. DirectInput calls are forwarded to the system DLL.
#include <windows.h>
#include <unknwn.h>

#include <string>

#include "camera_hook.h"
#include "d3d11_hook.h"
#include "game.h"
#include "hud.h"
#include "input.h"
#include "log.h"
#include "mouse.h"
#include "version.h"
#ifdef RFGVR_DEV
#include "video_hook.h"
#endif

namespace {

HMODULE g_realDinput8 = nullptr;

FARPROC realProc(const char* name) {
    if (!g_realDinput8) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);  // SysWOW64 for this 32-bit process
        g_realDinput8 = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());
        if (!g_realDinput8) LOG("FATAL: could not load the system dinput8.dll");
    }
    return g_realDinput8 ? GetProcAddress(g_realDinput8, name) : nullptr;
}

void onAttach() {
    rfgvr::log::init(rfgvr::game::pathNextToDll(L"rfg-vr.log").c_str());
    bool steam = rfgvr::game::isSteamBuild();
#ifdef RFGVR_DEV
    constexpr const char* kBuild = " (dev build)";
#else
    constexpr const char* kBuild = "";
#endif
    LOG("rfg-vr %s%s loaded; %s", RFGVR_VERSION, kBuild,
        steam ? "Steam build" : "unknown game build: engine hooks disabled");
    rfgvr::d3d11::install();
#ifdef RFGVR_DEV
    rfgvr::video::install();
#endif
    if (steam) {
        rfgvr::camera::install();
        rfgvr::hud::installEngineHook();
        rfgvr::mouse::install();
        rfgvr::input::install();
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        onAttach();  // hooks only; OpenXR starts at the first Present
    }
    return TRUE;
}

// dinput8 exports (see dinput8.def), forwarded to the system DLL.
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, IUnknown* outer) {
    using fn_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto fn = reinterpret_cast<fn_t>(realProc("DirectInput8Create"));
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(realProc("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(realProc("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(realProc("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(realProc("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}
