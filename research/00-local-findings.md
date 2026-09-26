# 00 - Local findings

Engineering log of the reverse engineering and measurements behind the mod: the Steam `rfg.exe`, this PC's VR runtime, and what each engine hook is based on. Addresses are virtual addresses as in Ghidra (image base 0x00400000); the exe is ASLR-relocated at runtime.

## rfg.exe

| Property | Value | Implication |
|---|---|---|
| Architecture | PE32 (x86), `LARGE_ADDRESS_AWARE` | The mod is x86; the process has a 4 GB address space. |
| Link timestamp | 2018-09-14 (`0x5B9B718A`), SHA-256 `0d52039e...2df4` | The Steam build; the mod checks the timestamp before hooking. |
| Sections | `.text .rdata .data .tls .rsrc .reloc`, no `.bind` | Not SteamStub-wrapped; loads straight into Ghidra. |
| PDB | none shipped (path string `...\win32\steam\master\rfg.pdb`) | Everything is found by static analysis. |
| Imports | `d3d11`, `DINPUT8`, `XINPUT1_3`, `WINMM`, `binkw32`, `steam_api`, `MSVCR100`, ... (no `dxgi`) | `dinput8.dll` works as the proxy DLL. |
| Stereo remnants | none (`stereo`, `openvr`, `Oculus`, `3D Vision`) | No dormant VR code. |

## VR runtime

