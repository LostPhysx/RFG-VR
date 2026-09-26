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
  headset's per-eye size whatever the window size: SteamVR's recommended pixel density over a
  symmetric view that covers both eyes (about 2520×2360 on the Index).
- **Head aim.** After each camera update the game camera's orientation is set to its own heading
  combined with the head orientation, so shooting, throwing and the crosshair follow the headset.
  The camera orbit itself stays under mouse/stick control.
- **HUD panel.** During gameplay the engine's UI pass (HUD, crosshair, subtitles, notifications) is
  drawn into a separate transparent texture instead of the eye images. The headset shows it as a
  flat panel in front of the player, locked to the head with head aim so the crosshair marks the
  aim.
- **Menus and loading.** Outside gameplay (main menu, pause, map, options, death screen, loading)
  the game renders its normal flat view with the menu, shown on a fixed screen in front of the
  player.
- **Comfort.** Camera shake and the third-person camera's vertical look input are removed (see
  the settings below).

Engine addresses are for the Steam `rfg.exe` (PE timestamp `0x5B9B718A`). On any other build the
engine hooks stay off. How the addresses were found is written up in
`../research/00-local-findings.md`.

## Source files

| File | Purpose |
|---|---|
| `dllmain.cpp`, `dinput8.def` | Proxy exports, build check, hook installation |
| `d3d11_hook.cpp` | `D3D11CreateDevice` hook, then `Present`/`ResizeBuffers` hooks through a dummy swapchain; logs fps and address space |
| `camera_hook.cpp` | Engine hooks: main view setup, `rl_camera::render_begin`, swapchain resize, camera update (head aim), camera shake, third-person camera (pitch lock) |
| `hud.cpp` | UI pass capture into the HUD texture |
| `xr.cpp` | OpenXR session, frame loop, eye, HUD and screen layers, render size |
| `gamestate.cpp` | The game's state (gameplay vs. menus), read once per frame |
| `vrmath.h` | Pose conversion between OpenXR (right-handed) and the game (left-handed) |
| `autostart.cpp`, `video_hook.cpp` | Dev convenience: get past the title screen, load the newest save (or start a new game), skip the intro cinematic |
| `config.cpp` | `rfg-vr.ini` settings, reloaded when the file changes |
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

Settings go in `rfg-vr.ini` next to the DLL. Saved changes apply within a second, even while the
game is running.

    [VR]
    ; Apparent size of the world: 1 = life-size, 1.5 = everything looks 1.5x bigger.
    WorldScale=1.0
    ; 1 = keep the game's camera shake (explosions, hammer hits), 0 = no shake.
    CameraShake=0
    ; 1 = mouse/stick only turn the camera around the player; look up and down with the headset.
    LockCameraPitch=1
    ; 1 = the game aims where you look (UI panel follows your head), 0 = mouse aim.
    HeadAim=1
    ; 1 = in-game UI on a separate panel in front of you, 0 = drawn into the 3D view.
    HudLayer=1
    ; Distance (metres) and width (metres) of the UI panel.
    HudDistance=2.0
    HudWidth=2.4

The game world is in metres, so `WorldScale=1` is geometrically correct. From the third-person
camera a larger value can still feel better, because it shrinks eye separation and head movement
together.

Camera shake is off by default. That covers every shake, including the constant idle sway while
standing, and the controller rumble and blur that come with them; the shake sounds still play.

`LockCameraPitch=1` (the default) keeps the on-foot camera at a level orbit. The mouse and stick
only turn it around the character, and up and down comes from the headset. With `HeadAim=1` the
game aims where you look; with `HeadAim=0` it aims along the camera, so shots stay level while the
pitch is locked. Vehicle and turret cameras aren't affected by the pitch lock.

Optional flag files next to the DLL:

| File | Effect |
|---|---|
| `rfg-vr-novr.txt` | Disable VR; the hooks stay passive |
| `rfg-vr-autostart.txt` | Get past the title screen and load the newest save, skipping the intro cinematic. If the file contains `new`, start a new game instead (its autosave can overwrite progress) |

For launching `rfg.exe` directly, outside Steam, the game folder also needs a `steam_appid.txt`
containing `667720`.

## Known limitations

- There is no motion-controller input yet. Play with mouse and keyboard or a gamepad.
- Thrown charges land less precisely than bullets with head aim (they are not perfectly precise
  with mouse aim either).
- The game pauses when its window loses focus (e.g. the SteamVR dashboard); click the window to
  resume.
- While the HUD panel is active, the desktop window shows the 3D view without the UI.
- The desktop window shows the eye image squeezed to the window's aspect ratio.
- Menus render at the eye resolution, so mouse positions in menus may not match the pointer.
- The process runs close to the 32-bit address-space limit (about 3.1 GB used, see the log).
