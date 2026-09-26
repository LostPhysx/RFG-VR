#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "game.h"
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
bool g_headAim = true;
bool g_hudLayer = true;
float g_hudDistance = 2.f;
float g_hudWidth = 2.4f;

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
    g_headAim = readFloat(L"VR", L"HeadAim", 1.f, 0.f, 1.f) != 0.f;
    g_hudLayer = readFloat(L"VR", L"HudLayer", 1.f, 0.f, 1.f) != 0.f;
    g_hudDistance = readFloat(L"VR", L"HudDistance", 2.f, 0.3f, 20.f);
    g_hudWidth = readFloat(L"VR", L"HudWidth", 2.4f, 0.1f, 40.f);
    LOG("config: WorldScale %.3f, CameraShake %d, HeadAim %d, HudLayer %d, HudDistance %.2f, HudWidth %.2f%s",
        g_worldScale, g_cameraShake, g_headAim, g_hudLayer, g_hudDistance, g_hudWidth,
        g_haveFile ? "" : " (no rfg-vr.ini, defaults)");
}

}  // namespace

void poll() {
    DWORD now = GetTickCount();
    if (!g_path.empty() && now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    bool first = g_path.empty();
    if (first) g_path = game::pathNextToDll(L"rfg-vr.ini");

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool have = GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa) != 0;
    if (!first && have == g_haveFile && (!have || CompareFileTime(&fa.ftLastWriteTime, &g_lastWrite) == 0)) return;
    g_haveFile = have;
    g_lastWrite = have ? fa.ftLastWriteTime : FILETIME{};
    load();
}

float worldScale() { return g_worldScale; }
bool cameraShake() { return g_cameraShake; }
bool headAim() { return g_headAim; }
bool hudLayer() { return g_hudLayer; }
float hudDistance() { return g_hudDistance; }
float hudWidth() { return g_hudWidth; }

}  // namespace rfgvr::config
