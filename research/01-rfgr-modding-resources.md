# 01 - RFG:R modding and reverse-engineering resources

Researched 2026-09-25. Sources: web searches and fetches, plus shallow clones of the key GitHub repos (read locally; nothing is vendored into this repo). **Only URLs that were actually fetched or cloned are listed as verified.** Items marked *(unverified)* came from search snippets only.

Legend. **Relevance** is relevance to a 6-DOF OpenXR VR mod (High / Med / Low). Addresses are given as **RVAs** (offsets from the `rfg.exe` module base) unless marked VA. A VA assumes image base `0x400000`, so `VA - 0x400000 = RVA`. ASLR is on, so always rebase.

---

## 0. Summary / key takeaways

1. **The game is 32-bit (x86).** PCGamingWiki says so, and every community hook project builds `x86` (Reconstructor `CMakeSettings.json` = `msvc_x86`, Sledge `set_arch("x86")`, RFGRHook needs the *x86* Ultimate ASI Loader). The VR mod DLL must be x86. See `00-local-findings.md`: SteamVR registers a 32-bit OpenXR runtime (`steamxr_win32.json`).
2. **There are two exe builds, and all community offsets are per-build:**
   - **Steam** `rfg.exe` SHA-256 `0d52039e7f2d3f25a4be52a2aba83919456fb3f00e52e75051726247471a2df4`. **This matches our local `game/rfg.exe`** (verified with sha256sum). Sledge notes this exe did not change between Steam builds 3121288 (2018-09-20) and 10642344 (2023-02-26).
   - **GOG** `rfg.exe` SHA-256 `7a82d2d0f425af5e75d8ffbce12fac53eb5ca9cd812731ccf5a29697e906af0e`.
   - Reconstructor and RFGRHook target **GOG only**. The Terraform community patch ("Unification feature: converts Steam version to GOG") xdelta-patches the Steam exe into the GOG one. **Sledge (Aug-Sep 2026, active) has a dual `OFFSET(gog, steam)` table.** That table is the best source of Steam-build addresses for our exe. RSL1 used older 2018 Steam addresses, so treat its raw numbers as stale.
3. **Debug symbols exist.** Steam originally shipped `rfg.pdb` in the game folder in July 2018 (FearLess CE forum: "Just delete the rfg.pdb located in the RFG DIR"). Our current install has no PDB; the exe only carries the path string `...\win32\steam\master\rfg.pdb`. RFGRHook's author says their work was "only possible thanks to the debugging symbols available for the game". **The Terraform patch repo contains `z_pdbs.7z` with `gog\rfg.pdb` and `steam\rfg.pdb`** (~116 MB each, dated 2023-02-16), but the archive is **AES-encrypted** (7z `7zAES`). The password is not public, so ask on the FactionFiles Discord. That repo's identifier names (e.g. the IDA-style anonymous struct `$E92AFFDEE62810D3128575A938105420`, `keen::graphics::beginFrame(keen::GraphicsSystem*, keen::RenderSwapChain*)`) come from PDB type info. Getting a PDB whose GUID matches our exe would make the RE side of the project much easier.
4. **Camera access is already solved:**
   - Global `rfg_camera` object: RVA `0x19E3B50` on GOG. The Steam VA is `0x01DE4B50` (RVA `0x19E4B50`) per Sledge.
   - The struct is fully laid out in `RFGR_Types/rfg/Camera.h`. It has `real_pos`, `real_orient` (rvec/uvec/fvec matrix), `ideal_*`, `last_*`, `real_fov`, **`render_pos` / `render_orient`**, `m_near_clip_dist`, `m_far_clip_dist`, and a `camera_mode` enum that includes `CAMERA_FIRST_PERSON_MODE = 8`, `CAMERA_SLEW_MODE = 1` and `CAMERA_FREE_MODE = 0`.
   - Free cameras work by patching `camera_do_frame` writes (Reconstructor: RVAs `0x2E0454` (8 bytes) and `0x2E0494` (6 bytes), GOG) and then writing `real_pos` every frame from a `player_do_frame` hook.
   - RSL1 also hooked **`rl_camera::render_begin`** (Volition render lib). `rl_camera` holds `matrix44 m_projection_transform`, `m_view_transform`, `m_fov`, frustums and clip planes. That is the most promising per-eye view/projection injection point.
5. **The render hooks are proven.** Kaiko's remaster renderer is a `keen::` D3D11 layer. Reconstructor and Sledge hook:
   - `keen::graphics::beginFrame`: GOG RVA `0x86A8A0`, Steam VA `0xC6ABD0`.
   - `keen::graphics::endFrame`: GOG RVA `0x86FFF0`, Steam VA `0xC70320`.

   `keen::GraphicsSystem` exposes `ID3D11Device* pDevice` and `ID3D11DeviceContext* pImmediateContext`. `RenderSwapChain` exposes `IDXGISwapChain*`, the back-buffer RTV and **`pBackBufferDepthView`** (DSV). A Dear ImGui overlay is drawn in the endFrame hook. Reconstructor dropped kiero/Present hooks in favour of these engine hooks (commit "Remove kiero and d3d11 hooks", 2023-10-07).
6. **First-person already exists in rough form:**
   - RSL1 had a first-person camera: `CameraManager::UpdateFirstPersonView` uses `human_get_head_pos_orient` (RSL1 RVA `0x69B5D0`, old Steam build) plus an offset and lerp.
   - MartianMadman's `RFG_1stPerson` (xtbl-only: `camera.xtbl`, `player.xtbl`, `turrets.xtbl`, `anim_files.xtbl`) and `RSL1_1stPerson-plus` (animation swaps to reduce clipping).
   - Known problems: body clipping, the camera sits right of the head, weapon zoom breaks, and vehicles stay third-person.
