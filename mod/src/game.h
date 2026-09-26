#pragma once
// Helpers for the Steam rfg.exe. Engine addresses in the mod are virtual addresses as in the
// disassembly (image base 0x400000); the exe is relocated at runtime.

#include <windows.h>

#include <safetyhook.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace rfgvr::game {

// PE timestamp of the Steam build all addresses belong to (SHA-256 0d52039e...2df4).
constexpr DWORD kSteamTimestamp = 0x5B9B718A;

bool isSteamBuild();

uintptr_t runtime(uintptr_t va);

template <typename T>
T& at(uintptr_t va) {
    return *reinterpret_cast<T*>(runtime(va));
}

// True if the code at `va` + `offset` starts with `bytes` (guards against other builds / patches).
bool codeMatches(uintptr_t va, std::initializer_list<uint8_t> bytes, size_t offset = 0);

// Inline hook on `va` if the code matches; logs and returns an empty hook otherwise.
SafetyHookInline hook(uintptr_t va, std::initializer_list<uint8_t> bytes, void* detour, const char* what,
                      size_t offset = 0);
template <typename Fn>
SafetyHookInline hook(uintptr_t va, std::initializer_list<uint8_t> bytes, Fn* detour, const char* what,
                      size_t offset = 0) {
    return hook(va, bytes, reinterpret_cast<void*>(detour), what, offset);
}
SafetyHookMid hookMid(uintptr_t va, std::initializer_list<uint8_t> bytes, safetyhook::MidHookFn fn,
                      const char* what);

// Path of a file in the mod's folder (next to dinput8.dll).
std::wstring pathNextToDll(const wchar_t* name);

}  // namespace rfgvr::game