- SteamVR 2.17+ registers a 32-bit OpenXR runtime: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` = `SteamVR\steamxr_win32.json` → `bin\vrclient.dll`. SteamVR ships only a 64-bit loader, so the mod links the Khronos loader statically (x86).
- `tools/xr-probe` (standalone x86 D3D11 OpenXR app, same process constraints as the game) validated it on 2026-09-25:

| Item | Result |
|---|---|
| Runtime | SteamVR/OpenXR 2.17.10, "lighthouse" system, max 16 layers |
| Recommended eye size | about 2016-2036 × 2240-2260 on the Index at 100% (SteamVR benchmarks the GPU) |
| Swapchain formats | 29 R8G8B8A8_UNORM_SRGB first; images are typeless (27), so RTVs must name the format |
| Frame timing | 120 Hz; xrWaitFrame blocks ~8 ms, xrEndFrame ~0.25 ms |
| Session | shouldRender stays false until the headset is worn |
| Address space | OpenXR, session and eye swapchains cost only ~30-80 MB |

## Community addresses verified in Ghidra (2026-09-25)

`re/ghidra_scripts/VerifyHookMap.java` checked the Sledge Steam table and the 2018 RSL1 offsets against this exe; all match:

| Symbol (source) | VA | Result |
|---|---|---|
| keen::graphics::beginFrame / endFrame (Sledge) | 0x00C6ABD0 / 0x00C70320 | function starts |
| set_hud_hidden (Sledge) | 0x00841A90 | function start |
| gameseq_get_state (Sledge) | 0x007BFCF0 | function start |
| get_local_player (Sledge) | 0x00A108D0 | `mov eax,[0x03023874]; ret`: local player pointer at 0x03023874 |
| human_get_head_pos_orient (RSL1) | 0x00A9B5D0 | function start |
| rl_camera::render_begin (RSL1) | 0x00537660 | function start |
| rfg_camera global | 0x01DE4B50 | 84 xrefs |
| g_multiplayer | 0x02FEB588 | 1476 xrefs |

## Proxy and D3D11 (2026-09-26)

- The `dinput8.dll` proxy loads from the game folder and forwards to the system DLL. For launching `rfg.exe` outside Steam, `steam_appid.txt` (667720) is needed.
- Game swapchain: 1280x720 window, `R8G8B8A8_UNORM_SRGB`, 1 buffer, DISCARD, windowed. Present runs on the main thread with sync interval 0.
- `IDXGISwapChain::Present` is hooked through a dummy swapchain on the game's device (shared vtable). `D3D11CreateDevice` is inline-hooked on the export: tools like RenderDoc re-patch the import table.
- RenderDoc is not usable: its resource shadowing pushes the 32-bit process to the 4 GB limit when a level loads.
- The New Game intro video (`rfg_cine_00a.bik_xbox2`) must exist; without it the game waits in GS_VIDEO_CUTSCENE_PLAY on a black screen. The dev build skips it with BinkGoto instead.

## Stereo rendering (2026-09-26)

- **Camera:** the main view setup `FUN_007cfbd0` (game thread) copies the game camera's `render_pos` (+0x10C), `render_orient` (+0x118, rows right/up/forward) and `real_fov` (+0xBC, vertical degrees) into the main `rl_camera` and calls render_begin, so culling, view and projection all follow them. The mod writes the eye pose there before the call and restores the fields afterwards. Eye pose = game camera yaw × HMD orientation (OpenXR RH → LH: quaternion (-x,-y,z,w), position (x,y,-z)); position = camera position + eye offset rotated by the yaw.
- **Threads:** the view is set up on the game thread and drawn and presented on the Present thread one frame later (render_begin call at 0x7CFB2F). Images are matched to eyes by the rl_camera position at draw time against a ring of recent setups.
- **Frame model:** one OpenXR frame spans two game frames (left eye, then right eye) from the same located views, submitted together.
- **FOV:** the engine rebuilds a symmetric projection from `real_fov` and the target aspect; writing a projection into the rl_camera has no effect. The submitted FOV is read back from the rl_camera projection (+0xC0), so it matches exactly.
- The main camera pointer is at VA 0x1DE48E4 (a VA, not an RVA).

## Headset resolution (2026-09-26)

- Every frame `FUN_00c6dd00` reads the game window's client rect and calls the keen swapchain resize `FUN_00c6ab20` (bool __cdecl(RenderSwapChain*, w, h); RenderSwapChain+0x13C = IDXGISwapChain*, +0x140/+0x144 = current size), then beginFrame `FUN_00c6abd0`. The resize only acts on a size change: DXGI ResizeBuffers, then `FUN_00c61e90` recreates RTV and depth from the actual buffer size (DXGI_SWAP_CHAIN_DESC lives at RenderSwapChain+0x140).
- The mod hooks `FUN_00c6ab20` and passes the headset per-eye size instead: SteamVR's recommended pixel density spread over the symmetric frustum that covers both eyes' fov (+5% margin), e.g. 2520x2356 for a recommended 2016x2240 on the Index.
- Result: about 240 game fps on an RTX 3070, so two game frames per headset frame give 120 Hz stereo.

## World scale (2026-09-26)

- Measured in game: player head (`human_get_head_pos_orient`, VA `0xA9B5D0`, cdecl `(human*, vector&, matrix&)`) is 1.58 units above the feet (`object::pos` at +4 of the local player, ptr global `0x03023874`). **1 game unit = 1 m.** The third-person camera sits 2.6 m behind the head, at head height.
- Headset side: eye separation 65.6 mm; the submitted fov matches the engine's projection exactly (image aspect 1.0707 = tangent aspect). The stereo is therefore geometrically 1:1; the "small world" impression in third person is perceptual. `WorldScale` in `rfg-vr.ini` divides the head/eye offset.

## Camera shake (2026-09-26)

- Game option: `DAT_025359bd` = "camera shake disabled" (config key `camera_shake` in section `game`, stored inverted), copied to the runtime flag `DAT_01648859` (1 = enabled) by `FUN_007ce8a0`.
- Shake start `FUN_006c6e00(camera_shake*, float intensity, bool ignore_disabled)` fills one of 5 active slots: `0x01DE4554` (camera_shake*[5]), elapsed `0x01DE4520`, intensity `0x01DE4888`. `FUN_006d4980` (building_stress) and `FUN_006d4340` (shard_impact_nearby/strong) do the same inline. Shakes marked ignore_disabled (weapon fire, kaboom) bypass the option.
- `FUN_006ccdf0` (called from camera update `FUN_006dffa0`) evaluates the slots each frame and multiplies the result into `real_orient` (`0x01DE4BA0`), plus shake blur (`FUN_007f1000`) and pad rumble (`FUN_00567fc0`). The mod hooks it and clears the slots first, which removes every shake including the permanent idle sway `player_standing`.

## Camera pitch lock (2026-09-26)

- The on-foot camera is **`CAMERA_THIRD_PERSON_MODE` (10)**, confirmed by logging `rfg_camera::mode` (`0x01DE4B50`) in gameplay, not the free mode (0). The camera update `FUN_006dffa0` calls the per-mode update from the table at `0x012CFC84` (5 pointers per mode; update at +0xC): free 0 → `0x6D9780`, lookaround 7 → `0x6D3D50`, first person 8 → `0x6CAA00`, turret 9 → `0x6DEA50`, third person 10 → `0x6DD250`, satellite 12 → `0x6DA7A0`.
- Third-person update `FUN_006dd250` (void, cdecl) orbits the player at the angles in `lookaround_mode_params`: **pitch `0x01DE4D70`**, heading `0x01DE4D7C` (radians, rfg_camera +0x220 / +0x22C). Look input accumulates into them before the update runs. With head aim in VR the mod sets pitch to 0 at the entry of `FUN_006dd250`, which gives a level orbit; only one frame's input (~0.004 rad) remains.
- Dead ends, for the record: `FUN_006cafb0` / `FUN_006cb1a0` (heading/pitch input into free_mode_params `user_rot` 0x01DE4CB0 / `user_elev` 0x01DE4CC0), `FUN_00a43150` and `FUN_00a21470` (turret-style look motors) are **not called** for on-foot mouse look (0 calls measured while moving the mouse). free_mode_params starts at rfg_camera+0x13C (`0x01DE4C8C`).
- Input options: `pitch_sensitivity` `0x025359A0` → runtime `0x016481D8`, `heading_sensitivity` `0x025359A4` → `0x016481DC`, `mouse_sensitivity` `0x025359A8` → `0x01518A30` (one value for both mouse axes).

## HUD capture (2026-09-26)

- Found with a one-frame D3D11 trace (temporary tool, since removed: hooks on the immediate context's OMSetRenderTargets / draws / clears / PSSetShader / OMSetBlendState, plus the caller of each draw). The UI is about 40 alpha-blended 4-vertex quads drawn onto the backbuffer near the end of the frame, all from the immediate-primitive helper `FUN_0052d690`, called only by `FUN_00543b60` during the frame.
- Chain: `FUN_007cf730` (main view render, Present thread) → render_begin, 3D world (`FUN_004c74c0`), `FUN_004f0f30`, **`FUN_00550830` (int __thiscall(queue, renderer): the frame's 2D primitive queue = the whole in-game UI)** → `FUN_0054c9f0` → `FUN_00543b60` → `FUN_0052d690`. Menus without a 3D view (main menu, loading) draw their UI elsewhere (`FUN_00550480`).
- Much of the frame is recorded by a second thread into a deferred context (its draws had no render-target changes of their own); the UI quads are drawn by the Present thread on the immediate context.
- The mod hooks `FUN_00550830`: while it runs, OMSetRenderTargets with the backbuffer is redirected to a transparent texture of the same size/format, and each blend state is cloned with the alpha channel set to (ONE, INV_SRC_ALPHA) so the result is premultiplied RGBA. Pitfall: draws with a colour write mask of 0 (stencil masks that clip subtitle and notification text) must keep writing nothing; forcing alpha writes on them made the whole HUD area opaque black.
- The texture goes to an OpenXR quad layer (`BLEND_TEXTURE_SOURCE_ALPHA`), in VIEW space with head aim.

## Head aim (2026-09-26)

- Camera update `FUN_006dffa0` (void, game thread) runs the per-mode update and sets real_pos/real_orient (rfg_camera +0x2C / +0x50 = `0x01DE4B7C` / `0x01DE4BA0`) from the ideal values, then applies shake. Aiming, throwing and the crosshair ray use real_orient: the mod sets it to yaw(game) × head orientation after each update and restores the game's own value before the next one; rendering uses the saved game orientation so the head is not applied twice.
- A 1 Hz probe of head-aim yaw vs. the player object's facing (object +0x10 orientation) showed the character turns toward the head-aim direction when shooting or throwing. Guns hit where the crosshair is; thrown charges land less precisely, which is accepted (they are not perfectly precise with mouse aim either).

## Game states and menus (2026-09-26)

- `gameseq_get_state` (`0x7BFCF0`) values seen in play (RFGR_Types rfg/Game.h): 0 main menu, 1 gameplay, 2 load, 3 boot, 0x0F in-game options, 0x10 death options, 0x25 map, 0x2B handbook, 0x2E weapon cabinet, 0x31 video cutscene, 0x32 save/load screen, 0x37 verify savegame, 0x3E quick pause.
- The mod runs stereo, head aim and HUD capture only in state 1; every other state shows the game's flat image on the virtual screen.
- State push is `FUN_007d87c0(state, a, b)`, state pop (Esc-like, queued) is `FUN_007d8870()`. Quick pause (0x3E) is entered when the window loses focus.

## Off-centre projection attempt (2026-09-26, reverted)

- Goal: render each eye's exact asymmetric FOV instead of the symmetric frustum covering it (~25% fewer pixels at the same density: 2016×2240 vs 2520×2356).
- `FUN_00520d70` (rl_camera perspective setup, __thiscall(rl_camera*, renderer)) writes the symmetric projection to rl_camera+0xC0, copies view and projection into the renderer state (renderer+0x478 → +0x400 / +0x440), combines them (`FUN_004d24d0`, `FUN_004f2500`) and builds the culling frustum (`FUN_004e5ea0`). Patching the projection after the call caused double vision (the GPU already had the symmetric matrices). Patching it in a mid hook at `0x52112A` (EBX = &proj, before the copy) gave correct stereo, but the terrain turned orange while the sky stayed normal: a fog/atmosphere pass evidently assumes a centred projection. Reverted; fixing it would need shader patching.

## Address space (2026-09-26)

- The Present log now includes address space used and the largest free block. Typical: 3.06 GB used at the main menu, 3.13-3.18 GB in gameplay, largest free block 880-1000 MB.
- Two loads did not complete this session: one froze after the mission cutscene (Present stopped; the Present and game threads waited in game code on a busy worker thread, no mod frames on their stacks), one stayed in GS_VERIFY_SAVEGAME with Present running. Cause unknown; the memory log will show whether address space is involved if it recurs.

## Autostart: title screen and save loading (2026-09-26, dev builds only)

- Title screen ("please press any key or button") is part of GS_MAINMENU. The main menu update at `0x913910` waits while byte `0x02C03571` is 0 until an input device belongs to player 0 (`FUN_00c748b0(input, 0)` scans the input system's 16 device slots at input+0x11AC, 24 bytes each: int state (1 keyboard, 2 mouse), byte player at +20, 0xFF = none). Then it runs the title sequence itself: sets the flag, hides the title (`0x02C0353E` = 0), starts the profile/session that reads the saves.
- The assignment happens in addInputEvent `FUN_00c7acf0`: input from an unowned slot while an assignment is pending (input+0x132C mode ≠ 3, input+0x1330 player) gives that slot and every keyboard/mouse slot to the pending player and marks it done (mode 3, player 0xFF). Input system pointer: `0x01CE86C0`. `FUN_00c74720(input, player, mode)` only registers a pending request (mode 3 means "done", so calling it with 3 disables assignment).
- Autostart assigns the keyboard/mouse slots to player 0 and marks the assignment done, then waits 2 s before using the menu. Skipping the title (calling the menu handler directly) leaves no player, and the Load Game screen stays empty ("No saved games"); rebuilding the list, reopening the screen or setting the active user (`0x01E2A9CC`, setter `FUN_0094a6a0`) did not help.
- Main menu items (`FUN_008f7e40`, handler `0x8F7C30`): 0 New Game, 1 Load Game (→ `FUN_0089e310` → state 0x32), 2 Wrecking Crew, 3 multiplayer, 4 Bonus Campaign (DLC), then Options, Back, Exit.
- Save/load screen: `FUN_0089ddb0` builds the list once when the screen opens (entries at `*0x02C08118`, 0xBC bytes each, count `0x02C0811C`, mode `0x02C0812C` 0 = load), sorted by `FUN_0089d9f0`: grouped by save type, then newest first (time fields +0x1C year since 2000, +0x18 month, +0x14 day, +0x20 hour, +0x24 minute, +0x28 second). Loading: `FUN_007e7650(entry, 0)` (→ GS_VERIFY_SAVEGAME 0x37) and `0x02C08108` = 1. Autostart picks the newest entry by date.
- All saves live in Steam Cloud `userdata\<id>\667720\remote\autocloud\save\keen_savegame_0_0.sav` (one 31 MB file holding the slots).

## Mouse and cursor (2026-09-26)

- Input is Raw Input plus window messages. The engine subclasses the game window with `FUN_00c7d070` (LRESULT __stdcall wndproc), which passes messages to `FUN_00c7cb70`. With the cursor visible (menus) WM_MOUSEMOVE client coordinates become the menu pointer position; the menus are laid out for the backbuffer, which the mod enlarges to the headset eye size, so the mod scales mouse-message coordinates by render size / client size outside gameplay.
- In gameplay the cursor is hidden and recentred in the window (`FUN_00c75430`, SetCursorPos to the client centre); look input is the offset from that centre. Scaling coordinates there makes the camera spin, so gameplay messages pass through unchanged.
- Cursor mode: `FUN_00c75320(input, show, clip)` hides the cursor and ClipCursor()s it to the client rect only while hidden (gameplay); in menus it is released. While the headset shows the game and the window has focus, the mod keeps the cursor clipped to the window.

## Vehicles and videos (2026-09-26)

- Vehicles use `CAMERA_FREE_MODE` (0), confirmed by the camera-mode log (on foot is 10). Its update `0x6D9780` passes the per-frame look input to its core `FUN_006d7180` at `0x6DA006` (usercall, EAX = rfg_camera) and clears it afterwards. A drive showed the vertical input in free_mode_params `user_elev` (`0x01DE4CC0`); the absolute-mouse pitch (`0x01DE4D54`) stayed 0. With head aim the mod zeroes both in a mid hook at that call.
- `HeadAim` is the single aim setting (the separate `LockCameraPitch` was merged into it): head aim and the pitch lock only make sense together.
- Video cutscenes (GS_VIDEO_CUTSCENE_PLAY 0x31) and the loading screen after them were black in the headset: behind the video the engine keeps drawing the frozen 3D camera, whose unchanged position matched a stale eye setup, so frames were taken for eye images. Now outside gameplay the headset always shows the virtual screen, and the eye setups are discarded while no eye poses are handed out.

## First person (2026-09-26, in development, `FirstPerson=1`)

- **Eye position:** `FUN_00ab2260(human*, vector* pos, matrix* orient or null)` (cdecl) returns the midpoint of the two eye bones (bone indices at `human+0xCC` → +0x154 / +0x158). Without them it falls back to `FUN_00a9b5d0` (RSL's `human_get_head_pos_orient`), which is only the object position plus a stance height. Measured: eyes 1.675 m above the feet (`object::pos`, human +4).
- **Floor:** a STAGE reference space gives the LOCAL origin's height above the real floor. The camera height is the character's feet + that height + the head's LOCAL y, so the virtual floor matches the real one (the player is usually shorter than the character).
- **Walking follows ideal_orient:** head aim changed only `real_orient`, and walking did not follow the head. Camera-relative movement uses the camera's `ideal_orient` (`0x01DE4BC4`, forward row `0x01DE4BDC`). In first person the mod sets it to the head-aimed orientation as well (restored before the next camera update); confirmed in the headset.
- **Hiding the player:**
  - `human_hide` (`0xA9B7E0`, cdecl(human*, bool)) sets the hidden flag (human +0xB4 bit 1) and clears the object's visible bit (+0x56 bit 7). **Both variants made the player fall through the ground**: the full call, and only the visible bit (hidden flag cleared right after the call).
  - `render_alpha` (human +0xAAC; offsets compiled from RFGR_Types, cross-checked with `hflags` at +0xB4) = 0 hides only the skin; the engine's look-around camera (`FUN_006d3d50`) does the same. The human render update `FUN_00a9bb90` (vtable slot 17) sets each skin instance's alpha to `render_alpha * camera_alpha_override` (+0xAB0); `FUN_00a7e1d0` (effectively human_set_render_alpha) passes it on to attached items.
  - Used: a mid hook at `0xA9BD0C` in the render update, where the "skin instances shown" byte (AL, human in ESI) is stored; it is cleared for the local player. That hides the body and the second skin instance (human +0x9C → +8, the head mesh) through `FUN_004c4910(instance, visible)`, without touching physics.
  - The jacket is not part of the character: it is an attached item (object type 1, no subtype) in the player's child list (`object+0x34`, next `+0x38`, type `+0x7E`, subtype `+0x7F`). It is hidden with `object_set_visible` (`FUN_00a8c440`, cdecl(object*, bool), recursive). Other children seen: the weapon (type 1, subtype 7) and an effect (type 4) at head height.
- **Room-scale:** the camera is the character's position plus the head's offset from a reference point in the room. Walk input towards the head is added through the analog getter; the distance covered that way moves the reference along. The offset is capped at 0.6 m. Not yet tested.

## Controls and motion controllers (2026-09-26, in development)

- **Control action table** (around `0x012B8600`): 16-byte entries `{name, index, ?, type}` for the CAS_* sets, CBA_* buttons and CAA_* analog inputs. On foot: attack 20, melee 21, jump 22, sprint 23, crouch 24, reload 25, zoom 26, cover 27, action 28, charge detonate 29; weapon menu 0, weapon menu directions 50/52/54/56, map 58, pause 86, cycle weapons back/forward 99/100, menu select 73, abort 74.
- **Analog getter** `FUN_006f8e30(axis, user)` (cdecl, float in [-1, 1], user 0): axis 0/1 camera rotate/elevate (`FUN_00702c70` → `FUN_006c6520` sets the look input `0x01DE4D6C` / `0x01DE4D68`), axis 2/3 walk right/forward. On-foot movement `FUN_006fc3e0` normalises axes 2/3 and turns them into a world direction with the camera orientation (`FUN_006f74f0`).
- **Button getters**, cdecl(action, user), bool in AL: `0x6FE970` held, `0x6FEA30` pressed this frame, `0x6FEAF0` pressed this frame and consumed, `0x6FE9D0` probably released. Decoded from the gamepad path (value below -threshold = down, +4 previous value, +0x11 consumed flag); keyboard bitsets at input +0x658, +0x690/+0x6C8, +0x700/+0x738. The mod ORs its virtual buttons into all four and adds its axes in the analog getter.
- **OpenXR actions:** grip/aim pose with velocity, trigger, squeeze, A/B, thumbstick and click; Index bindings with Touch and simple-controller fallbacks; synced once per headset frame. The button mapping and a swing-to-melee trigger (right controller faster than 2.5 m/s) are in `input.cpp`. Not yet tested.
- **Hand aim:** in first person with controllers, `real_pos` / `real_orient` (+0x2C / +0x50) follow the right controller's aim pose; walking still follows the head.
- **Weapon in the hand:** writing the weapon object's position from the camera update has no visible effect (flat-screen test: the game re-attaches it to the hand bone before drawing). The item render update `FUN_00a65190` (`__thiscall(item, arg)`, `ret 4`, vtable slot 17 of `item`; `weapon` uses `FUN_00bdd030`, which calls it) copies `object::pos` / `orient` into the render instance, or passes them to `FUN_0080b9a0` for skinned items. The mod swaps in the hand pose for that call only. Not yet tested; the weapon models' axes are a guess.
- **Vtables** (RTTI): object `0x012D712C`, item `0x01313754`, weapon `0x0133BD84`, human `0x013169C4`, npc `0x01317364`, player `0x01317F64`.
- `rfg_camera::last_aim_point_dist` (+0xFC) is only ever reset (to -1, `FUN_006d0a10`), so it is no aim distance; the reticle sits at a fixed 15 m on the hand ray.