7. **Stereo prior art:**
   - bo3b's **3DMigoto fix** for the DX11 Re-Mars-tered build (2018). Sky moved to depth, HUD/crosshair depth-selectable, HUD hide. Shadows were never fixed.
   - A **vorpX G3D profile** (RJK_, 2019) with head tracking.
   - ReShade reports "the game doesn't hide its Z buffer".
   - No real VR mod, UEVR-style attempt or VR Discord effort was found.
8. **Shader source recreation exists.** `rfg-modding/RFGR-Shaders` (MIT, 2025) holds auto-decompiled HLSL (DXBC → SPIR-V → HLSL) for the `.fxo_kg` shaders plus `ShaderMetadata.json`. Use it to find constant-buffer layouts (view/proj) and the HUD/post-process shaders.
9. **There is no anti-cheat.** Multiplayer is P2P through Kaiko's `sw_api.dll` plus a master server. Community replacements: RfgNetworking and SwApiNet. Modded clients desync or crash, and the MP community went back to vanilla. **The VR mod should refuse to run in MP.** Sledge reads `g_multiplayer` at VA `0x02FEA588` (GOG) / `0x02FEB588` (Steam).
10. **Best code to learn from or reuse** (check licenses):
    - **Sledge** (C++23/xmake/safetyhook/sol2/imgui; dual Steam+GOG; no license file, so ask the author).
    - **Reconstructor** (MPL-2.0; DashFaction-derived patching library, plugin hot-reload host).
    - **RFGR_Types** (struct definitions; no license, so ask).
    - **RSL1** (GPL-2.0; first-person and free cam, `rl_camera` hook).
    - **RFGRHook** (no license; GOG console and slew restore).

---

## 1. Code-injection frameworks / script loaders (highest relevance)

| Resource | URL | Relevance | Status / last activity | License | What it is |
|---|---|---|---|---|---|
| **Sledge** (arrowsv) | https://github.com/arrowsv/sledge · docs https://sledge.readthedocs.io *(docs URL from README, not fetched)* | **High** | **Active.** First commit 2026-08-12, last 2026-09-22, 28 commits; releases 0.1.0-beta.1/beta.2 (2026-08-18) | **None in repo** (ask before reuse) | Mod loader + mod manager + Lua (sol2) API + ImGui overlay. **Supports Steam and GOG** by exe SHA-256 (`src/common/game_version.cpp`). |
| **Reconstructor** (formerly RSL2; moneyl, arrowsv) | https://github.com/rfg-modding/Reconstructor (old URL https://github.com/rsl-dev/RSL2 redirects) | **High** | Maintained for patches. Last commit 2025-07-23 (MP patches, cheats-save patch, Sentry). README: "on hold to focus on Nanoforge". 64 commits in clone | **MPL-2.0** | "Community patch / extension". Launcher + injected Host DLL + hot-reloadable plugin DLLs. **GOG exe only.** |
| **RSL / RSL1** (moneyl) | https://github.com/rfg-modding/RSL · docs https://rsl.readthedocs.io | **High** (reference) | **Archived.** Final release "Final2" 2020-11-25; last commit 2021-05-28 | **GPL-2.0** | Lua (LuaJIT + sol2) scripting, ImGui overlay (script editor, teleport, explosion spawner, free cam, **first-person cam**), MinHook + kiero. Built for the 2018-2020 Steam exe. |
| RSL-Docs | https://github.com/rfg-modding/RSL-Docs | Low | Archived 2020-11-25 | GPL-2.0 | Sphinx docs for the RSL1 Lua API. |
| **RFGR_Types** | https://github.com/rfg-modding/RFGR_Types | **High** | Last commit 2025-01-29 ("Add keen memory layout types") | **None** (ask) | Reversed engine structs used by Reconstructor. |
| **RFGRHook** (Tervel1337) | https://github.com/Tervel1337/RFGRHook · FactionFiles https://www.factionfiles.com/ff.php?action=file&id=8105 | Med-High | v1.0 2025-02-06; 4 commits | None | Restores ~300 debug console commands and the console (tilde), slew/freecam F1, HUD hide F2, pause F3, fog F4, teleport-to-camera F5, and `drop_car`. **GOG only.** ASI (MinHook + CookiePLMonster ModUtils). |
| **rustfaction** (arrowsv) | https://github.com/arrowsv/rustfaction | Low-Med | 2025-11-25; 8 commits | None | Rust rewrite of the community patch (GOG): launcher + patch (memory-limit/string-pool patches ported from Reconstructor). |
| RFGR-Extended-Camera (moneyl) | https://github.com/Moneyl/RFGR-Extended-Camera · Nexus https://www.nexusmods.com/redfactionguerrillaremarstered/mods/12 *(Nexus page behind Cloudflare, not fetched)* | Med | Archived. Releases 1.0.0 (2019-01-17) and 1.0.1 (2019-02-02) | MIT | Early small free-cam, fog and HUD toggle DLL. Its README calls it a "relatively small example". Superseded by RSL. |
| RfgPatch (moneyl) | https://github.com/Moneyl/RfgPatch | Low | 2022-06 | MIT | Map-mod installer. |

### 1.1 Reconstructor: useful file paths
Paths are relative to the repo root. It was cloned and read.
- `Launcher/launcher/Launcher.cpp`. Starts `rfg.exe` **suspended** with the command-line arg `/RanWithReconstructor`, injects `Reconstructor/Host.dll` via `VirtualAllocEx`, `WriteProcessMemory` and `CreateRemoteThread(LoadLibraryA)`. A good template for our launcher.
- `NagDLL/dinput8.c`. A `dinput8.dll` proxy that exits the game unless `/RanWithReconstructor` is on the command line. **If a user has Reconstructor/Terraform installed, a direct launch fails, which could conflict with our proxy-DLL approach.**
- `Host/host/*`. Plugin host with hot reload (`IHost.h`, `Plugin.cpp`) and Sentry crash reporting.
- `Common/common/patching/{FunHook.h, CallHook.h, CodeInjection.h, AsmWriter.h, Offset.h}`. The DashFaction patching library (x86 asm writer; `OffsetPtr<T>(rva)` = module base + RVA).
- `Reconstructor/reconstructor/hooks/RenderHooks.cpp`:
  - `keen_graphics_beginFrame` at RVA `0x0086A8A0`. It captures `GraphicsSystem`/`RenderSwapChain`, waits about 600 frames and `gameseq_get_state()` in the range 0..63, then grabs `pDevice`, `pImmediateContext`, `pDefaultSwapChain->pSwapChain` and sub-classes the WndProc.
  - `keen_graphics_endFrame` at RVA `0x0086FFF0`. It renders ImGui into `pCurrentSwapChain->backBufferRenderTarget.renderTargetViews[0]`.
  - `primitive_renderer_begin_deferred` at RVA `0x000F0E50`. A callback point for engine debug-draw primitives (`gr_3d_line`, `gr_sphere`, ...). Useful for debug-drawing VR controllers and rays.
