# rfg-vr mod

An x86 `dinput8.dll` proxy for the Steam build of Red Faction Guerrilla Re-Mars-tered. It forwards
DirectInput to the system DLL and adds VR through OpenXR. Features, install and settings are in
the [main README](../README.md).

## How it works

- **Stereo.** The engine's main view setup is hooked to render the main camera from the current
  eye pose. One OpenXR frame spans two game frames (left eye, then right eye) from the same head
  pose; the images are matched to their eye by camera position and submitted together.
- **Resolution.** The engine's per-frame swapchain resize is hooked, so the game renders at a
  symmetric frustum covering both eyes at SteamVR's pixel density, whatever the window size.
- **Head aim.** After each camera update the camera orientation is set to game yaw × head
  orientation; aiming uses it, rendering uses the game's own orientation. With head aim the on-foot
  and vehicle cameras get no vertical look input.
- **HUD.** The engine's UI pass is redirected into a transparent texture, shown as an OpenXR quad.
- **Screen.** Outside gameplay (menus, videos, loading) the game's flat image is shown on a quad.
- **Mouse.** Menu pointer coordinates are scaled to the render size; the cursor is kept in the
  window while the headset shows the game.

Engine addresses are for the Steam `rfg.exe` (PE timestamp `0x5B9B718A`); on other builds the
engine hooks stay off, and each hook checks the code it patches. How every address was found is in
[research/00-local-findings.md](../research/00-local-findings.md).

## Source files

| File | Purpose |
|---|---|
| `dllmain.cpp`, `dinput8.def` | Proxy exports, hook installation |
| `game.cpp` | Address translation, byte-checked hooks, paths |
| `d3d11_hook.cpp` | `D3D11CreateDevice` and `Present` hooks, fps/address-space log |
| `camera_hook.cpp` | Eye rendering, render size, head aim, pitch lock, camera shake |
| `xr.cpp` | OpenXR session, frame loop, eye / HUD / screen layers |
| `hud.cpp` | UI pass capture |
| `mouse.cpp` | Menu mouse scaling, cursor confinement |
| `gamestate.cpp` | The game's state (gameplay vs. menus) |
| `config.cpp` | `rfg-vr.ini`, reloaded when it changes |
| `vrmath.h` | Pose conversion between OpenXR (right-handed) and the game (left-handed) |
| `log.cpp` | Log file |
| `autostart.cpp`, `video_hook.cpp`, `iat.cpp` | Dev build only, see below |

## Build

Visual Studio 2022 and CMake 3.24 or newer; SafetyHook 0.7.0 and the OpenXR SDK 1.1.63 static
loader are fetched automatically.

    cmake -S . -B build -A Win32 [-DRFGVR_DEV=ON] [-DRFGVR_DEPLOY=ON]
    cmake --build build --config Release

- `-DRFGVR_DEPLOY=ON` copies `dinput8.dll` and its `.pdb` into `../game/`.
- `-DRFGVR_DEV=ON` adds developer tools. With `rfg-vr-autostart.txt` next to the DLL, the game
  passes the title screen and loads the newest save by itself; if the file contains `new`, it
  starts a new game instead and skips its intro video. Release builds contain none of this.
- `rfg-vr-novr.txt` next to the DLL disables VR (the hooks stay passive).

To launch `rfg.exe` directly, outside Steam, the game folder needs a `steam_appid.txt` containing
`667720`.

## Log

`rfg-vr.log` next to the DLL (`rfg-vr.<pid>.log` if locked): hook status, OpenXR session and
frame statistics, game-state and camera-mode changes, and every 10 s the fps and the process's
address-space use.
