// rfg-vr: VR mod for Red Faction Guerrilla Re-Mars-tered (Steam, x86).
// Loaded as a dinput8.dll proxy from the game folder; forwards DirectInput to the system DLL.
#include <windows.h>
#include <unknwn.h>

#include <string>

#include "camera_hook.h"
#include "d3d11_hook.h"
#include "log.h"
#include "video_hook.h"

namespace {

HMODULE g_self = nullptr;
HMODULE g_realDinput8 = nullptr;

// All engine addresses in the mod are for this Steam rfg.exe (SHA-256 0d52039e...2df4); the PE
// timestamp is a cheap stand-in for hashing 25 MB at startup.
constexpr DWORD kSteamExeTimestamp = 0x5B9B718A;  // 2018-09-14 08:30:02 UTC

std::wstring moduleDir(HMODULE m) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

FARPROC realProc(const char* name) {
    if (!g_realDinput8) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);  // SysWOW64 for this 32-bit process via redirection
        g_realDinput8 = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());
        if (!g_realDinput8) LOG("FATAL: could not load system dinput8.dll");
    }
    return g_realDinput8 ? GetProcAddress(g_realDinput8, name) : nullptr;
}

void onAttach() {
    rfgvr::log::init((moduleDir(g_self) + L"rfg-vr.log").c_str());
    HMODULE exe = GetModuleHandleW(nullptr);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(exe) +
                                                  reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp;
    LOG("rfg-vr loaded. exe base %p, PE timestamp 0x%08lX (%s)", exe, ts,
        ts == kSteamExeTimestamp ? "Steam build: known addresses apply" : "UNKNOWN build: address-based hooks disabled");
    rfgvr::d3d11::install();
    rfgvr::video::install();
    if (ts == kSteamExeTimestamp) rfgvr::camera::install();
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        onAttach();  // log file + hook installation only; OpenXR starts at the first Present
    }
    return TRUE;
}

// --- dinput8 forwarding exports (see dinput8.def) -------------------------------------------
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
