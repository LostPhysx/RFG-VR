# rfg-vr

A VR mod for **Red Faction Guerrilla Re-Mars-tered** (Steam) using OpenXR: stereo 3D with 6-DOF
head tracking.

> [!WARNING]
> **AI-generated content.** The code and documentation of this project are 100% AI-generated
> (Claude Code by Anthropic). Despite thorough testing, you use this mod at your own risk.

**Third person only.** The game is played in its normal third-person view, seen in stereo 3D, with
mouse/keyboard or gamepad. Motion controllers and a first-person mode are not implemented yet.

## Features

- Stereo 3D with head tracking (rotation and position) at SteamVR's resolution.
- Head aim: the game aims where you look; mouse or stick turn the camera around the character.
- The HUD on a panel in front of you; menus, videos and loading screens on a virtual screen.
- No camera shake (optional).

## Requirements

- The **Steam** version of Red Faction Guerrilla Re-Mars-tered. Other builds (e.g. GOG) are
  detected and left unmodified.
- **SteamVR 2.17 or newer**, set as the OpenXR runtime (SteamVR settings → OpenXR). It provides
  the 32-bit OpenXR runtime the game needs.
- A PC VR headset. Tested with a Valve Index.

## Install

1. Download `rfg-vr-<version>.zip` from the
   [releases](https://github.com/LostPhysx/RFG-VR/releases) page.
2. Copy `dinput8.dll` and `rfg-vr.ini` into the game folder, next to `rfg.exe`
   (Steam: right-click the game → Manage → Browse local files).
3. Start SteamVR, then the game. Put the headset on; recenter from the SteamVR dashboard if needed.

To uninstall, delete `dinput8.dll` (and `rfg-vr.ini`). The mod writes `rfg-vr.log` next to it.

## Settings

`rfg-vr.ini`, section `[VR]`. Changes apply within a second, even while the game runs.

| Setting | Default | Meaning |
|---|---|---|
| `HeadAim` | 1 | 1: aim with your head (the camera only turns horizontally with mouse/stick, the HUD follows your head). 0: aim with the mouse. On the flat screen the mouse always aims. |
| `WorldScale` | 1.0 | Apparent size of the world. 1 is life-size (the game is in metres); 1.5 makes everything look 1.5× bigger. |
| `CameraShake` | 0 | 1 keeps the game's camera shake. |
| `HudLayer` | 1 | 0 draws the HUD into the 3D view instead of a separate panel. |
| `HudDistance`, `HudWidth` | 2.0, 2.4 | Distance and width of the HUD panel in metres. |

## Known limitations

- Third person only; no motion controllers.
- Thrown charges land less precisely than bullets with head aim (not perfectly precise with mouse
  aim either).
- The game is 32-bit and runs close to its 4 GB address-space limit.
- While the HUD panel is active, the desktop window shows the 3D view without the UI.

## Repository

| Path | Contents |
|---|---|
| `mod/` | The mod (x86 `dinput8.dll` proxy); see [mod/README.md](mod/README.md) for building |
| `research/` | Reverse-engineering log (`00-local-findings.md`) and background research |
| `re/` | Ghidra headless scripts used to find and verify engine addresses |
| `tools/xr-probe/` | Standalone x86 OpenXR + D3D11 test app |
