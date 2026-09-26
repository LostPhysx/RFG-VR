#include "autostart.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game.h"
#include "gamestate.h"
#include "log.h"

namespace rfgvr::autostart {
namespace {

using clock = std::chrono::steady_clock;
using game::at;

// Title screen: passed once an input device belongs to player 0. A key press does that in the
// input system (addInputEvent): unowned keyboard/mouse slots go to the pending player, and the
// pending assignment is marked done. Autostart applies the same result.
constexpr uintptr_t kTitlePassed = 0x02C03571;  // byte
constexpr uintptr_t kInputSystem = 0x01CE86C0;  // pointer
constexpr size_t kInputSlots = 0x11AC;          // 16 x 24 bytes: int state (1 keyboard, 2 mouse), byte player at +20
constexpr size_t kInputSlotSize = 24;
constexpr size_t kPendingMode = 0x132C;    // int, 3 = nothing pending
constexpr size_t kPendingPlayer = 0x1330;  // int, 0xFF = none

// Main menu: item handler (0 New Game, 1 Load Game, ...) and its state.
constexpr uintptr_t kMainMenuSelect = 0x8F7C30;    // void __cdecl(int item)
constexpr uintptr_t kMainMenuList = 0x016631A8;    // int, -1 until built
constexpr uintptr_t kMainMenuAction = 0x016631C8;  // int, -1 = none pending
constexpr uintptr_t kMenuBusy = 0x02C03579;        // byte
// New Game follow-up dialogs, pre-answered: autosave notice, difficulty, start weapon.
constexpr uintptr_t kAutosaveShown = 0x02C0356C;
constexpr uintptr_t kDifficultyDone = 0x02C0356F;
constexpr uintptr_t kWeaponDone = 0x02C03570;

// Load Game screen: save list (0xBC-byte entries) and the load call it makes on confirm.
constexpr uintptr_t kSaveLoadMode = 0x02C0812C;  // int, 0 = load
constexpr uintptr_t kSaveEntries = 0x02C08118;   // entry array pointer
constexpr uintptr_t kSaveCount = 0x02C0811C;     // int
constexpr uintptr_t kSaveLoadDone = 0x02C08108;  // byte
constexpr uintptr_t kLoadSave = 0x7E7650;        // void __cdecl(entry*, bool)
constexpr uintptr_t kPopState = 0x7D8870;        // void __cdecl(), like Esc
constexpr size_t kSaveEntrySize = 0xBC;

enum : int { GS_MAINMENU = 0x00, GS_GAMEPLAY = 0x01, GS_SAVE_LOAD_SCREEN = 0x32 };

constexpr auto kSettle = std::chrono::seconds(1);      // per game state
constexpr auto kAfterTitle = std::chrono::seconds(2);  // profile and save list load
constexpr auto kGiveUp = std::chrono::seconds(240);

bool g_init = false, g_enabled = false, g_newGame = false, g_done = false;
bool g_titleKey = false, g_selected = false, g_loaded = false, g_startedNewGame = false;
int g_lastState = -2;
clock::time_point g_start, g_stateSince, g_titlePassedAt;

// Save time: year since 2000, month, day, hour, minute, second (the fields the list sorts by).
struct SaveTime {
    int v[6];
};
SaveTime saveTime(const uint8_t* e) {
    auto f = [e](size_t off) { return *reinterpret_cast<const int*>(e + off); };
    return {{f(0x1C), f(0x18), f(0x14), f(0x20), f(0x24), f(0x28)}};
}
bool newer(const SaveTime& a, const SaveTime& b) {
    for (int i = 0; i < 6; ++i)
        if (a.v[i] != b.v[i]) return a.v[i] > b.v[i];
    return false;
}

void init() {
    g_init = true;
    if (!game::isSteamBuild()) return;
    // mov eax,[esp+4] ; xorps xmm0,xmm0 ; push ebx ; xor ebx,ebx
    bool codeOk = game::codeMatches(kMainMenuSelect, {0x8B, 0x44, 0x24, 0x04, 0x0F, 0x57, 0xC0, 0x53, 0x33, 0xDB});
    FILE* f = nullptr;
    if (_wfopen_s(&f, game::pathNextToDll(L"rfg-vr-autostart.txt").c_str(), L"r") == 0 && f) {
        char buf[64] = {};
        fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        g_enabled = codeOk;
        g_newGame = strstr(buf, "new") != nullptr;
    }
    g_start = g_stateSince = clock::now();
    LOG("autostart: %s", !codeOk      ? "disabled (main menu code differs)"
                         : !g_enabled ? "off"
                         : g_newGame  ? "new game"
                                      : "load the newest save");
}

void passTitle() {
    auto* input = at<uint8_t*>(kInputSystem);
    if (!input) return;
    g_titleKey = true;
    for (int i = 0; i < 16; ++i) {
        uint8_t* slot = input + kInputSlots + i * kInputSlotSize;
        int state = *reinterpret_cast<int*>(slot);
        if ((state == 1 || state == 2) && slot[20] == 0xFF) slot[20] = 0;
    }
    *reinterpret_cast<int*>(input + kPendingPlayer) = 0xFF;
    *reinterpret_cast<int*>(input + kPendingMode) = 3;
    LOG("autostart: title screen passed (keyboard/mouse -> player 0)");
}

void selectMenuItem(int item) { reinterpret_cast<void(__cdecl*)(int)>(game::runtime(kMainMenuSelect))(item); }

void loadNewestSave() {
    int count = at<int>(kSaveCount);
    auto* entries = at<uint8_t*>(kSaveEntries);
    int best = 0;
    for (int i = 1; i < count && i < 12; ++i)
        if (newer(saveTime(entries + i * kSaveEntrySize), saveTime(entries + best * kSaveEntrySize))) best = i;
    SaveTime t = saveTime(entries + best * kSaveEntrySize);
    LOG("autostart: loading save %d of %d (%04d-%02d-%02d %02d:%02d)", best, count, 2000 + t.v[0], t.v[1], t.v[2],
        t.v[3], t.v[4]);
    reinterpret_cast<void(__cdecl*)(void*, bool)>(game::runtime(kLoadSave))(entries + best * kSaveEntrySize, false);
    at<uint8_t>(kSaveLoadDone) = 1;
    g_loaded = true;
}

}  // namespace

void onPresent() {
    if (!g_init) init();
    if (!g_enabled || g_done) return;
    auto now = clock::now();
    int st = gamestate::current();
    if (st != g_lastState) {
        g_lastState = st;
        g_stateSince = now;
    }

    if (st == GS_GAMEPLAY || now - g_start > kGiveUp) {
        g_done = true;
        LOG("autostart: %s after %.1f s", st == GS_GAMEPLAY ? "gameplay reached" : "gave up",
            std::chrono::duration<double>(now - g_start).count());
        return;
    }
    if (now - g_stateSince < kSettle) return;

    if (!g_selected && st == GS_MAINMENU) {
        if (!at<uint8_t>(kTitlePassed)) {
            if (!g_titleKey) passTitle();
            return;
        }
        if (g_titlePassedAt == clock::time_point{}) g_titlePassedAt = now;
        if (now - g_titlePassedAt < kAfterTitle) return;
        if (at<int>(kMainMenuList) == -1 || at<int>(kMainMenuAction) != -1 || at<uint8_t>(kMenuBusy) == 1) return;
        g_selected = true;
        if (g_newGame) {
            selectMenuItem(0);
            // The handler just reset these; answering them makes the menu start the game directly.
            at<uint8_t>(kAutosaveShown) = 1;
            at<uint8_t>(kDifficultyDone) = 1;
            at<uint8_t>(kWeaponDone) = 1;
            g_startedNewGame = true;
            LOG("autostart: New Game");
        } else {
            selectMenuItem(1);
            LOG("autostart: Load Game");
        }
        return;
    }

    if (g_selected && !g_loaded && st == GS_SAVE_LOAD_SCREEN && at<int>(kSaveLoadMode) == 0) {
        if (at<int>(kSaveCount) > 0 && at<uint8_t*>(kSaveEntries)) {
            loadNewestSave();
        } else if (now - g_stateSince > std::chrono::seconds(3)) {
            reinterpret_cast<void(__cdecl*)()>(game::runtime(kPopState))();
            g_done = true;
            LOG("autostart: no saves; back to the main menu");
        }
    }
}

bool startedNewGame() { return g_startedNewGame; }

}  // namespace rfgvr::autostart