- `Reconstructor/reconstructor/hooks/Camera.cpp`. The free cam:
  - Patches camera_do_frame at `+0x2E0454` (8 bytes) and `+0x2E0494` (6 bytes).
  - Moves `MainCamera->real_pos` along `real_orient.fvec/rvec/uvec`, lerping toward the target.
  - Teleports the player with `human_teleport_unsafe` (RVA `0x0067C290`) and sets `hflags.ai_ignore`/`invulnerable`.
- `Reconstructor/reconstructor/hooks/PlayerDoFrame.h`. The `player_do_frame` hook at RVA `0x6E6290` is the per-frame update point.
- `Reconstructor/reconstructor/util/Util.h`. Globals:
  - `MainCamera = OffsetPtr<rfg_camera*>(0x019E3B50)` (static object, not a pointer)
  - `World = 0x02B97490`
  - `RfgMenusList = 0x1266698`
  - `SpeedScale = 0x0125BBD4`
  - mouse-visible patch at `+0x1B88DC`
  - center-mouse call at `+0x878D90`
- `Reconstructor/reconstructor/functions/Functions.h`. About 100 function RVAs (GOG), including:
  - `camera_push_mode 0x2C9240` and `camera_pop_mode 0x2C92D0`
  - `hud_hide 0x441AE0`
  - `game_render_set_fog_enabled 0x3C2C50`
  - `gr_screen_coords_from_world_coords 0x109570`
  - `gr_3d_line 0x15E000`
  - `GameseqGetState 0x3BFC70`
  - `static_mesh_find 0x4094A0`
- `Reconstructor/reconstructor/hooks/GrdRenderHooks.h`. Re-enables the stripped `grd_*` debug-draw functions by forwarding them to `gr_*`.
- `Reconstructor/reconstructor/hooks/XmlHooks.h`. Overrides xtbl files from `Reconstructor/Overrides/` (loose-file override).
- Dependencies (submodules): subhook, imgui, kiero (removed from use), sol2, asmjit, tracy, sentry-native, pugixml, RFGR_Types, RSL2-Dependencies.

### 1.2 RFGR_Types: useful files
- `rfg/Camera.h`:
  - `rfg_camera` layout. `mode` + `mode_stack[8]` + `mode_stack_size` + `level_mode`. Then `real_pos` at `+0x2C`, `ideal_pos` at `+0x38`, `last_pos`, `real_orient` at `+0x50`, `ideal_orient` at `+0x74`, `last_orient`, `real_fov`, `ideal_fov`, `last_fov`, `target_handle`, ..., `render_pos`, `render_orient`, `free_params`, `lookaround_params`, `third_person_params`, `mscript_params`, `cscript_params`, `m_camera_view_data`, DOF values, `m_near_clip_dist`, `m_far_clip_dist`, `m_high_lod_far_clip_dist`.
  - The offsets were derived by hand from the struct definition. RFGRHook's slew `Pos` (VA `0x01DE3B88`) and `Orient` (VA `0x01DE3BC4`) land exactly on `ideal_pos` and `ideal_orient`, which cross-validates the layout. **Re-verify in the debugger.**
  - Also defines `camera_mode`, `camera_view_table_entry` (fov_min/max, zoom) and `c_cutscene_shot` (fov/near/far keylists).
- `rfg/keen/GraphicsSystem.h`. `keen::GraphicsSystem` (`pDevice`, `pImmediateContext`, `screenAspectRatio`, `pDefaultSwapChain`, `pCurrentSwapChain`, `currentFrameNumber`, window-mode fields) and `keen::RenderSwapChain` (`depthBufferFormat`, `windowHandle`, `backBufferRenderTarget`, `pSwapChain`, `pBackBufferRenderTargetView`, `pBackBufferDepthView`, `windowWidth`/`Height`, `presentationInterval`). Also `keen::PixelFormat`, `RenderGeometry`, `VertexFormat`.
- `rfg/Player.h`, `Human.h`, `Object.h`, `World.h`, `Weapon.h`, `Matrix.h`, `Vector.h`, `render_lib/{Mesh,Material,Light,Renderable,RlBase}.h`, `stream2/*`, `keen/{Memory,Threading}.h`.

### 1.3 Sledge: useful file paths (Steam-compatible)
- `src/patch/utils/address.hpp`. `#define OFFSET(gog, steam)`; values are **VAs** (base `0x400000`).
- `src/common/game_version.cpp`. exe SHA-256 → version (hashes in section 0).
- `src/patch/graphics/graphics.cpp`. `keen_graphics_begin_frame` at `OFFSET(0xC6A8A0, 0x00C6ABD0)` and `keen_graphics_end_frame` at `OFFSET(0xC6FFF0, 0x00C70320)`, using safetyhook. ImGui init and WndProc subclass.
- `src/patch/rfg/camera.hpp`. `REF_VAR(g_camera, rfg::camera, OFFSET(0x01de3b50, 0x01de4b50))`. The full camera struct, now including `rl_view_frustum`, `plane_info` and `frustum_cache`.
- `src/patch/rfg/game.hpp`:
  - `set_hud_hidden OFFSET(0x00841ae0, 0x00841a90)`
  - `set_fog_enabled OFFSET(0x007c2c50, 0x007c2c70)`
  - `gameseq_get_state OFFSET(0x7BFC70, 0x007bfcf0)`
  - `game_pause`/`unpause`/`is_paused`
  - `g_frames_per_second`, `g_frametime_minimum`
  - `g_multiplayer OFFSET(0x02fea588, 0x02feb588)`
  - `g_player_input_disabled OFFSET(0x01e299b9, 0x01e2a9b9)`
