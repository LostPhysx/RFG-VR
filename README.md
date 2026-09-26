# rfg-vr

A 6-DOF VR mod for **Red Faction Guerrilla Re-Mars-tered** (Steam) using OpenXR, built as an
injected DLL in the flat2vr style.

**Status: early prototype.** Third person is playable: stereo 3D with head tracking (rotation and
position) at the headset's native resolution, 120 Hz on a Valve Index with an RTX 3070, head aim,
the HUD on a panel in front of the player and menus on a virtual screen. Motion controllers and
first-person play are not done yet.

## Layout

| Path | Contents |
|---|---|
| `mod/` | The mod (x86 `dinput8.dll` proxy). See `mod/README.md` for building and use. |
| `research/` | Collected modding, OpenXR and flat2vr research, plus local reverse-engineering findings (`00-local-findings.md`) |
| `re/` | Ghidra headless scripts and launchers used to find and verify engine addresses |
| `tools/xr-probe/` | Standalone x86 OpenXR + D3D11 test app (validates SteamVR's 32-bit OpenXR path) |

These are not in the repository:

| Path | Contents |
|---|---|
| `game/` | A disposable copy of the game install for testing |
| `tools/re/` | Portable Ghidra and JDK |
| `re/ghidra/rfg.*` | The Ghidra project; regenerate it with `re/ghidra/run-analyze.cmd` |
| `research/downloads/` | Third-party reference code (licenses vary, some unlicensed) |

## Requirements

- The Steam version of the game. `rfg.exe` is 32-bit, so the mod is built for Win32.
- SteamVR 2.17 or newer, which provides the 32-bit OpenXR runtime.
- Visual Studio 2022 and CMake 3.24 or newer.
