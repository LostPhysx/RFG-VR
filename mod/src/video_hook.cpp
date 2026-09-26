#include "video_hook.h"

#include <windows.h>

#include <cctype>
#include <string>

#include "autostart.h"
#include "iat.h"
#include "log.h"

namespace rfgvr::video {
namespace {

using BinkOpen_t = void*(__stdcall*)(const char* name, unsigned flags);
using BinkGoto_t = void(__stdcall*)(void* bink, unsigned frame, int flags);

BinkOpen_t g_origBinkOpen = nullptr;
BinkGoto_t g_binkGoto = nullptr;

// The New Game intro. Deleting the file instead makes the game wait on a black screen.
bool isIntro(const char* name) {
    if (!name) return false;
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("rfg_cine_00a") != std::string::npos;
}

void* __stdcall hkBinkOpen(const char* name, unsigned flags) {
    void* h = g_origBinkOpen(name, flags);
    if (!g_binkGoto)
        if (HMODULE bink = GetModuleHandleW(L"binkw32.dll"))
            g_binkGoto = reinterpret_cast<BinkGoto_t>(GetProcAddress(bink, "_BinkGoto@12"));
    LOG("BinkOpen(\"%s\", 0x%X) -> %p", name ? name : "(null)", flags, h);
    if (h && g_binkGoto && autostart::startedNewGame() && isIntro(name)) {
        // BINK starts with Width, Height, Frames, FrameNum: jump to the last frame.
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
    void* prev =
        iat::hook(GetModuleHandleW(nullptr), "binkw32.dll", "_BinkOpen@8", reinterpret_cast<void*>(&hkBinkOpen));
    g_origBinkOpen = reinterpret_cast<BinkOpen_t>(prev);
    if (HMODULE bink = GetModuleHandleW(L"binkw32.dll"))
        g_binkGoto = reinterpret_cast<BinkGoto_t>(GetProcAddress(bink, "_BinkGoto@12"));
    LOG("Video hook: %s", prev ? "ok" : "FAILED");
    return prev != nullptr;
}

}  // namespace rfgvr::video
