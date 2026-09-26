#include "gamestate.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "log.h"

namespace rfgvr::gamestate {
namespace {

constexpr DWORD kSteamExeTimestamp = 0x5B9B718A;
constexpr uintptr_t kGameseqGetState = 0x7BFCF0;  // int __cdecl gameseq_get_state()
constexpr int kGameplay = 0x01;                   // GS_GAMEPLAY

bool g_init = false;
int(__cdecl* g_getState)() = nullptr;
std::atomic<int> g_state{-1};

}  // namespace

void update() {
    if (!g_init) {
        g_init = true;
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp == kSteamExeTimestamp)
            g_getState = reinterpret_cast<int(__cdecl*)()>(base + kGameseqGetState - 0x400000);
    }
    if (!g_getState) return;
    int st = g_getState();
    if (st != g_state.exchange(st)) LOG("game state -> %d (0x%X)", st, st);
}

int current() { return g_state.load(); }

bool gameplay() { return g_state.load() == kGameplay; }

}  // namespace rfgvr::gamestate
