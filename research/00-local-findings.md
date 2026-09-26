# 00 - Local findings (installed game + this PC)

Collected 2026-09-25 by static inspection of the working copy in `game/` (a disposable copy of the Steam install; a clean copy can be recreated from the Steam library). Nothing was modified.

## rfg.exe

| Property | Value | Implication |
|---|---|---|
| Architecture | **PE32, i386 (x86, 32-bit)**, `LARGE_ADDRESS_AWARE` | **The mod DLL must be built for x86.** Every tool, library and vcpkg triplet must be x86 (`x86-windows-static`). ~4 GB address space limits extra VR render targets. SafetyHook/MinHook both support x86. |
| Link timestamp | 2018-09-14 | Re-Mars-tered launch build; addresses should be stable unless Steam updates it. |
| Size | 25.2 MB, sections `.text .rdata .data .tls .rsrc .reloc` | **No `.bind` section → not SteamStub-wrapped.** Steamless is not needed; the exe can go straight into Ghidra. |
| DllCharacteristics | 0x8140 (DYNAMIC_BASE, NX_COMPAT, TERMINAL_SERVER_AWARE) | ASLR on: use RVAs or signatures, not absolute addresses. |
| PDB path string | `...\win32\steam\master\rfg.pdb` | No PDB shipped. The build config was "win32 steam master". |
| Imports | `d3d11.dll`, `D3DCOMPILER_43.dll`, `XINPUT1_3.dll`, `DINPUT8.dll`, `WINMM.dll`, `dbghelp.dll`, `MSVCR100.dll` (VS2010 CRT), `binkw32.dll`, `GfeSDK.dll`, `discord-rpc.dll`, `sw_api.dll`, `steam_api.dll` (delay/dynamic) | D3D11 renderer; **no direct `dxgi.dll` import** (swapchain comes via `D3D11CreateDeviceAndSwapChain`). Candidate proxy DLLs: `dinput8.dll`, `winmm.dll`, `xinput1_3.dll`, `d3d11.dll`. XInput is used for gamepad, so faking an XInput pad is a quick path for early controller input. |
| Stereo remnants | No strings for `stereo`, `Oculus`, `openvr`, `nvapi`, `3D Vision` | Unlike Alien: Isolation (MotherVR), there is no dormant VR code to revive. |
| Useful strings | `rl_camera`, `camera.xtbl`, `camera_view`, `fov_*` (fov_min/max/zoom/multiplier...), Lua event handler strings | The camera is table-driven (`camera.xtbl` in a vpp); the renderer layer is `rl_*`. Good anchors for Ghidra string xrefs. |

## Game folder

- `game/data/`: 157 files: `*.vpp_pc` archives (anims, humans, interface, items, effects, dlc*), `.bik` videos.
- Root: `binkw32.dll`, `discord-rpc.dll`, `GfeSDK.dll` (NVIDIA GeForce Experience highlights), `sw_api.dll`, OpenSSL (`libeay32/ssleay32`), `steam_api.dll`, gamepad configs `dualshock4.padcfg`, `switchprocon.padcfg`.
- Total size ~30 GB.

## VR runtime on this PC

