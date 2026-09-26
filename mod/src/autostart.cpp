#include "autostart.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include "log.h"

namespace rfgvr::autostart {
namespace {

using clock = std::chrono::steady_clock;

constexpr DWORD kSteamExeTimestamp = 0x5B9B718A;

// Steam rfg.exe virtual addresses (image base 0x400000), rebased at runtime.
// FUN_008f75f0 is the main menu's per-frame new-game flow (autosave notice, difficulty dialog,
// start-weapon dialog, then start); 0x8F7C30 is the menu's item-selected handler.
constexpr uintptr_t kGameseqGetState = 0x7BFCF0;   // int __cdecl gameseq_get_state()
constexpr uintptr_t kMainMenuSelect = 0x8F7C30;    // void __cdecl (int item); item 0 = New Game
constexpr uintptr_t kMainMenuList = 0x016631A8;    // int: main menu list handle, -1 until built
constexpr uintptr_t kMainMenuAction = 0x016631C8;  // int: pending main menu action, -1 none
constexpr uintptr_t kMenuBusy = 0x02C03579;        // byte: handler ignores New Game while 1
constexpr uintptr_t kAutosaveShown = 0x02C0356C;   // byte: autosave notice handled
constexpr uintptr_t kDifficultyDone = 0x02C0356F;  // byte: difficulty dialog answered
constexpr uintptr_t kWeaponDone = 0x02C03570;      // byte: start-weapon dialog answered

enum : int { GS_MAINMENU = 0x00, GS_GAMEPLAY = 0x01 };  // game_state (RFGR_Types rfg/Game.h)

constexpr auto kSettle = std::chrono::milliseconds(1000);
constexpr auto kGiveUp = std::chrono::seconds(240);

bool g_init = false;
bool g_enabled = false;
bool g_done = false;
bool g_selected = false;
uintptr_t g_base = 0;
int(__cdecl* g_getState)() = nullptr;
int g_lastState = -2;
clock::time_point g_start, g_stateSince;

template <typename T>
T& at(uintptr_t va) {
    return *reinterpret_cast<T*>(g_base + (va - 0x400000));
}

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

void init() {
    g_init = true;
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kSteamExeTimestamp) return;
    g_getState = reinterpret_cast<int(__cdecl*)()>(&at<uint8_t>(kGameseqGetState));
    // mov eax,[esp+4] ; xorps xmm0,xmm0 ; push ebx ; xor ebx,ebx
    static const uint8_t expect[] = {0x8B, 0x44, 0x24, 0x04, 0x0F, 0x57, 0xC0, 0x53, 0x33, 0xDB};
    bool codeOk = memcmp(&at<uint8_t>(kMainMenuSelect), expect, sizeof expect) == 0;
    g_enabled = codeOk && fileNextToDll(L"rfg-vr-autostart.txt");
    g_start = g_stateSince = clock::now();
    LOG("autostart: %s", !codeOk ? "disabled (main menu handler bytes differ)"
                         : g_enabled ? "enabled by rfg-vr-autostart.txt"
                                     : "off (no rfg-vr-autostart.txt)");
}

}  // namespace

void onPresent() {
    if (!g_init) init();
    if (!g_getState) return;
    auto now = clock::now();

    int st = g_getState();
    if (st != g_lastState) {
        LOG("game state -> %d (0x%X)", st, st);
        g_lastState = st;
        g_stateSince = now;
    }
    if (!g_enabled || g_done) return;

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
    if (g_selected || st != GS_MAINMENU || now - g_stateSince < kSettle) return;
    if (at<int>(kMainMenuList) == -1 || at<int>(kMainMenuAction) != -1 || at<uint8_t>(kMenuBusy) == 1) return;

    reinterpret_cast<void(__cdecl*)(int)>(&at<uint8_t>(kMainMenuSelect))(0);  // select "New Game"
    // The handler just cleared these flags; mark the follow-up dialogs as answered so the menu flow
    // starts the game directly (difficulty stays at its current setting).
    at<uint8_t>(kAutosaveShown) = 1;
    at<uint8_t>(kDifficultyDone) = 1;
    at<uint8_t>(kWeaponDone) = 1;
    g_selected = true;
    LOG("autostart: New Game selected");
}

}  // namespace rfgvr::autostart