- `src/patch/rfg/graphics.hpp`. keen graphics structs (shader/texture/D3D11 types), `g_mouse_visible`, `g_os_app_active`.
- `src/patch/rfg/player.hpp`. `get_local_player OFFSET(0xA10540, 0x00a108d0)`. `src/patch/rfg/human.hpp` has large human struct plus function offsets.
- `src/patch/rfg/{havok,vehicle,weapon,world,object,xml,...}.hpp`. More reversed types.
- `mods/Freecam/mod.lua`. A Lua free cam built on `game.get_camera()`. `mods/Toggle HUD and Weather/`.
- `src/launcher/injector.cpp`. Launcher and injector.

### 1.4 RSL1: useful file paths (older Steam build; addresses likely stale)
- `RSL/Bootstrapper.cpp`. Hooks:
  - `KeenGraphicsBeginFrame` at `+0x86DD00`
  - `KeenGraphicsResizeRenderSwapchain` at `+0x86AB20`
  - **`rl_camera::render_begin` at `+0x137660`**
  - `keen::ImmediateRenderer::beginRenderPass` at `+0x86C810`
  - `D3D11Present` via kiero
- `RSL/FunctionManager.cpp`. About 170 function RVAs:
  - camera: `CameraStartSlewMode 0x2D09B0`, `CameraStartFirstPerson 0x2C9AC0`, `CameraStopFirstPerson 0x2C9B60`, `CameraViewDataSetView 0x2D0290`
  - render: `GameRenderGetMainScene 0x3C2920`, `GameRenderSetFarClipDistance 0x3C2C40`
  - player: `human_get_head_pos_orient 0x69B5D0`
- `RSL/RlCamera_RenderBegin.cpp`. Captures `rl_camera`, `rl_renderer`, `rl_render_lib`, `rl_state_manager`, and the main scene's `m_scene_renderer_p->m_part2_params.p_camera`.
- `RSL/CameraManager.cpp`:
  - Free cam and **first-person cam** (`UpdateFirstPersonView`: head pos + offset + lerp; optional auto-rotate of the player toward the camera heading).
  - Camera-write disable is done by **byte-pattern scans** (`FindPattern`), which are more robust across builds than hardcoded RVAs.
  - `rfg_camera*` was read as `*(DWORD*)(base+0x23394C)` in that build.
- `RSL/RFGR_Types.h`. Includes a commented-out `rl_camera : rl_scene_entity` with `matrix44 m_projection_transform; matrix44 m_view_transform; float m_fov; rl_view_frustum m_frustum; ... m_near_clip; m_far_clip; ... m_use_pixel_aspect_ratio; motion blur fields`.
- `RSL/*StateBlock*.h` (`HdrStateBlock`, `DofStateBlock`, `SsaoStateBlock`, `ShadowStateBlock`, `SunShaftsStateBlock`, `TerrainStateBlock`). Render-lib state blocks, useful for disabling motion blur, DOF and screen-space effects in VR.

---

## 2. File formats / asset tools

| Resource | URL | Relevance | Status | License | Notes |
|---|---|---|---|---|---|
| **Nanoforge** (moneyl) | https://github.com/rfg-modding/Nanoforge · releases https://github.com/Moneyl/Nanoforge/releases | Med | Master = C# rewrite (last commit 2025-03-30, "multi draw indirect in renderer"; repo pushed 2025-06-29). Release v1.3.0 2025-01-26. The older C++ and Beef versions live on branches | MIT | Map viewer/editor (MP and Wrecking Crew maps), mesh view/export (.obj), texture view/export/reimport, xtbl editor with validation, auto mod-manager output. Useful for inspecting meshes (weapon/arm models for VR hands) and xtbls. |
| RfgToolsPlusPlus | https://github.com/rfg-modding/RfgToolsPlusPlus | Med | 2023-08 (repo push), last commit in clone 2022-10 | MIT | C++ readers for vpp_pc/str2_pc (r/w), asm_pc (r/w), cpeg/cvbm (r/w), csmesh/ccmesh (r), rfgzone/layer_pc (r), cterrain/ctmesh (r). Docs: `Documentation/Packfile.md`. Credits Gibbed for the original RE of packfile/peg/vint_doc/asm_pc. |
| RfgTools (Beef) | https://github.com/rfg-modding/RfgTools | Low | 2025-01-17 | MIT | Beef port; `Documentation/RfgZonexFormat.md`. |
| **RFGM.Formats / RFGM.Archiver** (rast1234) | https://github.com/rfg-modding/RFGM.Formats · FF https://www.factionfiles.com/ff.php?action=file&id=8095 | Med | 2025-08-09; NuGet package | MIT | C# formats library. Adds **fxo_kg shader (partial)**, anim/rig (partial), savefile. `RFGM.Formats/Shaders/ShaderFile.cs` extracts the DXBC from `.fxo_kg`. |
| **RFGR-Shaders** | https://github.com/rfg-modding/RFGR-Shaders | **High** (for stereo shader work) | 2025-09-19 | MIT | Auto-generated HLSL of the game shaders (`Shaders/{Character,Generic,Standard,Terrain}/<fxo_kg name>/..._vs#.hlsl/_ps#.hlsl` + `ShaderMetadata.json`). Use it to identify view/projection constant buffers and screen-space passes. |
| RfgUtil (moneyl) | https://github.com/Moneyl/RfgUtil (releases v1.2.1 2023-12-28) | Low | 2023 | MIT | CLI unpack/repack and asm_pc updater. |
| AsmTool | https://github.com/rfg-modding/AsmTool | Low | 2022 | MIT | asm_pc editing. |
| RFG-Texture-Editor-Redux | https://github.com/Moneyl/RFG-Texture-Editor-Redux | Low | 2020 | GPL-2.0 | peg texture editor. |
| rfg-localization / RFGR.SaveEditor | https://github.com/rfg-modding/rfg-localization · https://github.com/rfg-modding/RFGR.SaveEditor | Low | 2025 | MIT | Localization strings; save editor. |
| Gibbed.Volition | https://github.com/gibbed/Gibbed.Volition *(from search; not cloned)* | Low | old | ? | Original vpp/peg tools. Gibbed Tools 1.0.2 is on FactionFiles. |
| Volition Table File Editor | inside the Terraform repo `z_tools/Volition_Table_File_Editor.zip` | Low | - | Volition | GUI xtbl editor (Volition internal tool). |
| OGE (Open Guerrilla Editor) | https://github.com/Moneyl/OGE | Low | archived 2020 | MIT | Nanoforge predecessor. |
| ZenHAX thread | https://zenhax.com/viewtopic.php@t=8125.html *(unverified; search only)* | Low | - | - | RFGR format discussion. |

