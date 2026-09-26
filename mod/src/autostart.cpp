#include "autostart.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "gamestate.h"
#include "log.h"

namespace rfgvr::autostart {
namespace {

using clock = std::chrono::steady_clock;

constexpr DWORD kSteamExeTimestamp = 0x5B9B718A;

// Steam rfg.exe virtual addresses (image base 0x400000), rebased at runtime.
// Findings: research/00-local-findings.md, "Autostart".

// Title screen ("press any key", part of GS_MAINMENU). The main menu update (0x913910) waits while
// the title flag is 0 until an input device belongs to player 0 (FUN_00c748b0(input, 0) != 0), then
// runs the title sequence itself (sets the flag, starts the profile/session that reads the saves).
// A key press makes that assignment in the input system's addInputEvent (FUN_00c7acf0): every
// keyboard/mouse slot (state 1 or 2) not owned by a player goes to the pending player, and the
// pending assignment is marked done. Autostart applies the same result for player 0.
constexpr uintptr_t kTitlePassed = 0x02C03571;     // byte: 1 once past the title screen
constexpr uintptr_t kInputSystem = 0x01CE86C0;     // input system pointer
constexpr size_t kInputSlots = 0x11AC;             // 16 device slots of 24 bytes: int state, ..., byte player at +20
constexpr size_t kInputSlotSize = 24;
constexpr size_t kPendingMode = 0x132C;            // int: 3 = no assignment pending
constexpr size_t kPendingPlayer = 0x1330;          // int: player a pending assignment goes to, 0xFF none
constexpr auto kAfterTitle = std::chrono::seconds(2);  // let the profile and save list load

// Main menu. 0x8F7C30 is the main menu's item-selected handler (items: 0 New Game, 1 Load Game, 2 Wrecking
// Crew, 3 multiplayer, 4 Bonus Campaign, ...); FUN_008f75f0 runs the chosen action each frame.
constexpr uintptr_t kMainMenuSelect = 0x8F7C30;    // void __cdecl (int item)
constexpr uintptr_t kMainMenuList = 0x016631A8;    // int: main menu list handle, -1 until built
constexpr uintptr_t kMainMenuAction = 0x016631C8;  // int: pending main menu action, -1 none
constexpr uintptr_t kMenuBusy = 0x02C03579;        // byte: handler ignores New Game while 1
constexpr uintptr_t kAutosaveShown = 0x02C0356C;   // byte: autosave notice handled
constexpr uintptr_t kDifficultyDone = 0x02C0356F;  // byte: difficulty dialog answered
constexpr uintptr_t kWeaponDone = 0x02C03570;      // byte: start-weapon dialog answered

// Save/load screen (GS_SAVE_LOAD_SCREEN). Opening it builds the save list of player 0 (FUN_0089ddb0:
// up to 12 entries, grouped by save type, then newest first, so the newest is picked by date). In
// load mode, confirming an entry calls FUN_007e7650(entry, 0) (stores it, switches to
// GS_VERIFY_SAVEGAME, which loads it) and sets the screen's done flag (FUN_009076e0).
constexpr uintptr_t kSaveLoadMode = 0x02C0812C;    // int: 0 = load
constexpr uintptr_t kSaveEntries = 0x02C08118;     // save entry array (0xBC bytes each)
constexpr uintptr_t kSaveCount = 0x02C0811C;       // int: number of entries
constexpr uintptr_t kSaveLoadDone = 0x02C08108;    // byte
constexpr uintptr_t kLoadSave = 0x7E7650;          // void __cdecl (entry*, bool)
constexpr uintptr_t kPopState = 0x7D8870;          // void __cdecl gameseq_pop_state() (queued, like Esc)
constexpr size_t kSaveEntrySize = 0xBC;

enum : int { GS_MAINMENU = 0x00, GS_GAMEPLAY = 0x01, GS_SAVE_LOAD_SCREEN = 0x32 };  // RFGR_Types rfg/Game.h

constexpr auto kSettle = std::chrono::milliseconds(1000);
constexpr auto kGiveUp = std::chrono::seconds(240);

bool g_init = false;
bool g_enabled = false;
bool g_newGame = false;  // rfg-vr-autostart.txt says "new": start a new game instead of loading
bool g_done = false;
bool g_selected = false;
bool g_titleKey = false;
clock::time_point g_titlePassedAt{};
bool g_loaded = false;
uintptr_t g_base = 0;
int g_lastState = -2;
clock::time_point g_start, g_stateSince;

template <typename T>
T& at(uintptr_t va) {
    return *reinterpret_cast<T*>(g_base + (va - 0x400000));
}

std::wstring fileNextToDll(const wchar_t* name) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&fileNextToDll), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L"\\/") + 1) + name;
}

