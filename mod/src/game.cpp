#include "game.h"

#include <cstring>

#include "log.h"

namespace rfgvr::game {

bool isSteamBuild() {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == kSteamTimestamp;
}

uintptr_t runtime(uintptr_t va) {
    static const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base + (va - 0x400000);
}

bool codeMatches(uintptr_t va, std::initializer_list<uint8_t> bytes, size_t offset) {
    return memcmp(reinterpret_cast<const void*>(runtime(va) + offset), bytes.begin(), bytes.size()) == 0;
}

SafetyHookInline hook(uintptr_t va, std::initializer_list<uint8_t> bytes, void* detour, const char* what,
                      size_t offset) {
    if (!codeMatches(va, bytes, offset)) {
        LOG("%s: code at %p differs from the Steam build; not hooked", what, reinterpret_cast<void*>(runtime(va)));
        return {};
    }
    return safetyhook::create_inline(reinterpret_cast<void*>(runtime(va)), detour);
}

SafetyHookMid hookMid(uintptr_t va, std::initializer_list<uint8_t> bytes, safetyhook::MidHookFn fn,
                      const char* what) {
    if (!codeMatches(va, bytes)) {
        LOG("%s: code at %p differs from the Steam build; not hooked", what, reinterpret_cast<void*>(runtime(va)));
        return {};
    }
    return safetyhook::create_mid(reinterpret_cast<void*>(runtime(va)), fn);
}

std::wstring pathNextToDll(const wchar_t* name) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&pathNextToDll), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L"\\/") + 1) + name;
}

}  // namespace rfgvr::game