**Format notes relevant to VR:**
- The camera is table-driven. `data/misc.vpp_pc` contains `camera.xtbl` (camera views: fov_min/max, distances, DOF), `player.xtbl`, `turrets.xtbl` (camera offsets) and `anim_files.xtbl`. The 1st-person mods edit exactly these files.
- Shaders are in `.fxo_kg` files.
- There are Lua-ish `.scriptx` mission scripts.
- Loose-file overrides are possible via SyncFaction or the Reconstructor `Overrides/` folder.

---

## 3. Mod managers, patches, community

| Resource | URL | Relevance | Status | Notes |
|---|---|---|---|---|
| **FactionFiles Discord** ("Red Faction Community Discord (FactionFiles)") | https://discord.gg/factionfiles (link from the SyncFaction and Terraform READMEs; not joined) | **High** | Active | The main RFG modding hub. moneyl, rast1234, arrowsv, Camo and Tervel1337 are here. Ask about the PDB password, `rl_camera` addresses on current builds, and prior VR/first-person work. |
| FactionFiles.com | https://www.factionfiles.com · RFG tools https://www.factionfiles.com/ff.php?action=files&file_category=3 · Remaster mods https://www.factionfiles.com/ff.php?action=files&file_category=30 | Med | Active (uploads through Sep 2026) | Tools list: RFGR Hook, Gibbed Tools, Texture Editor Redux, Localization Tool, RFGR.SaveEditor, SyncFaction, RFGM.Archiver, Nanoforge v0.19.0. Also "Kinzie's Toy Box" (Volition's **Saints Row: The Third** SDK, not RFG). **RSL Legacy Collection** (id 7920, 2024-07-02): a bundle of all RSL1 scripts including **first-person camera**, flyer, telekinesis and graphics tweaks. **Steam only**, and it disables Reconstructor. **Steam Re-enabler** (rast1234, 2024-07-02) undoes the Terraform Steam→GOG conversion. |
| **SyncFaction** (rast1234) | https://github.com/rfg-modding/SyncFaction · docs https://rfg-modding.github.io/SyncFaction/ | Med | Last commit 2026-06-07; release 2023-09-18 | Mod manager (.NET 6/WPF). Checks file hashes for both versions (`src/SyncFaction.Core/Hashes.cs` lists Steam/GOG hashes of rfg.exe, sw_api.dll, vpps; `rfg.pdb` appears commented out). Launches `launcher.exe` (Reconstructor) if present. Supports `.steam/` and `.gog/` version-specific mod folders (`docs/modding/version_specific.md`). Save locations: Steam `...\userdata\<id>\667720\remote\autocloud\save\keen_savegame_0_0.sav`, GOG `%LOCALAPPDATA%\GOG.com\Galaxy\Applications\51153410217180642\Storage\Shared\Files\autocloud\save\`. |
| **Terraform Patch** (Camo) | https://github.com/CamoRF/Red-Faction-Guerrilla-Terraform-Patch | Med (for PDB / version) | v1.0753 HOTFIX (2023-06-10); `z_pdbs.7z` added 2025-08 | Community content and bug patch (mostly MP). Includes the "**Unification feature: converts Steam version to GOG**". `z_pdbs.7z` is **encrypted** and holds `gog/rfg.pdb` and `steam/rfg.pdb` (116 MB each, 2023-02-16). Also contains `z_tools/` (mTools, Volition Table File Editor, scripts) and `z_debug/`. The changelog shows internal source paths like `C:/unit4projects/rfg/root/code/volition/rfg/code/video_player/video_player.cpp` and a game log file `Red Faction Guerrilla Re-Mars-tered.log`. |
| Red Faction Wiki: RF:G Editing | https://www.redfactionwiki.com/wiki/RF:G_Editing_Main_Page · Nanoforge https://www.redfactionwiki.com/wiki/Nanoforge_(Tool) | Med | Active | Lists 40+ file formats (xtbl, scriptx, dtodx, gtodx, vpp_pc, meshes, textures, anims, audio), tools, and Nanoforge tutorials. |
| Nexus Mods: RFG:R | https://www.nexusmods.com/games/redfactionguerrillaremarstered | Med | - | Cloudflare blocked fetching; items below are from search snippets *(unverified detail)*. #12 RFGR Extended Camera. **#13 Vehicle Camera Options** (vehicle FOV, walkers, tanks; likely xtbl edits). #32 "RFG R - DEFINITIVE" graphics mod. **#40 No-HUD-No-Effects** (ReShade ShaderToggler toggling the HUD and effects, so HUD shader hashes are identifiable). Vortex extension https://www.nexusmods.com/site/mods/1145. |
| Nexus Mods: original RFG | https://www.nexusmods.com/games/redfactionguerilla/mods | Low | - | Original 2009 game mods (e.g. "Red Planet Guerrilla Re-shade"). |
| ModDB | https://www.moddb.com/games/red-faction-guerilla (mods/downloads pages returned 403) | Low | - | Mostly classic-era mods. |
| Mod Manager (HazardX, classic) | https://www.factionfiles.com/ff.php?action=file&id=3863 *(search only)* | Low | old | Original xtbl-merge mod manager (modinfo.xml format, still used by the 1stPerson mods). |
| RfgNetworking | https://github.com/rfg-modding/RfgNetworking | Low | 2024-01 | Beef replacement for `sw_api.dll` plus a master server. |
| SwApiNet | https://github.com/rfg-modding/SwApiNet | Low | 2025-01 | .NET proxy for `sw_api.dll` (x86; renames the original to `sw_api_original.dll`). |
| Steam MP group chat | https://steamcommunity.com/chat/invite/AIMCH8E7 (from SyncFaction docs) | Low | - | - |

