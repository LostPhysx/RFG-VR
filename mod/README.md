# rfg-vr mod

An x86 `dinput8.dll` proxy for Red Faction Guerrilla Re-Mars-tered (Steam build). It forwards
DirectInput to the system DLL and adds 6-DOF stereo VR through OpenXR (tested with SteamVR and a
Valve Index).

## What it does

- **Stereo camera.** It hooks the engine's main view setup and renders the main camera from the
  current eye pose. One OpenXR frame spans two game frames: the left eye, then the right eye, both
  from the same head pose. The two images are submitted together.
- **Eye pairing.** The camera is set up on a game thread and drawn one frame later on the Present
  thread. Each image is matched to its eye by the camera position that eye's setup produced.
- **Resolution.** It hooks the engine's per-frame swapchain resize. The game then renders at the
  headset's per-eye size (2544×2376 on the Index), whatever the window size.
- **Menus and loading.** While no 3D view renders, the game image is shown on a flat screen in
  front of the player.

Engine addresses are for the Steam `rfg.exe` (PE timestamp `0x5B9B718A`). On any other build the
engine hooks stay off. How the addresses were found is written up in
`../research/00-local-findings.md`.

## Source files

| File | Purpose |
|---|---|
| `dllmain.cpp`, `dinput8.def` | Proxy exports, build check, hook installation |
| `d3d11_hook.cpp` | `D3D11CreateDevice` hook, then `Present`/`ResizeBuffers` hooks through a dummy swapchain |
| `camera_hook.cpp` | Engine hooks: main view setup, `rl_camera::render_begin`, swapchain resize |
| `xr.cpp` | OpenXR session, frame loop, eye and screen layers, render size |
| `vrmath.h` | Pose conversion between OpenXR (right-handed) and the game (left-handed) |
| `autostart.cpp`, `video_hook.cpp` | Dev convenience: start a new game and skip the intro cinematic |
| `log.cpp`, `iat.cpp` | Log file, import-table hook |

## Build

This needs Visual Studio 2022 and CMake 3.24 or newer. The dependencies (SafetyHook 0.7.0 and the
OpenXR SDK 1.1.63 static loader) are fetched automatically.

    cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 [-DRFGVR_DEPLOY=ON]
    cmake --build build --config Release

`-DRFGVR_DEPLOY=ON` copies `dinput8.dll` and its `.pdb` into `../game/`.

## Use

1. Copy `dinput8.dll` into the game folder, next to `rfg.exe`.
2. Start SteamVR, then start the game. Put the headset on: the view switches to VR once the game
   renders 3D.
3. Recenter through the SteamVR dashboard if needed.

To uninstall, delete `dinput8.dll`. The log is written to `rfg-vr.log` next to the DLL, or to
`rfg-vr.<pid>.log` if that file is locked.

Optional flag files next to the DLL:

| File | Effect |
|---|---|
| `rfg-vr-novr.txt` | Disable VR; the hooks stay passive |
| `rfg-vr-autostart.txt` | Start a new game straight from the main menu and skip the intro cinematic |

For launching `rfg.exe` directly, outside Steam, the game folder also needs a `steam_appid.txt`
containing `667720`.

## Known limitations

- The HUD is rendered into both eyes at screen depth.
- There is no motion-controller input yet. Play with mouse and keyboard or a gamepad.
- The desktop window shows the eye image squeezed to the window's aspect ratio.
- Menus render at the eye resolution, so mouse positions in menus may not match the pointer.
