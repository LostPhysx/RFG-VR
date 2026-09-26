# RFG:R 6-DOF VR: Research Index

Goal: mod *Red Faction Guerrilla Re-Mars-tered* (Steam) to full 6-DOF VR with motion controllers (Valve Index / SteamVR) through an injected DLL using OpenXR.
Research compiled 2026-09-25.

## Documents

| # | File | Contents | Read when |
|---|---|---|---|
| 00 | [00-local-findings.md](00-local-findings.md) | Static inspection of our `game/rfg.exe` and this PC's VR runtime registration | **First.** It corrects assumptions in 01-04. |
| 01 | [01-rfgr-modding-resources.md](01-rfgr-modding-resources.md) | RFG:R script loaders/hook projects (Sledge, Reconstructor, RSL/RSL2, RFGRHook), file formats, community, camera/FOV/1st-person/stereo mods, engine facts, **Steam/GOG address table and hook map (§7)** | Before any reversing |
| 02 | [02-openxr-resources.md](02-openxr-resources.md) | OpenXR spec/SDK/tutorials, **x86 target (§0a)**, D3D11 binding, Index input, SteamVR quirks, math (§6), **minimal integration flow (§8)**, 24 gotchas (§9) | When writing the XR layer |
| 03 | [03-flat2vr-techniques-and-resources.md](03-flat2vr-techniques-and-resources.md) | Communities, reference mods (REFramework, anvilengine2vr, PreyVR, MGS5VR, Cyberpunk…), **stereo technique decision matrix (§4)**, 3rd-to-1st-person conversion, pitfalls (§6), plan of attack (§7) | Architecture decisions |
| 04 | [04-toolchain-and-reference-code.md](04-toolchain-and-reference-code.md) | RE tools, hooking libraries, build setup, reference codebases with key file paths, **starting stack (§7)**, **first RE steps (§8)** | Setting up the dev environment |

> **Architecture correction:** docs 03 and 04 were researched assuming x64. **`rfg.exe` is 32-bit (x86).** Read every `x64-windows*` triplet / x64 reference as x86. REFramework/UEVR code is x64-only and must be ported, not linked.

## The most important facts

### The game
- **x86, PE32, large-address-aware, D3D11, VS2010 CRT, no SteamStub.** It can go straight into Ghidra. Local exe SHA-256 `0d52039e…2df4` = the **Steam** build, unchanged 2018-2023 (so community addresses stay valid).
- **Two exe variants exist (Steam vs GOG).** Reconstructor/RFGRHook target GOG only; **Sledge** (arrowsv, active Aug-Sep 2026) keeps a Steam+GOG address table and is our best address source. It has **no license**, so read it but don't copy.
- **The camera is already reversed:** global camera object at Steam `0x01DE4B50`. Its layout is in `RFGR_Types/rfg/Camera.h` (pos/orient, separate `render_pos`/`render_orient`, FOV, clip planes, camera modes incl. first-person).
- **Render hooks are known:** per-frame begin/end at Steam `0xC6ABD0` / `0xC70320` give device, context, swapchain and depth view. `rl_camera::render_begin` (holds view/proj) is the likely per-eye injection point; its Steam address still has to be found.
- **Debug symbols exist:** Steam `rfg.pdb` (2018) is in a password-protected `z_pdbs.7z` in the Terraform repo. **Asking FactionFiles Discord for it is the single highest-leverage action.**
- No dormant stereo/Oculus code in the exe. No anti-cheat; MP is P2P via `sw_api.dll`, so the mod must disable itself in multiplayer.
- Prior art: RSL1 first-person head camera, two data-only 1st-person mods, a 2018 3DMigoto stereo fix (shadows never fixed), vorpX profile, decompiled shaders (RFGR-Shaders, MIT). **No VR mod exists.**

### OpenXR on x86
- **SteamVR 2.17 (2026-09-10) added 32-bit OpenXR.** It is registered on this PC (`WOW6432Node\…\steamxr_win32.json` → `bin\vrclient.dll`; installed SteamVR build ≈ 2026-09-15). It is brand new and has no track record, which makes it **the #1 technical risk**. Validate day 1 with an x86 hello_xr.
- Khronos ships an official **Win32 loader** (1.1.63; imports only KERNEL32/ADVAPI32). vcpkg `openxr-loader:x86-windows(-static)` works.
- x86 gotcha: all OpenXR handles are `uint64_t` typedefs (no type safety), so wrap them or use OpenXR-Hpp.
- Fallback order: **A** in-process OpenXR x86 → **B** in-process OpenVR x86 (long supported, works on older SteamVR) → **C** 64-bit helper process with shared D3D11 textures (only if the 4 GB address space runs out).
- Memory: keep VR textures `D3D11_USAGE_DEFAULT` with no CPU access, and allocate early before fragmentation.