---

## 4. Camera / FOV / first-person / HUD / stereo mods

| Resource | URL | Relevance | Status | Takeaways |
|---|---|---|---|---|
| **RFG_1stPerson** (MartianMadman) | https://github.com/MartianMadman/RFG_1stPerson | Med-High | v2.1.2, last commit 2022-10-30 | Pure data mod (Classic and Remaster variants, Mod Manager `modinfo.xml`). Edits `misc.vpp\camera.xtbl` (replace), `player.xtbl` (PlayerHeight option 0.75-1.0; 0.875 recommended), `turrets.xtbl` (Default/Fine_Aim offsets), `anim_files.xtbl` (walk-only anims, flinch/land anims removed), `startup_video_sequence.xtbl`. Vertical FOV 43. Known issues: **camera sits right of the head, weapon zoom broken, FOV fixed, vehicles stay 3rd person.** Shows how far xtbl alone gets you. |
| **RSL1_1stPerson-plus** (MartianMadman) | https://github.com/MartianMadman/RSL1_1stPerson-plus | Med | 2022-10-20 | Add-on for the RSL1 first-person cam: many animation swaps to cut body clipping, plus vehicle FOV. Notes that RSL1 first-person updates the camera per frame, so higher FPS means less clipping. **In VR the pose must be applied at render time, after animation.** |
| RSL1 first-person / free cam | see section 1.4 | High | archived | Head pos from `human_get_head_pos_orient`; the camera-write patches. |
| **RFGRHook slew** | see section 1 | Med | 2025-02 | Calls the game's own slew function (`Camera::Slew(pos, orient, dt, 0, false)` at VA `0x005B77F0`, GOG) on `ideal_pos`/`ideal_orient`. Calls `camera update` at VA `0x006DFEC0` while paused. Restores mouse/keyboard slew controls. |
| **3DMigoto fix for Re-MARS-tered** (bo3b) | https://helixmod.blogspot.com/2014/12/red-faction-guerrilla_8.html (download `Re-MARS-tered.zip`) | **High** (stereo prior art) | 2018-07-07 | DX11 build: sky moved to depth, broken sun removed, HUD depth-selectable (`\`), crosshair depth-selectable (`'`), HUD hide (Backspace). **Shadows not fixed**; laser-sight cursor broken; blue tint in 4K fullscreen. The d3dx.ini/ShaderFixes contain **shader hashes for the HUD, crosshair, sky and sun**, which is directly reusable knowledge for render-pass classification. |
| Helix fix for original DX9 (Stryker_66) | same page (`Red_Faction_Guerrilla_3D_Shaders_Removed.zip`) | Low | 2014, updated 2018 | Original game: shadows off, DX9 mode, motion blur/DOF disabled, Prototype 2 3D Vision profile. |
| **vorpX profile** (RJK_) | https://www.vorpx.com/forums/topic/red-faction-guerrilla-re-mars-tered/ | Med | 2019-07-03 | G3D mode, head tracking, in-game FOV 85, windowed required, scalable HUD. G3D working implies the game tolerates camera re-rendering and matrix changes reasonably. |
| ReShade depth | Steam thread https://steamcommunity.com/app/667720/discussions/0/1726450077675625721/ · RTShade presets https://github.com/DHYCIX/RTShade/tree/main/2.%20RTShade/Red%20Faction%20Guerilla%20Re-Mars-tered | Med | 2018; RTShade (current) | "This game doesn't hide its Z buffer": DOF and MXAO work. RTShade notes it could not put effects behind fog (the game crashed while searching the fog shaders) and uses `ReshadeEffectShaderToggler.ini`. No reversed-Z or upside-down info was found, so this is still an open question. |
| No-HUD-No-Effects | https://www.nexusmods.com/redfactionguerrillaremarstered/mods/40 *(snippet only)* | Med | ? | Uses ShaderToggler to toggle the HUD and effects, i.e. there is a HUD shader-hash list. |
| Vehicle Camera Options | https://www.nexusmods.com/redfactionguerrillaremarstered/mods/13 *(snippet only)* | Low-Med | ? | Vehicle/walker/tank camera and FOV. |
| Toggle HUD v1.0 | https://www.gamepressure.com/download/red-faction-guerrilla-re-mars-tered-toggle-hud-v10-mod/zd10a9b *(snippet only)* | Low | ? | Caps Lock HUD toggle. |
| Steam guide "HOTFIX: Fix Resolution, FOV, and FPS issues" | https://steamcommunity.com/sharedfiles/filedetails/?id=1433413088 (HTTP 429 on fetch; *content unverified*) | Low | 2018 | - |
| FOV Fix for the original, non-Steam game | https://steamcommunity.com/sharedfiles/filedetails/?id=350864473 (linked from PCGW; not fetched) | Low | - | - |
| Steam discussions | VR wish https://steamcommunity.com/app/667720/discussions/0/1726450077676641854/ (2018, no tech content) · first-person view https://steamcommunity.com/app/667720/discussions/0/1726450077687982181/ (not fetched) | Low | - | **No prior VR mod attempt was found anywhere.** UEVR is Unreal-only and irrelevant; one search snippet wrongly claimed RFG uses Unreal. |