// Save time of an entry (year since 2000, month, day, hour, minute, second; see comparator FUN_0089d9f0).
struct SaveTime {
    int v[6];
};
SaveTime entryTime(const uint8_t* e) {
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
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kSteamExeTimestamp) return;
    // mov eax,[esp+4] ; xorps xmm0,xmm0 ; push ebx ; xor ebx,ebx
    static const uint8_t expect[] = {0x8B, 0x44, 0x24, 0x04, 0x0F, 0x57, 0xC0, 0x53, 0x33, 0xDB};
    bool codeOk = memcmp(&at<uint8_t>(kMainMenuSelect), expect, sizeof expect) == 0;

    std::wstring flag = fileNextToDll(L"rfg-vr-autostart.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, flag.c_str(), L"r") == 0 && f) {
        char buf[64] = {};
        fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        g_enabled = codeOk;
        g_newGame = strstr(buf, "new") != nullptr;
    }
    g_start = g_stateSince = clock::now();
    LOG("autostart: %s", !codeOk ? "disabled (main menu handler bytes differ)"
                         : !g_enabled ? "off (no rfg-vr-autostart.txt)"
                         : g_newGame  ? "enabled: new game"
                                      : "enabled: load the most recent save");
}

void startNewGame() {
    reinterpret_cast<void(__cdecl*)(int)>(&at<uint8_t>(kMainMenuSelect))(0);  // "New Game"
    // The handler just cleared these flags; mark the follow-up dialogs as answered so the menu flow
    // starts the game directly (difficulty stays at its current setting).
    at<uint8_t>(kAutosaveShown) = 1;
    at<uint8_t>(kDifficultyDone) = 1;
    at<uint8_t>(kWeaponDone) = 1;
    LOG("autostart: New Game selected");
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

    if (st == GS_GAMEPLAY) {
        g_done = true;
        LOG("autostart: gameplay reached %.1f s after launch", std::chrono::duration<double>(now - g_start).count());
        return;
    }
    if (now - g_start > kGiveUp) {
        g_done = true;
        LOG("autostart: giving up (state %d)", st);
        return;
    }
    if (now - g_stateSince < kSettle) return;

    if (!g_selected && st == GS_MAINMENU) {
        if (at<uint8_t>(kTitlePassed) == 0) {
            auto* input = at<uint8_t*>(kInputSystem);
            if (!g_titleKey && input) {
                g_titleKey = true;
                int assigned = 0;
                for (int i = 0; i < 16; ++i) {
                    uint8_t* slot = input + kInputSlots + i * kInputSlotSize;
                    int state = *reinterpret_cast<int*>(slot);
                    if ((state == 1 || state == 2) && slot[20] == 0xFF) {
                        slot[20] = 0;  // keyboard / mouse -> player 0
                        ++assigned;
                    }
                }
                *reinterpret_cast<int*>(input + kPendingPlayer) = 0xFF;
                *reinterpret_cast<int*>(input + kPendingMode) = 3;
                LOG("autostart: title screen, %d keyboard/mouse device(s) assigned to player 0", assigned);
            }
            return;
        }
        if (g_titlePassedAt == clock::time_point{}) g_titlePassedAt = now;
        if (now - g_titlePassedAt < kAfterTitle) return;
        if (at<int>(kMainMenuList) == -1 || at<int>(kMainMenuAction) != -1 || at<uint8_t>(kMenuBusy) == 1) return;
        g_selected = true;
        if (g_newGame) {
            startNewGame();
        } else {
            reinterpret_cast<void(__cdecl*)(int)>(&at<uint8_t>(kMainMenuSelect))(1);  // "Load Game"
            LOG("autostart: Load Game selected");
        }
        return;
    }

    if (g_selected && !g_loaded && st == GS_SAVE_LOAD_SCREEN) {
        if (at<int>(kSaveLoadMode) != 0) return;
        int count = at<int>(kSaveCount);
        auto* entries = at<uint8_t*>(kSaveEntries);
        if (count <= 0 || !entries) {
            if (now - g_stateSince < std::chrono::seconds(3)) return;
            reinterpret_cast<void(__cdecl*)()>(&at<uint8_t>(kPopState))();  // no saves: back to the menu
            g_done = true;
            LOG("autostart: no saves listed; left the Load Game screen");
            return;
        }
        int best = 0;
        for (int i = 0; i < count && i < 12; ++i) {
            SaveTime t = entryTime(entries + i * kSaveEntrySize);
            LOG("autostart: save %d: %04d-%02d-%02d %02d:%02d:%02d", i, 2000 + t.v[0], t.v[1], t.v[2], t.v[3], t.v[4], t.v[5]);
            if (newer(t, entryTime(entries + best * kSaveEntrySize))) best = i;
        }
        reinterpret_cast<void(__cdecl*)(void*, bool)>(&at<uint8_t>(kLoadSave))(entries + best * kSaveEntrySize, false);
        at<uint8_t>(kSaveLoadDone) = 1;
        g_loaded = true;
        LOG("autostart: loading save %d of %d (the newest)", best, count);
    }
}

}  // namespace rfgvr::autostart