### Technique
- **Stereo:** start with **AFR** (one eye per frame) to prove the pipeline. Target **synchronized double render** (render the scene twice per sim tick) because AFR ghosts fast Geo-Mod debris and smears TAA.
- Write the eye offset into the engine camera **before culling** and build an off-center projection from the per-eye `XrFovf` (keep the engine's near/far).
- Per-frame systems (particles, clocks, TAA history, shadow fitting) must advance **once per stereo pair**.
- World, near-field weapon and HUD each need different stereo handling. Put the HUD/menus on OpenXR quad layers.
- The 3rd-to-1st-person conversion is its own workstream: head-bone camera, hide head, aim from controller, weapon attached to controller, keep a 3rd-person fallback for vehicles/cutscenes.
- Closest open-source templates (MIT): **REFramework** VR module (D3D11 hook, OpenXR/OpenVR runtime abstraction, D3D11 state backup), **anvilengine2vr** (proprietary 3rd-person engine → VR), **KCD1VR**, **elliotttate/vrframework** (17-part guide), **BioShock Trilogy VR**, **maluoi/OpenXRSamples** (minimal).

### Recommended starting stack (adjusted for x86)
- **Loader:** Ultimate ASI Loader or our own proxy DLL. The exe imports `dinput8`, `winmm`, `xinput1_3` and `d3d11` (no direct `dxgi`), so a `winmm.dll`/`dinput8.dll` proxy is natural.
- **Hooks:** SafetyHook (x86 supported; mid-function hooks for grabbing matrices) or MinHook. kiero is archived.
- **XR:** OpenXR loader x86 static, sharing the game's `ID3D11Device`. DirectXMath.
- **UI/log/config:** Dear ImGui (rendered to a VR quad), spdlog, toml++.
- **Build:** CMake + vcpkg `x86-windows-static`, MSVC 2022, `/MT`, PDBs in Release. Hot-reloadable logic DLL behind a stable host DLL.
- **RE:** Ghidra (+ RTTI class recovery), x64dbg (its 32-bit x32dbg), Cheat Engine, ReClass.NET, RenderDoc (launch via `steam_appid.txt` = 667720; PIX/Nsight no longer practical for D3D11).
- **Testing without the headset:** OpenXR-Simulator / xr-sim.

## Licensing summary (for code reuse)

| Source | License | Use |
|---|---|---|
| REFramework, anvilengine2vr, KCD1VR, BioShock VR, vrframework, PreyVR, MGS5VR, RFGR-Shaders, OpenXRSamples | MIT | Copy/port freely with attribution |
| Reconstructor | MPL-2.0 | File-level copyleft; OK if modified files stay MPL |
| RSL1, Special K, OpenComposite, theHunterCotW-VR | GPL | Reference only (unless we go GPL) |
| UEVR | All rights reserved | Ideas only |
| Sledge, RFGR_Types, RFGRHook | none | Reference only; ask authors |

## Top risks
1. ~~SteamVR's 32-bit OpenXR path is untested~~ **Validated 2026-09-25 with `tools/xr-probe`** (SteamVR 2.17.10, 120 Hz, D3D11 session OK; see 00 §xr-probe). Controller input on x86 is still untested.
2. 4 GB address space: the OpenXR runtime + eye swapchains measured at only ~30-80 MB VA. The remaining risk is extra game-side render targets for the second eye.
3. Engine features not built for two views per frame (shadows, TAA, screen-space effects, culling).
4. 3rd-person game design (animations, aiming, vehicles) → 1st-person comfort.
5. Publisher legal pressure (Take-Two/Luke Ross precedent). THQ Nordic's stance is unknown.

## Suggested next steps
1. Ask FactionFiles Discord for the Steam `rfg.pdb` (Terraform `z_pdbs.7z` password) and for permission to reuse Sledge / RFGR_Types code.
2. ~~Build an x86 hello_xr (D3D11) against SteamVR 2.17~~ Done: `tools/xr-probe` works.
3. ~~Load `rfg.exe` into Ghidra, locate `rl_camera::render_begin`, and confirm the Sledge Steam addresses.~~ Done without a PDB (see 00-local-findings).
4. ~~Scaffold the x86 DLL project with a proxy loader and a frame hook.~~ Done: `mod/` (dinput8 proxy, SafetyHook, OpenXR loader). ImGui overlay still open.
5. ~~Milestone 1: game view mirrored to the headset on a quad (a "virtual screen").~~ Done 2026-09-26.
6. ~~Milestone 2: head-tracked stereo.~~ Done 2026-09-26: 6-DOF stereo at the headset's resolution, 120 Hz (two game frames per headset frame, same head pose for both eyes). See 00-local-findings "Milestone" and "Headset resolution".
7. Next: HUD on its own quad layer, motion controllers (aim, input), first-person camera, comfort options.