---

## 5. Engine and platform facts

| Fact | Source | Notes |
|---|---|---|
| Engine: **Geo-Mod 2.0**, derived from the Saints Row (CTG) engine; Havok physics; Bink 1.300g cutscenes | PCGW (both pages); Wikipedia/IGDB via search | Volition said Geo-Mod 2.0 was too restrictive to port to Saints Row: The Third. RSL1 bound Havok types (`hkpWorld`, `hkpRigidBody`) and hooked `hkpWorld::stepDeltaTime`. |
| Remaster renderer: a Kaiko `keen::` D3D11 layer (`keen::GraphicsSystem`, `keen::graphics::beginFrame/endFrame`, `keen::ImmediateRenderer::beginRenderPass`, `keen::getBuildVersionString` at RVA `0x58740`) under Volition's `rl_*` render lib (`rl_camera`, `rl_renderer`, `rl_scene`, `rl_primitive_renderer`) | Reconstructor, RSL1, RFGR_Types | Save file is `keen_savegame_0_0.sav`. |
| D3D11, SM5, **32-bit exe only** | PCGW RFG:R | Original RFG Steam Edition: 32-bit, D3D 9.0c / 10 / 11 (DX10 replaced by DX11 in the 2014 Steamworks update; `-nod3d11` / `-nod3d10` switches). |
| FOV slider 45-85 in SP (60 max in MP) | PCGW | Ultrawide native, but renders about 1.6% taller then scales down (e.g. 2560x1097 for 2560x1080). This is relevant to projection and aspect assumptions. |
| Frame cap 250 FPS; vsync option | PCGW | - |
| Config file `%APPDATA%\kaiko\rfg\config.ini` | PCGW | Original RFG: binary `Documents\My Games\Red Faction Guerrilla\Options\rfg_display_options.rfgs_pc`. |
| Intro skip: delete or rename `data\legal_hd.bik`, `logo_kaiko.bik`, `logo_nordic_hd.bik`, `logo_volition_hd.bik` | PCGW | RFGRHook and Sledge also skip intros in code (`g_exit_startup_videos`). |
| Input: XInput, DirectInput, Steam Input (gyro since Patch #5, 2018-08-12); DualSense seen as DInput with broken mapping; 2 binds per action | PCGW | "Tracked motion controllers: false". |
| DRM: Steam (Steamworks) or GOG DRM-free; **no anti-cheat** | PCGW, SyncFaction | Steam exe is not SteamStub-wrapped (see 00-local-findings). |
| MP: up to 16 players, P2P + matchmaking via `sw_api.dll` (Kaiko/THQN master server), no dedicated servers | PCGW, RfgNetworking | Any memory patch can desync; community MP is back on vanilla. |
| Switch port (2019); PS4/XB1 (2018) | PCGW / Wikipedia | Irrelevant, except that a console-derived codebase explains the `keen` platform layer. |
| Steam app 667720; GOG id 2029222893 | PCGW | Owners of the original got the remaster free. |

**Original 2009 / Steam Edition (app 20500) vs the remaster:**
- The original has a different renderer: D3D9/10, later D3D11 in the Steam Edition.
- There is no `keen` layer.
- None of RSL, Reconstructor, Sledge or RFGRHook support it.
- It uses the binary config, and the HazardX Mod Manager and Gibbed tools.
- The 3D Vision Helix fix (DX9) exists.
- **Not a sensible target.** Stick to the remaster.

---

## 6. Reversing aids

| Aid | URL / location | Status | Notes |
|---|---|---|---|
| **rfg.pdb (Steam, originally shipped)** | FearLess CE thread https://fearlessrevolution.com/viewtopic.php?t=7356&start=45 (post by l0wb1t, 2018-07-16) | Historical | The PDB sat in the game dir around CS 4496-4590 (July 2018). **Not in our current install.** Check SteamDB depot history or old manifests (`DepotDownloader` with an older manifest) to recover it for our exe. The GUID must match our exe's CodeView record. |
| **Encrypted PDBs (GOG + Steam)** | Terraform repo `z_pdbs.7z` | Available, password needed | Ask Camo, moneyl or arrowsv on the FactionFiles Discord. |
| Community struct and function databases | RFGR_Types, Sledge `src/patch/rfg/*.hpp`, Reconstructor `Functions.h`, RSL1 `FunctionManager.cpp` + `RFGR_Types*.h` | Available | No shared IDA/Ghidra database was found publicly. |
| Cheat Engine tables | FearLess: https://fearlessrevolution.com/viewtopic.php?t=7359 (CT v1.0, tested on Steam v1.0) · trainer threads t=7361, t=7356 | Available | Health, ammo, etc. **No camera or FOV table was found.** Low value; we already have better camera data. |
| Debug console command names | RFGRHook `Console.cpp` (~300 commands restored thanks to the PDB) | GOG | Likely includes camera, render and debug toggles (slew etc.). Worth reading for render debug switches. |
| Engine debug draw | Reconstructor `GrdRenderHooks.h` + `gr_*` functions | GOG RVAs | Useful for drawing debug rays and controller axes in-world. |

---

## 7. Concrete VR-relevant hook map (collected; all need verification on our Steam exe)

| Purpose | GOG | Steam (our exe) | Source |
|---|---|---|---|
| exe SHA-256 | `7a82d2d0...af0e` | `0d52039e...2df4` (**matches local**) | Sledge, SyncFaction |
| Global `rfg_camera` | VA `0x01DE3B50` (RVA `0x19E3B50`) | VA `0x01DE4B50` | Sledge `camera.hpp`, Reconstructor `Util.h` |
| `keen::graphics::beginFrame` | VA `0xC6A8A0` | VA `0xC6ABD0` | Sledge `graphics.cpp` |
| `keen::graphics::endFrame` | VA `0xC6FFF0` | VA `0xC70320` | Sledge `graphics.cpp` |
| `set_hud_hidden(bool)` | VA `0x841AE0` | VA `0x841A90` | Sledge `game.hpp` |
| `set_fog_enabled(bool)` | VA `0x7C2C50` | VA `0x7C2C70` | Sledge |
| `gameseq_get_state()` | VA `0x7BFC70` | VA `0x7BFCF0` | Sledge |
| `get_local_player()` | VA `0xA10540` | VA `0xA108D0` | Sledge |
| `g_multiplayer` (bool) | VA `0x02FEA588` | VA `0x02FEB588` | Sledge |
| `g_player_input_disabled` | VA `0x01E299B9` | VA `0x01E2A9B9` | Sledge / RFGRHook |
| `g_mouse_visible` | VA `0x01CE76EA` | VA `0x01CE86EA` | Sledge |
| `player_do_frame` | RVA `0x6E6290` | ? (find via sig) | Reconstructor |
| camera_do_frame pos-write patches | RVA `0x2E0454` (8 B), `0x2E0494` (6 B) | ? (RSL1 byte patterns) | Reconstructor, RSL1 |
| `camera_push_mode` / `pop_mode` | RVA `0x2C9240` / `0x2C92D0` | ? | Reconstructor, RFGRHook |
| slew update `(pos, orient, dt, int, bool)` | VA `0x5B77F0` | ? | RFGRHook |
| `human_teleport_unsafe(human*, vector, matrix)` | RVA `0x67C290` (VA `0xA7C290`) | VA `0xA7C660` | Reconstructor, Sledge `human.hpp` (`teleport_human_unsafe`) |
| `human_get_head_pos_orient` | ? | RVA `0x69B5D0` on the **old** 2018 Steam build | RSL1 |
| `rl_camera::render_begin` | ? | RVA `0x137660` on the old Steam build | RSL1 |
| `primitive_renderer_begin_deferred` | RVA `0xF0E50` | ? | Reconstructor |
| `gr_screen_coords_from_world_coords` | RVA `0x109570` | ? | Reconstructor |

Observed GOG→Steam deltas are not constant (for example +0x330 for beginFrame and +0x1000 for camera data). **Do not extrapolate. Use Sledge's table, signatures, or the PDB.**

---

## 8. Open questions / things to verify once the game is installed

Our Steam copy at `game/` has hash `0d52039e...`, so the Steam column of Sledge applies.

1. **PDB.** Can we get a `rfg.pdb` matching our exe's CodeView GUID/age? Options: SteamDB depot history for app 667720 (older manifest with the PDB), the Terraform `z_pdbs.7z` password (ask on Discord), or the GOG build plus its PDB. Decide whether to switch our dev target to GOG if only the GOG PDB is obtainable.
2. Validate the `rfg_camera` layout at VA `0x01DE4B50` live. Check the offsets of `real_pos`/`real_orient`/`render_pos`/`render_orient`/`real_fov`/clip planes, and confirm **which pair the renderer consumes** (`render_*` vs `real_*`).
3. Find `rl_camera::render_begin` and the `rl_camera` layout (`m_view_transform`, `m_projection_transform`, `m_use_pixel_aspect_ratio`) on the current Steam exe. RSL1's `0x137660` is from an older build, so search by string xref `rl_camera` or with RSL1's approach. Determine whether it is called once per frame (main view) or also for shadow and reflection cameras.
4. How many scene renders per frame, and is there a clean "render main scene" function we can call twice (per eye)? Check `game_render_get_main_scene` (RSL1 `0x3C2920`), `m_scene_renderer_p`, and `keen::ImmediateRenderer::beginRenderPass`.
5. Depth buffer: format (`RenderSwapChain::depthBufferFormat`), reversed-Z or not, and whether `pBackBufferDepthView` is the scene depth or only a final one. Needed for reprojection/depth submission (`XR_KHR_composition_layer_depth`).
6. Screen-space effects that break in stereo: shadows (never fixed by 3DMigoto), SSAO, sun shafts, DOF, motion blur, HDR bloom, fog. Map them to `rl_*StateBlock`s (RSL1) so they can be toggled.
7. HUD: can the scaleform-like `vint` UI be rendered to a separate RT for a VR quad layer? Pull the HUD and crosshair shader hashes from bo3b's 3DMigoto `d3dx.ini` / ShaderFixes and from No-HUD-No-Effects.
8. Aspect ratio: does the engine (like the 1.6% ultrawide oversizing) assume symmetric frusta? Index FOV is asymmetric, so check whether the projection can be overridden directly.
9. Does Reconstructor's `dinput8.dll` nag DLL or Sledge's launcher conflict with our injection method (proxy DLL vs launcher)? Decide on coexistence.
10. 32-bit address space: exe is LARGE_ADDRESS_AWARE (00-local-findings). Measure headroom with two eye render targets at Index resolution plus the OpenXR runtime inside a 4 GB process.
11. Aiming decoupling: find where the player's aim/fire direction comes from (camera fvec vs a separate aim vector). This is needed for controller-based aiming. Look at `free_mode_params`/`lookaround_mode_params`, `human` aim fields in Sledge `human.hpp`, and weapon fire functions.
12. Confirm whether the first-person mode `CAMERA_FIRST_PERSON_MODE (8)` and `camera_start_first_person` (RSL1 `0x2C9AC0`) are functional leftovers (e.g. used by turrets) that could replace hand-rolled head attachment.
13. Licensing: contact arrowsv (Sledge, rustfaction) and moneyl (RFGR_Types) about license terms before copying code. Reconstructor is MPL-2.0 (file-level copyleft) and RSL1 is GPL-2.0 (avoid copying code if our license differs).
14. Check `Red Faction Guerrilla Re-Mars-tered.log` (the game's own log, mentioned in the Terraform changelog) for render and camera diagnostics.
