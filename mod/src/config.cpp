#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "log.h"

namespace rfgvr::config {
namespace {

constexpr float kMinWorldScale = 0.1f, kMaxWorldScale = 10.f;

std::wstring g_path;
FILETIME g_lastWrite{};
bool g_haveFile = false;
DWORD g_lastCheck = 0;
float g_worldScale = 1.f;
bool g_cameraShake = false;

std::wstring iniPath() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&iniPath), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"rfg-vr.ini";
}

float readFloat(const wchar_t* section, const wchar_t* key, float def, float lo, float hi) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, g_path.c_str());
    wchar_t* end = nullptr;
    float v = std::wcstof(buf, &end);
    if (end == buf) return def;
    return std::clamp(v, lo, hi);
}

void load() {
    g_worldScale = readFloat(L"VR", L"WorldScale", 1.f, kMinWorldScale, kMaxWorldScale);
    g_cameraShake = readFloat(L"VR", L"CameraShake", 0.f, 0.f, 1.f) != 0.f;
    LOG("config: WorldScale %.3f, CameraShake %d%s", g_worldScale, g_cameraShake,
        g_haveFile ? "" : " (no rfg-vr.ini, defaults)");
}

}  // namespace

void poll() {
    DWORD now = GetTickCount();
    if (!g_path.empty() && now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    bool first = g_path.empty();
    if (first) g_path = iniPath();

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool have = GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa) != 0;
    if (!first && have == g_haveFile && (!have || CompareFileTime(&fa.ftLastWriteTime, &g_lastWrite) == 0)) return;
    g_haveFile = have;
    g_lastWrite = have ? fa.ftLastWriteTime : FILETIME{};
    load();
}

float worldScale() { return g_worldScale; }
bool cameraShake() { return g_cameraShake; }

}  // namespace rfgvr::config
