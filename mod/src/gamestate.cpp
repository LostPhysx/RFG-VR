#include "gamestate.h"

#include <atomic>

#include "game.h"
#include "log.h"

namespace rfgvr::gamestate {
namespace {

constexpr uintptr_t kGetStateVa = 0x7BFCF0;  // int __cdecl gameseq_get_state()
constexpr int kGameplay = 0x01;              // GS_GAMEPLAY

std::atomic<int> g_state{-1};

}  // namespace

void update() {
    static const bool steam = game::isSteamBuild();
    if (!steam) return;
    int st = reinterpret_cast<int(__cdecl*)()>(game::runtime(kGetStateVa))();
    if (st != g_state.exchange(st)) LOG("game state -> %d (0x%X)", st, st);
}

int current() { return g_state.load(); }

bool gameplay() { return g_state.load() == kGameplay; }

}  // namespace rfgvr::gamestate