- 64-bit OpenXR active runtime: `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` = `SteamVR\steamxr_win64.json` → `bin\vrclient_x64.dll`
- **32-bit OpenXR active runtime: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` = `SteamVR\steamxr_win32.json` → `bin\vrclient.dll` (present).** SteamVR does expose OpenXR to 32-bit processes, so an in-process x86 OpenXR mod is viable in principle. Verify early with a minimal x86 hello_xr + D3D11 test.
- SteamVR ships only a 64-bit `openxr_loader.dll` (`bin\win64`), so we need our own x86 loader (vcpkg `openxr-loader:x86-windows-static`, or build OpenXR-SDK for Win32).
- OpenVR's 32-bit client (`vrclient.dll`) is also present, which makes OpenVR the fallback path.

## Consequences for the research docs

Docs 01-04 were researched assuming x64. Where they recommend `x64-windows-static`, C++23 on x64, etc., read **x86**. The REFramework/UEVR reference code is x64-only, but the concepts carry over. Watch for x64-only libraries (e.g. anything using x64-only intrinsics or inline hooks assuming 64-bit trampolines).

## xr-probe results (2026-09-25): 32-bit OpenXR validated

`tools/xr-probe` is a standalone x86, LARGE_ADDRESS_AWARE, static-CRT D3D11 OpenXR app (OpenXR-SDK 1.1.63 static loader, built from source via CMake FetchContent). It mirrors the game's process constraints.

| Item | Result |
|---|---|
| Runtime | **SteamVR/OpenXR 2.17.10**, 41 instance extensions, no API layers |
| System | "SteamVR/OpenXR : lighthouse", vendor 0x28DE, orientation+position tracking, max 16 layers |
| Recommended eye size | 2036×2260 per eye (100% SteamVR SS on Index; runtime GPU-benchmarks this) |
| D3D11 | Runtime wants adapter = RTX 3070, min FL 11_0; device created at FL 11_1. (DXGI reports only 3072 MB VRAM in a 32-bit process because of a SIZE_T cap. Cosmetic.) |
| Swapchain formats (runtime order) | 29 R8G8B8A8_UNORM_SRGB, 91 B8G8R8A8_UNORM_SRGB, 2 R32G32B32A32_FLOAT, 10 R16G16B16A16_FLOAT, 24 R10G10B10A2_UNORM, 40 D32_FLOAT, 55 D16_UNORM, 45 D24_UNORM_S8_UINT, 20 D32_FLOAT_S8X24 |
| Swapchain textures | 3 images/eye, **typeless (27 = R8G8B8A8_TYPELESS)**, bind RT+SRV, MISC_SHARED. The RTV must specify the format. Depth formats are offered, so depth submission is possible. |
| Reference space | STAGE available |
| Frame timing | 120 Hz locked (period 8.33 ms). xrWaitFrame blocks ~8 ms, begin ~0.01 ms, **end ~0.25 ms** |
| Session states | shouldRender stays false in SYNCHRONIZED until the HMD is worn (VISIBLE/FOCUSED). Expected; the mod must keep the game running when not rendering. |
| **Address space** | startup 54 MB → xrCreateInstance 101 MB → D3D11 device 326 MB (driver) → session 327 MB → swapchains 328 MB → steady ~359 MB. **OpenXR + session + both eye swapchains cost only ~30-80 MB of VA**, so the 4 GB limit is not a blocker for the runtime itself. |
| Input | Index + simple_controller bindings accepted. Controllers were off during the run, so pose/trigger/haptics are still untested. |

**Conclusion:** plan A (in-process OpenXR, x86) is viable. The OpenVR fallback is no longer needed as a contingency, only for users on SteamVR < 2.17.

## Ghidra verification of the community hook map (2026-09-25)

Ghidra 12.1.4 headless auto-analysis of `rfg.exe` took 576 s (project `re/ghidra/rfg`). `re/ghidra_scripts/VerifyHookMap.java` → `re/ghidra/verify.log`. Image base 0x00400000.

| Symbol (source) | VA | Result |
|---|---|---|
| keen::graphics::beginFrame (Sledge) | 0x00C6ABD0 | function start, 145 B |
| keen::graphics::endFrame (Sledge) | 0x00C70320 | function start, 165 B (reads `[esi+0x2c8]`) |
| set_hud_hidden (Sledge) | 0x00841A90 | function start, counter at 0x02B9DC68 |
| set_fog_enabled (Sledge) | 0x007C2C70 | function start, writes byte 0x0165CBEA |
| gameseq_get_state (Sledge) | 0x007BFCF0 | function start, 250 xrefs |
| get_local_player (Sledge) | 0x00A108D0 | `mov eax,[0x03023874]; ret`, 730 xrefs, so **local player ptr global = 0x03023874** |
| human_teleport_unsafe (Sledge) | 0x00A7C660 | function start |
| human_get_head_pos_orient (RSL1, RVA 0x69B5D0) | 0x00A9B5D0 | function start, 520 B: **old RSL1 offsets are valid on this build** |
| **rl_camera::render_begin** (RSL1, RVA 0x137660) | **0x00537660** | function start, 22 B: loads `[arg+0x478]`, writes `ecx` into `+0x70`, then tail-calls. The per-eye injection point. |
| rfg_camera global | 0x01DE4B50 | 84 xrefs |
| g_multiplayer | 0x02FEB588 | 1476 xrefs |
| g_player_input_disabled / g_mouse_visible | 0x01E2A9B9 / 0x01CE86EA | 4 / 3 xrefs |
| rl_camera RTTI | vftable 0x012C1414 (4 entries: 0x548EB0, 0x4BA9E0, 0x4F5DA0, 0x4F5DB0) | recovered by the PE RTTI analyzer |

Conclusion: the Sledge Steam table and the 2018 RSL1 RVAs all line up with our exe. No PDB needed to proceed.

## Modifications to `game/`

- 2026-09-25: deleted startup videos `data/logo_kaiko.bik`, `data/logo_nordic_hd.bik`, `data/logo_volition_hd.bik`, `data/legal_hd.bik` (skip boot logos). Not yet confirmed that the game boots cleanly without them; if it hangs, restore them from the Steam install.

## Stage-0 test (2026-09-26): dinput8 proxy inside the game

Launched `game\rfg.exe` with the proxy deployed and `game\steam_appid.txt` = 667720 (Steam was running).

- Proxy loaded from the game folder; the system `dinput8.dll` loaded behind it. The game ran normally and reached the main menu.
- PE timestamp matched (`0x5B9B718A`). The IAT hook on `D3D11CreateDevice` fired once: adapter NULL, driver type HARDWARE, flags 0, feature level 11_0 (`0xB000`), RTX 3070.
- `IDXGISwapChain::Present` and `ResizeBuffers` were hooked via a dummy swapchain on the game device. The game swapchain reports the same device pointer, so the vtable is shared as expected.
- Game swapchain: **1280x720, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB (29), 1 buffer, DISCARD, windowed, flags ALLOW_MODE_SWITCH**. No `ResizeBuffers` call during a menu-only run.
- Present is called from the **main thread** (same thread id as DllMain) with `syncInterval 0`; the menu runs at about 250 fps, which looks like an internal cap rather than vsync.
- The game exits cleanly on `WM_CLOSE` (`Process.CloseMainWindow()`), so automated runs can end without killing the process.
- Logger bug found: `_wfopen_s` opens with `_SH_DENYRW`, so the log was unreadable while the game ran. Fixed to `_wfsopen(..., _SH_DENYWR)`.

Files added to `game/` by this step: `dinput8.dll`, `dinput8.pdb`, `steam_appid.txt`; the mod writes `rfg-vr.log` there. Dropping a `rfg-vr-novr.txt` next to the dll disables the VR path (stage 1+).

- 2026-09-26: renamed `game/data/intro_1.bik` to `intro_1.bik.disabled` on request, to skip the intro video on New Game. Rename back to restore it.
- 2026-09-26: renamed `game/data/rfg_cine_00a.bik_xbox2` to `.disabled` (the New Game intro cinematic, requested as RFG_CINE_00A.bik by the new-game code). Rename back to restore it.
- 2026-09-26: both renames reverted. With `rfg_cine_00a.bik_xbox2` missing the game hangs on a black screen in GS_VIDEO_CUTSCENE_PLAY (it checks the file exists, BinkOpen is never called). `intro_1.bik` is not opened on New Game. Video skipping is done in the mod instead (BinkOpen hook).

## RenderDoc attempt (2026-09-26)

- RenderDoc 1.46 (portable, x86 capture via `x86/renderdoccmd.exe`; removed again from `tools/re/`) injects fine, but the game hangs/crashes when a level loads: virtual memory reaches ~4 GB (the LAA 32-bit limit) because of RenderDoc's resource shadowing. In-level captures are not feasible for this 32-bit game.
- RenderDoc re-patches rfg.exe's import table, which silently replaced the mod's IAT hook on D3D11CreateDevice (no Present hook, so no auto-start). The mod now inline-hooks `d3d11!D3D11CreateDevice` instead, which works with or without such tools.
- The mod log is opened with `_SH_DENYWR`, so a second simultaneous game instance cannot write its log.

## Milestone: working 6-DOF stereo (2026-09-26, confirmed in the headset)

How it works (mod/src/camera_hook.cpp, mod/src/xr.cpp):
- **Camera**: hook `FUN_007cfbd0` (main view setup, game thread). Before it runs, write the eye pose into the game camera `rfg_camera` @ VA 0x01DE4B50: `render_pos` (+0x10C), `render_orient` (+0x118, rows r/u/f), `real_fov` (+0xBC, vertical degrees). The function copies those into the main `rl_camera` and calls render_begin, so culling, GPU view and projection all follow. Restore the three fields afterwards so gameplay never sees the head pose. Eye pose = game camera yaw (horizontal forward/right) * HMD orientation (OpenXR RH -> LH: quat (-x,-y,z,w), pos (x,y,-z)); position = game pos + HMD eye position rotated by the yaw.
- **Threads**: camera setup runs on a game thread; the main camera is drawn (render_begin at 0x7CFB2F) and presented on the Present thread one frame later. Images are paired with eyes by matching the rl_camera position at draw time against a ring of recent setups (never by timing).
- **Frame model**: one OpenXR frame spans two game frames (left eye, then right eye), both from the same located views; submitted together when both arrived. Result: 0 stale images, stereo every headset frame; each eye at half the game frame rate.
- **FOV**: engine builds a symmetric projection from `real_fov` + backbuffer aspect; the fov submitted to OpenXR is read back from the rl_camera projection matrix (so it matches exactly). Currently 138 x 112 deg at 16:9.
- Writing an asymmetric projection into rl_camera (+0xC0) or the renderer state has no effect on the final image; the engine rebuilds it.
- Bug that cost hours: `g_main_camera` pointer is at **VA** 0x1DE48E4 (RVA 0x19E48E4); it had been used as an RVA and always read null.

Open issues: resolution is the window backbuffer (1280x720 per eye); HUD is baked into both eyes; half-rate per eye.

## Headset resolution (2026-09-26)

- Every frame `FUN_00c6dd00` reads the game window's client rect and calls the keen swapchain resize `FUN_00c6ab20` (bool __cdecl(RenderSwapChain*, w, h); RenderSwapChain+0x13C = IDXGISwapChain*, +0x140/+0x144 = current size), then beginFrame `FUN_00c6abd0`. The resize only acts on a size change: DXGI ResizeBuffers, then `FUN_00c61e90` recreates RTV and depth from the actual buffer size (DXGI_SWAP_CHAIN_DESC lives at RenderSwapChain+0x140).
- The mod hooks `FUN_00c6ab20` and passes the headset per-eye size instead: SteamVR recommended pixel density (2036x2260 on the Index) spread over the symmetric frustum that covers both eyes' fov (+5% margin) = **2544x2376**.
- Result: 235-239 game fps at 2544x2376 on an RTX 3070, so the 2-game-frames-per-headset-frame scheme delivers 120 Hz stereo (each eye 120 Hz), 0 stale images.

## World scale (2026-09-26)

- Measured in game: player head (`human_get_head_pos_orient`, VA `0xA9B5D0`, cdecl `(human*, vector&, matrix&)`) is 1.58 units above the feet (`object::pos` at +4 of the local player, ptr global `0x03023874`). **1 game unit = 1 m.** The third-person camera sits 2.6 m behind the head, at head height.
- Headset side: eye separation 65.6 mm; the submitted fov matches the engine's projection exactly (image aspect 1.0707 = tangent aspect). The stereo is therefore geometrically 1:1; the "small world" impression in third person is perceptual. `WorldScale` in `rfg-vr.ini` divides the head/eye offset.

## Camera shake (2026-09-26)

- Game option: `DAT_025359bd` = "camera shake disabled" (config key `camera_shake` in section `game`, stored inverted), copied to the runtime flag `DAT_01648859` (1 = enabled) by `FUN_007ce8a0`.
- Shake start `FUN_006c6e00(camera_shake*, float intensity, bool ignore_disabled)` fills one of 5 active slots: `0x01DE4554` (camera_shake*[5]), elapsed `0x01DE4520`, intensity `0x01DE4888`. `FUN_006d4980` (building_stress) and `FUN_006d4340` (shard_impact_nearby/strong) do the same inline. Shakes marked ignore_disabled (weapon fire, kaboom) bypass the option.
- `FUN_006ccdf0` (called from camera update `FUN_006dffa0`) evaluates the slots each frame and multiplies the result into `real_orient` (`0x01DE4BA0`), plus shake blur (`FUN_007f1000`) and pad rumble (`FUN_00567fc0`). The mod hooks it and clears the slots first, which removes every shake including the permanent idle sway `player_standing`.
