// Logs every Bink video the game opens (rfg.exe imports binkw32.dll!_BinkOpen@8) and, when
// rfg-vr-autostart.txt is present, skips the New Game intro cinematic by jumping it to its last
// frame right after it opens. The file must stay in place: when it is missing the game waits in
// GS_VIDEO_CUTSCENE_PLAY forever (black screen).
#include "video_hook.h"

#include <windows.h>

#include <cctype>
#include <string>

#include "iat.h"
#include "log.h"

namespace rfgvr::video {
namespace {

using BinkOpen_t = void*(__stdcall*)(const char* name, unsigned flags);
using BinkGoto_t = void(__stdcall*)(void* bink, unsigned frame, int flags);

BinkOpen_t g_origBinkOpen = nullptr;
BinkGoto_t g_binkGoto = nullptr;
bool g_skipIntro = false;

// Videos skipped in dev auto-start mode (lower-case substrings of the path).
const char* const kSkip[] = {"rfg_cine_00a"};

bool fileNextToDll(const wchar_t* name) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&fileNextToDll), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + name;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool shouldSkip(const char* name) {
    if (!g_skipIntro || !name) return false;
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* s : kSkip)
        if (lower.find(s) != std::string::npos) return true;
    return false;
}

void* __stdcall hkBinkOpen(const char* name, unsigned flags) {
    void* h = g_origBinkOpen(name, flags);
    if (!g_binkGoto)
        if (HMODULE bink = GetModuleHandleW(L"binkw32.dll"))
            g_binkGoto = reinterpret_cast<BinkGoto_t>(GetProcAddress(bink, "_BinkGoto@12"));
    LOG("BinkOpen(\"%s\", 0x%X) -> %p", name ? name : "(null)", flags, h);
    if (h && g_binkGoto && shouldSkip(name)) {
        // BINK struct starts with Width, Height, Frames, FrameNum (Bink SDK layout).
        unsigned frames = static_cast<unsigned*>(h)[2];
        if (frames > 1) {
            g_binkGoto(h, frames, 0);
            LOG("  skipped: jumped to last frame %u (now at %u)", frames, static_cast<unsigned*>(h)[3]);
        }
    }
    return h;
}

}  // namespace

bool install() {
    void* prev = iat::hook(GetModuleHandleW(nullptr), "binkw32.dll", "_BinkOpen@8", reinterpret_cast<void*>(&hkBinkOpen));
    g_origBinkOpen = reinterpret_cast<BinkOpen_t>(prev);
    if (HMODULE bink = GetModuleHandleW(L"binkw32.dll"))
        g_binkGoto = reinterpret_cast<BinkGoto_t>(GetProcAddress(bink, "_BinkGoto@12"));
    g_skipIntro = fileNextToDll(L"rfg-vr-autostart.txt");
    LOG("IAT hook _BinkOpen@8: %s; BinkGoto %p; intro skip %s", prev ? "ok" : "NOT FOUND",
        reinterpret_cast<void*>(g_binkGoto), g_skipIntro ? "on" : "off");
    return prev != nullptr;
}

}  // namespace rfgvr::video
