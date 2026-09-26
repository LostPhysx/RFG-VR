# 03 - Flat2VR Techniques, Tools, Communities and Lessons Learned

Target: **Red Faction Guerrilla Re-Mars-tered (RFG:R)**, Windows x64, D3D11, Volition's proprietary engine (Geo-Mod 2.0), third-person over-the-shoulder shooter. Goal: full 6-DOF VR with motion controllers via an injected DLL using **OpenXR** (Valve Index / SteamVR OpenXR runtime).

Research date: 2026-09-25. Every URL listed here was fetched, returned HTTP 200 from a script, or came back in search results during this session. Where a page could not be fetched (Reddit and Nexus block scripted fetches), this is stated. License data comes from the GitHub API `spdx_id` field or the repo's LICENSE file. "NOASSERTION" means GitHub could not classify the license, so read the LICENSE file before reusing code.

---

## 0. TL;DR / Key takeaways

1. **No open-source VR mod exists for a native, closed-source, D3D11, third-person engine that we can fork as-is.** The closest *architectural* references are:
   - **praydog/REFramework** (MIT). Its VR code is engine-agnostic: D3D11/D3D12 components, an OpenXR+OpenVR runtime abstraction and AFR submission. Several third-party native-engine VR mods are built on it: **mutars/anvilengine2vr** (Assassin's Creed, a *third-person* game, MIT), **mutars/starfield2vr** (MIT), and the extracted **elliotttate/vrframework** (MIT), which comes with a 17-chapter field guide. **This is the recommended code base to borrow from.**
   - **praydog/UEVR**: study it for its ideas (rendering modes, UI projection, aim methods, UObjectHook attachment). Its LICENSE is "Copyright (c) 2022-2025 praydog. All rights reserved.", so **do not copy code from it**.
   - **MotherVR** (Alien: Isolation) is conceptually very relevant: a dxgi.dll proxy that patches statically linked Oculus SDK calls and adds assembly-level hooks. **Its GitHub repo contains only a README, with no source and no license.** It is useful as an idea, not as code.
   - Recent (2025-2026) open-source native-engine OpenXR mods worth reading: **PreyVR** (CryEngine, D3D11, OpenXR, MinHook, MIT), **MGS5VR** (Fox Engine, *third-person to first-person*, same-frame stereo, MIT), **dariulone/cyberpunk-vr-port** (a real second engine camera, VRIK, MIT), **theHunterCotW-VR** (GPL-3.0, has an excellent flicker postmortem), and **Dishonored-VR** (UE3 via a d3d9-to-DXVK proxy).
2. **Stereo strategy is the #1 architecture decision.** From best quality to most compatible:
   - native stereo
   - **synchronized sequential**: render both eyes on the same simulation tick
   - **AFR**: one eye per engine frame
   - **AER**: Luke Ross's AFR plus reprojection

   AFR is by far the easiest to get running (REFramework, anvil and starfield all use it), but it produces eye desync, ghosting and TAA smearing. UEVR's docs warn it "usually" causes nausea. **For RFG:R, a fast-moving destruction game full of debris and particles, the recommendation is: (a) bring up the pipeline with AFR, then (b) move to synchronized double rendering by re-invoking the engine's scene-render function twice per tick.** Once per-frame state is audited, that path gives the best result.
3. **Camera injection must go where the engine's camera actually lives**, not only into the projection matrix. The engine culls, sets LOD, builds per-light scissor rects and so on from its own CPU-side camera. A projection-only eye shift gives wrong culling and lighting per eye ("the void behind the player", lights lit in one eye only).
4. **Use the HMD's asymmetric per-eye frustum (XrFovf tangents)** to build an off-center projection. Keep the engine's own near and far planes. Put the IPD offset in the view matrix. Filter the main camera from shadow and reflection cameras with a discriminator such as far-plane size or render-target size.
5. **A frame has three stereo regimes that need different transforms:**
   - world geometry: per-eye offset
   - near-field viewmodel or held weapon: a smaller separation, or its own projection
   - screen-space 2D/HUD: fixed depth, the same in both eyes, ideally re-rendered to an offscreen RT and composited as an **OpenXR quad layer** or a world-space quad
6. **Anything that advances per frame must advance once per stereo pair, not once per eye.** This covers particles, the wind/shader clock, shadow cascade fitting, TAA history, random seeds and audio. Violations show up as flickering shadows and per-eye grain. This is critical for Geo-Mod debris and particles.
7. **Third-person to first-person conversion is its own project.** It is done by REFramework for RE2/RE3, MGS5VR, anvilengine2vr and Luke Ross's mods. You need:
   - the camera at the head bone, with the head/neck hidden or shrunk
   - aim decoupled from the camera
   - weapon attached to the controller, with the muzzle/ray redirected
   - arms via IK (VRIK-style), or simply "floating hands"
8. **Use OpenXR, not OpenVR, for per-eye poses.** On OpenVR, `IVRCompositor::Submit` keeps only the pose from the last per-eye call (LukeRoss00's openvr issue #1253, still open). OpenXR's projection layer carries a pose and FOV per view. However, LukeRoss00 reported in 2020 that SteamVR's OpenXR runtime mishandled spec-correct per-view poses (wrong baseline plus vertical offset), so **test it on the current SteamVR runtime early**.
9. **Tooling:**
   - Hooking: SafetyHook (BSL-1.0; also used by REFramework and UEVR) or MinHook
   - Graphics debugging: RenderDoc
   - Reverse engineering: Cheat Engine, ReClass.NET, x64dbg, Ghidra
   - Loading: a proxy DLL (dxgi.dll / d3d11.dll / version.dll / dinput8.dll) or the Ultimate ASI Loader
   - Overlays: ImGui
   - Headset-free testing: **VRto3D** (a SteamVR driver that shows SbS on a monitor) and **phunkaeg/xr-sim**, an MIT-licensed headless OpenXR runtime
10. **RFG-specific community code already exists:** `rfg-modding/RSL` (GPL-2.0, Lua script loader with game struct and hook knowledge) and `rfg-modding/Reconstructor` (MPL-2.0, the RSL2 rewrite). Mine them for addresses and structs such as camera, player and UI. Mind the licenses.

---

## 1. Communities and people

| Resource | URL | What it is | Relevance | Notes / takeaways |
|---|---|---|---|---|
| Flat2VR Modding Discord | https://discord.com/invite/flat2vr | The main flat-to-VR community (~160k members per search snippet). Has channels per framework (UEVR, REFramework, etc.) | **High** | The best place to ask about native-engine hooks, share WIP builds and recruit testers. Most modders listed below hang out here. |
| Flat2VR on X | https://x.com/Flat2VR | Announcements | Low | News only. |
| Flat2VR Modding Group (Notion) | https://beastsaber.notion.site/Flat2VR-Modding-Group-8eb9ae0535144eac843f428abb104de9 | Community wiki/landing page | Med | Index of mods. |
| Flat2VR Studios | https://www.flat2vrstudios.com/ | Commercial studio spun out of the community (founded 2024 by Impact Reality) doing *licensed* ports (Trombone Champ: Unflattened, WRATH, Roboquest, FlatOut) | Low (technical) / Med (strategic) | Shows the path from mod to official port. Irrelevant for tech. |
| Beyond Flatscreen mod hub | https://beyondflatscreen.vercel.app/ | Community VR-mod index | Low | Use it to check if anyone has attempted RFG VR. |
| r/flatscreentoVR, r/flat2vr | https://www.reddit.com/r/flatscreentoVR/ | Subreddits | Low-Med | Reddit blocked scripted fetch (403); existence not independently verified here. |
| Team Beef (DrBeef) | https://www.teambeefvr.com/ , https://github.com/Team-Beef-Studios | Source ports of id Tech/Doom-era engines to Quest (RTCWQuest, Quake3Quest, QuestZDoom), now OpenXR | Low-Med | Source-owned route (they have engine source). Good reference for weapon/hand ergonomics and OpenXR input, not for injection. |
| praydog | https://github.com/praydog | Author of REFramework and UEVR | **High** | See section 2. |
| Raicuparta | https://raicuparta.com/ , https://github.com/raicuparta | Unity VR mods (NomaiVR, Two Forks VR), UUVR, Rai Pal | Med | Unity-specific code, but excellent UX/comfort design. |
| Luke Ross (R.E.A.L. VR) | https://www.patreon.com/realvr , https://github.com/LukeRoss00 | Per-game closed-source VR mods for AAA native engines using AER | **High (technique)** | See section 2.3. |
| mbucchia (Matthieu Bucchianeri) | https://github.com/mbucchia | OpenXR expert: OpenXR-Toolkit, Quad-Views-Foveated, many API layers | Med | Reference for OpenXR API layers, D3D11 interop and foveation. |
| CrossVR (Jules Blok) | https://github.com/CrossVR | LibreVR/Revive (Oculus-to-OpenVR translation), Dolphin VR contributions | Low-Med | Revive is a good reference for translating one VR API into another and for D3D11 texture sharing. |
| TefMeister | https://github.com/TefMeister/flat-to-vr-cross-engine-research | 2026 cross-engine flat-to-VR research corpus (CC-BY-4.0) with dozens of per-game repos (far-cry-2-vr, the-darkness-vr, XIII2003-vr, ...) | **High** | A huge techniques doc (~7,000 lines). Heavily evidence-graded, and much of it is `[reported]`/`[inferred]` rather than headset-verified, so verify claims. |
| phunkaeg | https://github.com/phunkaeg/vr-modding-playbook | "Evidence-graded engineering playbook for VR ports" (MIT). Also PreyVR, SOMAVR, xr-sim, xr-tape | **High** | Failure atlas (symptom to cause), pattern catalog, cross-project index (~107 projects). |
| RFG modding community | https://github.com/rfg-modding , https://rsl.readthedocs.io/ | RSL (Lua script loader, GPL-2.0) and Reconstructor (RSL2 rewrite, community patch, MPL-2.0) | **High** | Existing reverse-engineering of RFG:R: function addresses, structs and hook points. Read before reversing from scratch. |

---

## 2. Frameworks and reference mods

### 2.1 praydog: REFramework and UEVR

| Item | URL | License | Relevance |
|---|---|---|---|
| REFramework | https://github.com/praydog/REFramework | **MIT** | **High**, the best reusable code base |
| REFramework nightly | https://github.com/praydog/REFramework-nightly/releases | same | Med |
| UEVR | https://github.com/praydog/UEVR | **"All rights reserved"** (LICENSE file) | **High for ideas, do not copy code** |
| UEVR docs | https://docs.uevr.io/ (moved from praydog.github.io/uevr-docs) | n/a | High |

**REFramework VR architecture** (verified from the repo tree and source):
- `src/hooks/D3D11Hook.cpp`, `D3D12Hook.cpp`, `DInputHook`, `XInputHook` and `WindowsMessageHook` intercept graphics and input.
- `src/mods/VR.cpp` holds the central VR mod: frame counters, poses and eye selection.
- `src/mods/vr/D3D11Component.cpp` covers D3D11 submission. Its logic:
  1. Copy the backbuffer.
  2. Choose the eye by parity: `if (vr->m_render_frame_count % 2 == vr->m_left_eye_interval)`, which is AFR.
  3. `CopyResource` into per-eye textures.
  4. Submit through OpenVR, or copy into OpenXR swapchain images, then `end_frame()`.

  It includes a full `DX11StateBackup` RAII class that saves and restores pipeline state around the mod's own draws. Copy this pattern.
- `src/mods/vr/runtimes/{VRRuntime.hpp, OpenXR.cpp, OpenVR.cpp}` is the runtime abstraction. `OpenXR::synchronize_frame()` calls `xrWaitFrame`, then `xrLocateViews` for both view space and stage space, with a tunable `prediction_scale` on `predictedDisplayTime`. `begin_frame()`/`end_frame()` wrap `xrBeginFrame`/`xrEndFrame`, and there is a "synchronize stage" (early/late/very late) that controls *where in the engine frame* `xrWaitFrame` is called.
- `src/mods/vr/OverlayComponent` handles UI and ImGui overlays as OpenXR/OpenVR overlay or quad layers.
- `src/mods/vr/games/RE8VR.cpp` holds game-specific motion-control logic.

**UEVR concepts worth replicating** (from https://docs.uevr.io/ and https://docs.uevr.io/usage/adding_6dof.html):
- **Three rendering modes.**
  - *Native Stereo* ("when it works, it looks the best, performs the best").
  - *Synchronized Sequential* renders "two frames sequentially in a synchronized fashion on the same engine tick". The game world does not advance time between the eyes, but TAA ghosts and motion blur must be off. It comes in *Skip Draw* and *Skip Tick* variants.
  - *AFR*, where the world advances between eyes. The docs say it causes "eye desyncs and usually nausea".
- **Decoupled Pitch** flattens the view pitch/roll applied to the character so the horizon stays level. It is a comfort option.
- **UI:** the game UI is rendered to a separate target and projected into 3D space as a quad. The motion controller can act as a mouse pointer on menus.
- **Aim methods:** Game / Head / Right controller / Left controller.
- **UObjectHook:** find the weapon or hand mesh component, "Attach right/left", adjust the offset in-headset, then "Permanent change". This is a generic *attach-engine-object-to-controller* tool driven by the engine's object hierarchy. For RFG we would need the equivalent: find the weapon's world transform and override it each frame.
- **Depth pass-through** (`VR_PassDepthToRuntime`) submits depth to the runtime. It improves reprojection and latency on some runtimes.
- **Community add-on:** https://github.com/Alex3DStereo/UEVR_shaderhacking, UEVR fused with 3DMigoto-style shader fixes for broken screen-space effects.

### 2.2 Code derived from REFramework for native engines (the most transferable)

| Item | URL | License | What | Relevance |
|---|---|---|---|---|
| vrframework | https://github.com/elliotttate/vrframework | MIT (upstream REFramework copyright kept; GitHub shows NOASSERTION) | Engine-agnostic VR core extracted from REFramework (`IEngineAdapter`, `FrameTimeline`, `StereoView` SPI) plus a **17-chapter guide**: 02 injection, 03 hooking and pattern scanning, 04 graphics API interception, 06 VR runtime, 07 frame timing, 08 stereo strategies, 09 camera/coordinates, 10 submission and TAA, 11 HUD/UI, 12 input/controllers, 16 porting checklist | **High**: essentially a textbook for exactly our project |
| anvilengine2vr | https://github.com/mutars/anvilengine2vr | MIT | OpenXR VR for Assassin's Creed Odyssey/Valhalla/Mirage (AnvilNext 2.0). **Third-person** games. dxgi.dll hook, 6DOF, head aim, HUD scale, AFR. TAA byte-patched off. Main camera filtered with `farPlane > 1201.f`. Hooks `onCalcProjection`/`onCalcFinalView` and rewrites out-params | **High**: closest analogue (proprietary engine, third-person, dxgi proxy, OpenXR) |
| starfield2vr | https://github.com/mutars/starfield2vr | MIT | Creation Engine 2. **Frame timing from NVIDIA Reflex markers**. Per-eye TAA history ping-pong (keeps TAA/DLSS working under AFR). Patches the view in the GPU constant-buffer struct. Uses **ViGEmBus** to present motion controllers as a virtual gamepad | High |

Key excerpt from vrframework ch. 08, the per-eye off-axis projection from runtime tangents (Anvil):
```
P = [ 2/(r-l)        0              0                 0
      0              2/(t-b)        0                 0
      (r+l)/(r-l)    (t+b)/(t-b)    f/(n-f)          -1
      0              0              n*f/(n-f)         0 ]   // l,r,t,b = tan(FOV angles); n,f = ENGINE near/far
```
Their key points:
- The skew terms `(r+l)/(r-l)` are "the whole point". A symmetric `perspective(fov)` "looks fine on a monitor and is subtly wrong in the headset".
- Keep the engine's near and far planes.
- Put the IPD in the view matrix.
- Adapt the matrix layout (row/column major, handedness, reversed Z) to your engine.

vrframework ch. 07 (frame timing) argues that an AFR mod must keep three counters in lockstep: **engine**, **render** and **presenter** frame counters. The HMD pose is sampled at the "wait/render" edge. You detect parity slip and schedule a corrective skip. If Reflex markers or a frame-index global exist, use them as the clock. Otherwise hook the engine's "begin engine frame" and "begin render frame" functions.

### 2.3 Luke Ross: R.E.A.L. VR (AER)

| Item | URL | License | Relevance |
|---|---|---|---|
| GTA V R.E.A.L. mod (archived) | https://github.com/LukeRoss00/gta5-real-mod | no license stated; abandoned 2022 after Take-Two pressure | **High (technique)** |
| Patreon (current mods: Cyberpunk, RDR2, Elden Ring, Hogwarts, Spider-Man, Uncharted 4, KCD2, ...) | https://www.patreon.com/realvr | closed source | Med |
| OpenVR issue #1253 (per-eye pose bug) | https://github.com/ValveSoftware/openvr/issues/1253 | n/a | **High** |
| SteamVR OpenXR per-view pose report (2020) | https://steamcommunity.com/app/250820/discussions/8/3001046778344834329/ | n/a | **High**: test this early |
| Overview article | https://compoundvr.com/articles/luke-ross-real-vr-guide/ | n/a | Low |
| News on AER v2 / DLSS | https://mixed-news.com/en/real-vr-mod-dlss-ray-reconstruction/ | n/a | Low |

Takeaways:
- **AER (Alternate Eye Rendering):** render one eye per frame and display the previous frame for the other eye, with reprojection. The engine only needs 90 fps for 90 Hz, but one eye always carries ~11 ms extra latency, so fast lateral motion doubles. AER v2 (March 2023) greatly reduced the ghosting.
- The GTA V README describes how it is done:
  - FOV patched to match the HMD
  - camera position and rotation matrices overridden
  - **3DMigoto shader manipulation** plus script hooks
  - dynamic crosshair depth
  - HUD drawn semi-transparent ~1 m ahead
  - recenter via a head-shake gesture
  - square render resolution required, with supersampling via frame scaling
- **No motion controls** in any R.E.A.L. mod. That choice is deliberate: per-game hand/IK/weapon work is expensive. **We need controllers, so R.E.A.L. is a model for stereo and camera only.**
- **Legal lesson:** Take-Two's hostility ended the GTA V mod. Keep the mod free, contain no game assets, and avoid monetization tied to a publisher's IP (Luke Ross's Patreon model has drawn publisher attention).

### 2.4 MotherVR (Alien: Isolation), a native proprietary engine on D3D11

| Item | URL | License | Relevance |
|---|---|---|---|
| MotherVR repo | https://github.com/Nibre/MotherVR | **No license. The repo contains only README.md (checked via the GitHub tree API), so no source** | **High (concept), no code** |
| Road to VR interview | https://roadtovr.com/mothervr-mod-alien-isolation-oculus-rift-nibre-zack-fannon/ | n/a | High |
| GRAND-MotherVR (adds hands) | https://www.uploadvr.com/alien-isolations-new-grand-mothervr-mod-adds-hands-brings-qol-improvements/ | n/a | Med |

Takeaways:
- It installs as a **`dxgi.dll` proxy**, and VR is toggled from the in-game Options menu, so the mod hooks the game's own menu system.
- Alien: Isolation shipped with *dormant* DK2-era Oculus SDK code statically linked. Nibre "patch[ed] every SDK call in the game when it launches, to redirect it to my own 'fake' SDK code", plus assembly-level hooks "from non-SDK game code and reading raw memory".
- He fixed 2014-era comfort problems: mandatory smooth turning, head-locked aiming, broken recentering and body positioning.
- **Lesson: before building stereo from scratch, grep the RFG:R binary for leftover stereo/VR code** (3D Vision / NVAPI stereo strings, `stereo` cvars, `StereoOffset` constants). The TefMeister corpus documents how often such dormant paths exist, and how often they are unreachable.

### 2.5 Other native-engine VR mods with source (2024-2026)

| Mod | URL | Engine / API | License | Technique highlights | Relevance |
|---|---|---|---|---|---|
| **PreyVR** | https://github.com/phunkaeg/PreyVR | CryEngine (Prey 2017), x64, D3D11, OpenXR 1.1, MinHook vendored | MIT | Stereo, 6DOF, controller hands through **the engine's own IK solver**, native HUD fixed by viewport-fraction correction (the 16:9 HUD canvas is scaled to cover the frame). Shots converge from the authored muzzle helper toward the reticle ray. Built-in OpenXR timing instrumentation (p50/p95/p99). The build refuses to run against unrecognized game versions. Perf note: "cutting pixel count by 44% bought 2.7% of the frame, so this is not pixel-bound" (CPU-bound) | **High** |
| **MGS5VR** | https://github.com/nikamigaming-create/MGS5VR | Fox Engine, D3D11, OpenXR, dinput8 proxy | MIT | **Third-person game converted to first-person**, *same-frame* stereo, first-person rig plus arm HUD (in progress), remappable INI controls with live reload, native smooth turn, recenter. DoF and motion blur forced off | **High**: the most similar genre problem |
| **cyberpunk-vr-port** | https://github.com/dariulone/cyberpunk-vr-port | REDengine 4 via RED4ext | MIT | The second eye is **a real second engine camera** (render-to-texture camera running the frame graph), not reprojection. **Sun shadow cascades, shader clock, foliage wind and reflection march are computed once and shared** across both views. Cameras are identified by name hash. VRIK full body, physical reload, "bullets follow the real weapon muzzle, not the camera". The HUD composite is re-run per eye. For off-axis lenses the frustum is sized to cover the panel | **High** (design) |
| Cyberpunk2077-VR-OpenSource | https://github.com/pinducoding/Cyberpunk2077-VR-OpenSource | REDengine 4, OpenXR | MIT | AER-based, RED4ext plugin | Med |
| **theHunterCotW-VR** | https://github.com/vaas993/theHunterCotW-VR | Apex engine, D3D11, OpenXR | GPL-3.0 | Full-rate stereo *or* AER, per-eye smoothing, live DLSS control, **flicker postmortem**: https://github.com/vaas993/theHunterCotW-VR/blob/main/docs/THE_FLICKER_POSTMORTEM.md | High (lessons) |
| **Dishonored-VR** | https://github.com/GingasVRFO/Dishonored-VR (maintained fork: https://github.com/VR-Stereo-Hub/Dishonored-VR) | UE3, D3D9 translated to Vulkan by a **forked DXVK** used as a `d3d9.dll` proxy | see repo | True stereo into a single 4032x2268 side-by-side target, 6DOF, both hands on the game's own animation rig, roomscale through game collision, real-life crouch, wrist HUD, per-eye shadows, sunshafts and reflections, F10 in-headset overlay. The author quit from burnout: "I'm burned out on trying to fix problems I can't reproduce" | Med-High |
| anvilengine2vr / starfield2vr | see 2.2 | | MIT | | High |
| MGS5VR/FNV and ~20 TefMeister per-game repos (far-cry-2-vr, the-darkness-vr, XIII2003-vr, psychonauts-vr, ...) | https://github.com/TefMeister/far-cry-2-vr , https://github.com/TefMeister/the-darkness-vr | various | see each | Many are research-grade, and much is unverified in a headset | Med |

### 2.6 Source-available engine VR ports (source-owned route)

These have engine source, so they show *what* to build, not *how* to inject.

| Mod | URL | Relevance | Takeaway |
|---|---|---|---|
| Half-Life 2: VR Mod (Source VR Mod Team) | https://store.steampowered.com/app/658920/HalfLife_2_VR_Mod/ | Med | The gold standard for FPS VR UX: weapon handling, holsters, comfort options |
| HL2VRU (fork) | https://github.com/vittorioromeo/HL2VRU | Med | Open-source VR-only interactions |
| Half-Life VR | https://github.com/maxmakesmods/Half-Life-VR | Low-Med | GoldSrc VR with source |
| DOOM 3 BFG VR: Fully Possessed | https://github.com/KozGit/DOOM-3-BFG-VR (**now 404**; seen in search results only) | Low | Repo appears to have been removed |
| Team Beef ports (RTCWQuest etc.) | https://github.com/Team-Beef-Studios/RTCWQuest | Low-Med | OpenXR input/weapon code in an id Tech 3 engine |

### 2.7 Unity (Raicuparta): UX lessons only

| Item | URL | License | Relevance |
|---|---|---|---|
| NomaiVR (Outer Wilds) | https://github.com/Raicuparta/nomai-vr | MIT | Low (code) / Med (UX) |
| Two Forks VR (Firewatch) | https://github.com/Raicuparta/two-forks-vr | MIT | Low / Med |
| UUVR (universal Unity VR) | https://github.com/Raicuparta/uuvr | GPL-3.0 | Low |
| Rai Pal (universal mod manager, Rust) | https://github.com/Raicuparta/rai-pal | GPL-3.0 | Low (distribution idea) |

Takeaways: Raicuparta's mods show strong comfort and UX design (laser pointers for menus, hand-attached tools, comfort toggles). The code is BepInEx/Harmony C#, which does not transfer to a native engine.

### 2.8 Generic stereo drivers and shader tools

| Tool | URL | License | Technique | Relevance |
|---|---|---|---|---|
| **3DMigoto** | https://github.com/bo3b/3Dmigoto | custom (GitHub NOASSERTION) | A d3d11.dll wrapper (`HackerDevice`/`HackerContext`) for **finding and replacing shaders at runtime**: hunting VS/PS by hash, dumping and fixing HLSL, INI-driven resource copies | **High as a tool**: find RFG's camera constant buffers, HUD shaders and post-FX shaders, and prototype shader fixes. Also a reference for wrapping D3D11 |
| geo-11 | https://github.com/ThreeDeeJay/geo-11 (binaries), https://helixmod.blogspot.com/2022/06/announcing-new-geo-11-3d-driver.html | binaries; source "restrictive/non-commercial" planned | Built on 3DMigoto. Doubles DX11 draw calls with a clip-space stereo shift (3D-Vision-Automatic style). No head tracking (for VR it is paired with Katanga/VRScreenCap) | Med: shows that "draw call doubling" stereo is feasible on DX11, and has per-game fixes |
| Helix Mod blog | https://helixmod.blogspot.com/ | n/a | Per-game stereo shader fixes | Low-Med |
| vorpX | https://www.vorpx.com/features/ , https://www.vorpx.com/more-headtracking-z-buffer-vs-geometry-3d/ | commercial | **Z-buffer 3D** (depth-based reprojection of a mono frame: ~2x faster, weaker depth, artifacts) vs **Geometry 3D** (true second camera: ~50% FPS cost, best depth, but shader/UI misalignments). Head tracking via mouse emulation. EdgePeek/Immersive Screen for third-person games | Low-Med: shows the fallback option (depth reprojection) |
| VRto3D | https://github.com/oneup03/VRto3D | see repo | OpenVR driver that presents a VR app as SbS/TaB 3D on a monitor | **Med: dev tool for testing stereo without wearing the HMD** (OpenVR only) |
| UEVR_shaderhacking | https://github.com/Alex3DStereo/UEVR_shaderhacking | see repo | UEVR plus 3DMigoto-style fixes | Low-Med |

### 2.9 OpenXR runtime-side tools

| Tool | URL | License | Use |
|---|---|---|---|
| OpenXR SDK + `hello_xr` (D3D11 plugin) | https://github.com/KhronosGroup/OpenXR-SDK-Source (see `src/tests/hello_xr/graphicsplugin_d3d11.cpp`) | Apache-2.0 | **Reference for session, swapchain and D3D11 binding (`XR_KHR_D3D11_enable`)** |
| OpenXR-Toolkit (mbucchia) | https://github.com/mbucchia/OpenXR-Toolkit | MIT | FSR/NIS upscaling, fixed foveated rendering (VRS), overlays. Useful to users *and* as code reference for API layers |
| Quad-Views-Foveated | https://github.com/mbucchia/Quad-Views-Foveated | see repo | Only helps apps that implement quad views; "not a universal injector" |
| OpenComposite | https://gitlab.com/znixian/OpenOVR | see repo | OpenVR-to-OpenXR translation (not needed if we target OpenXR natively) |
| Revive | https://github.com/LibreVR/Revive | see repo | Oculus-to-OpenVR translation; a reference for API translation |
| xr-sim / xr-tape (phunkaeg) | https://github.com/phunkaeg/xr-sim , https://github.com/phunkaeg/xr-tape | MIT / see repo | **Headset-free deterministic OpenXR runtime** plus an API-layer recorder and submission checker. Good for CI and for debugging without the Index |

---

## 3. Injection, hooking and reverse-engineering toolbox

### 3.1 Loading the DLL

| Method | Notes | Reference |
|---|---|---|
| **Proxy DLL** (`dxgi.dll`, `d3d11.dll`, `version.dll`, `dinput8.dll`, `winmm.dll`, `xinput*`) | Drop it in the game folder. It forwards all exports to the system DLL (loaded from System32 by full path) and starts the mod. **Must export every function the game imports from that DLL.** Check the game's import table (e.g. `dumpbin /imports`). A `dxgi.dll` proxy (MotherVR, anvilengine2vr) loads early enough to hook `CreateDXGIFactory*` and `D3D11CreateDevice(AndSwapChain)` before device creation. | MotherVR, anvilengine2vr, MGS5VR (dinput8), REFramework (dinput8) |
| **Ultimate ASI Loader** | A proxy that loads `*.asi` plugins. Supports d3d8-12, dxgi, dinput8, version, winmm, winhttp and more. The original DLL can be chained as `<name>Hooked.dll`. | https://github.com/ThirteenAG/Ultimate-ASI-Loader (MIT) |
| External injector (CreateRemoteThread/LoadLibrary) | Used by UEVR's frontend. Easier to debug, but too late to hook device creation. D3D hooks must then use the vtable-from-dummy-device technique. | UEVR |

**RFG:R note:** if the RSL/Reconstructor loaders also use a proxy DLL, pick a different proxy name or chain loaders so the mods co-exist. MGS5VR explicitly refuses to overwrite another mod's `dinput8.dll`.

### 3.2 Hooking libraries

| Library | URL | License | Notes |
|---|---|---|---|
| **SafetyHook** | https://github.com/cursey/safetyhook | BSL-1.0 | C++23. InlineHook, **MidHook** (hook at any instruction with full register context, great for grabbing a camera matrix mid-function), VmtHook/VmHook. It fixes up thread IPs and RIP-relative operands. Used by REFramework, UEVR and starfield2vr. **Recommended.** |
| MinHook | https://github.com/TsudaKageyu/minhook | BSD-2-Clause | Simple and battle-tested. Used by PreyVR and kiero. |
| Microsoft Detours | https://github.com/microsoft/Detours | MIT | Official, and includes an import/export rewriting helper. |
| kiero | https://github.com/Rebzzel/kiero | MIT | Gets D3D9-12/GL/Vulkan vtables by creating a dummy device, then hooks Present/ResizeBuffers etc. with MinHook. Good for a quick start. A proxy on dxgi is more robust. |
| Zydis | https://github.com/zyantific/zydis | MIT | Disassembler (used inside SafetyHook). Useful for pattern-scan plus decode of RIP-relative globals. |
| Dear ImGui | https://github.com/ocornut/imgui | MIT | Debug and settings overlay, rendered to a desktop window and an XR quad layer. |

**Pattern scanning:** find functions and globals by AOB signatures with wildcards, and resolve RIP-relative displacements, so the mod survives patches. RFG:R is an old, stable binary (the Steam and GOG builds may differ). Still verify the exe hash/size at startup and refuse unknown builds, as PreyVR does.

### 3.3 Reverse-engineering and graphics debugging

| Tool | URL | Use for VR modding |
|---|---|---|
| **RenderDoc** | https://renderdoc.org/ | Capture a frame and list the passes: shadow cascades, G-buffer, lighting, post, UI. Identify the **camera constant buffer** (view, proj, viewProj, invViewProj, camera position), the **UI pass** and the **backbuffer copy points**. Check the depth format and reversed-Z. |
| **Cheat Engine** | https://github.com/cheat-engine/cheat-engine | Search memory by known values (camera position/FOV), find what writes or accesses the camera struct, and freeze values to test effect. |
| **ReClass.NET** | https://github.com/ReClassNET/ReClass.NET | Map the camera, player, weapon and UI structs. |
| **x64dbg** | https://github.com/x64dbg/x64dbg | Breakpoints on the matrix writers, and tracing the render loop and frame boundaries. |
| **Ghidra** | https://github.com/NationalSecurityAgency/ghidra | Static analysis: strings (look for "stereo", "fov", "camera", debug names), vtables and call graphs. |
| 3DMigoto (hunting mode) | https://github.com/bo3b/3Dmigoto | Cycle through VS/PS live to identify HUD, crosshair, post-FX and shadow shaders by hash. Dump the constant buffers. |
| apitrace-mcp (phunkaeg) | https://github.com/phunkaeg/apitrace-mcp | Targets older APIs (D3D7-9/GL). Not D3D11, so low relevance. |

**Camera-hunting recipe** (vrframework ch. 09, TefMeister "finding the camera matrix the engine actually reads"):
1. Find a value the game shows you (player position from a debug overlay or Lua via RSL). Scan memory for those floats. Searching by value beats searching by address, because constant data lives in ring buffers that are recycled per frame.
2. Test for *effect*, not persistence. Write an extreme value and check the picture. Many matrices near the camera are derived outputs that nothing reads back.
3. Identify a view matrix arithmetically: its translation row equals `-R * cameraPos`. Check the row/column-major order two ways.
4. When overriding, snapshot and write **absolute** values each frame. Rotating the current value by an offset compounds into a spin. Rotate all basis vectors *and* the translation together.
5. If the culling/void problem appears, you are writing a derived copy, not the source.

---

## 4. Stereo rendering: technique decision matrix for RFG:R

Context that drives the choice:
- RFG:R is D3D11 (immediate context, single-threaded submission likely; verify).
- It is CPU-heavy: Geo-Mod physics, debris and many particles. GPU cost at 2x Index resolution will be significant.
- It is a third-person shooter, so we must convert it to first person.

| Strategy | How it works | Pros | Cons | Effort | Verdict for RFG:R |
|---|---|---|---|---|---|
| **A. AFR** (REFramework, anvil, starfield, Cyberpunk AER mods) | Even frame = left eye, odd = right. Inject per-eye view+proj into the engine camera each frame, copy the backbuffer into that eye's swapchain image. Submit both at the end of the pair, or use per-eye predicted poses | Needs only the camera hook plus a Present hook. The engine renders one view, exactly as designed. Least engine knowledge required | The world advances between eyes, so fast debris, explosions and vehicles **ghost/double**. Each eye updates at half rate (engine must run at 180 fps for a 90 Hz AFR stereo pair; otherwise effective 45 Hz per eye). **TAA/temporal effects smear** (history comes from the other eye). Parity slip swaps the eyes. Eye-labeling off-by-one bugs | Low-Med | **Milestone 1 bring-up only.** Proves the camera, projection, OpenXR and swapchain path. |
| **B. AER** (Luke Ross) | AFR plus reprojection of the stale eye using the pose delta, plus "yaw folding" | Much less ghosting than naive AFR at the same cost | Complex. Needs per-eye pose submission that the runtime honors (OpenVR cannot; SteamVR OpenXR must be tested). Still has latency asymmetry | High | Optional improvement if C proves impossible. |
| **C. Synchronized sequential / double render** (UEVR Sync Sequential, cyberpunk-vr-port second camera, geo-11-style draw doubling) | In one sim tick, call the engine's *scene render* function twice (or run the frame's render twice) with left and right cameras into two targets. Share once-per-frame state | Both eyes show the **same simulation instant**. No temporal eye desync, correct stereo for debris. TAA can work with per-eye history | ~2x GPU and render-thread CPU. Must find a re-entrant render entry point. Must stop per-frame state from double-advancing (particles, clocks, shadow fits, GPU queries, streaming). Screen-space effects may assume one view per frame | High | **Target architecture.** Find RFG's "render world view" function via RenderDoc plus a debugger and re-invoke it per eye. |
| **D. Single-pass instanced / multiview** | Double the instance count in draw calls and route by `SV_ViewportArrayIndex`/`SV_RenderTargetArrayIndex` | Cheapest GPU stereo | Requires patching **every** vertex shader plus draw calls. Not practical in a closed D3D11 engine | Very high | Not recommended. |
| **E. Clip-space draw-call doubling** (3D Vision Automatic / geo-11) | Wrap D3D11. Per draw, issue it twice with `clip.x += sep*(clip.w - conv)` patched into each VS, into per-eye RTs | No camera hunt needed for stereo | **No 6DOF** (only horizontal separation; no rotation/position). Deferred lighting and post-FX break (they unproject). Needs per-shader exclusions (HUD, skybox, full-screen quads) | High | Not suitable as the primary method for 6DOF. Useful only as a diagnostic. |
| **F. Depth-buffer reprojection** (vorpX Z3D) | Render mono, synthesize the second eye from depth | ~1x cost | Weak depth, disocclusion artifacts around edges (bad for near weapon and hands) | Med | Emergency fallback only. |
| **G. Dormant engine stereo path** | Enable leftover 3D Vision/stereo code if it exists | Engine-authored two views | Often unreachable or tied to dead NVAPI 3D Vision, and no head tracking | Low to check | **Check first** with a Ghidra string search on the RFG:R exe (e.g. "stereo", "NvAPI_Stereo", "3DVision"). |

**Camera-injection placement** (sub-decision):

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Overwrite engine camera object (pos/orientation/FOV) *before* culling | Culling, LOD, lighting rects and shadow cascades all follow the eye | Must find the right struct and timing | **Preferred** |
| Hook the matrix-building function out-params (anvil: `onCalcProjection`, `onCalcFinalView`) | Precise, one place | Must still make sure culling uses the same view | Preferred for projection |
| Patch the GPU constant buffer after the engine fills it (starfield) | Doesn't need the CPU camera | CPU-side culling and LOD see the mono camera, so there is a "void"/pop-in at the edges when looking away from the game camera. Multiple CB write paths must all be covered | Fallback |
| Projection-only eye shift | Fast first picture | Wrong per-eye lighting and culling (the-darkness-vr: lights lit in one eye only) | Prototype only |

**Runtime and API sub-decision:**

| Option | Verdict |
|---|---|
| OpenXR via `XR_KHR_D3D11_enable`, a projection layer with 2 views, and a quad layer for HUD/menus | **Use this.** It carries a pose and FOV per view, and the Index works through the SteamVR OpenXR runtime. Test per-view pose correctness on current SteamVR (vertical-offset symptom). |
| OpenVR | Avoid. Per-eye pose submission collapses to the last call (issue #1253). |
| Submit depth (`XR_KHR_composition_layer_depth`) | Nice-to-have for reprojection quality (UEVR `VR_PassDepthToRuntime`). |

**Swapchain and format notes** (REFramework D3D11Component):
- Create per-eye textures matching the backbuffer format. Handle non-8-bit backbuffers by rendering through an RTV/SRV with an explicit format.
- Watch sRGB. PreyVR found "gamma is a swapchain format, not a cvar", and the order of init mattered.
- Wrap every mod draw in a full D3D11 state backup/restore.

---

## 5. Other technique areas

### 5.1 Frame timing and pacing
- Call `xrWaitFrame` at a consistent point, sample poses (`xrLocateViews` at `predictedDisplayTime`) as late as possible before the engine builds its camera, and call `xrEndFrame` after the eye image(s) are copied. REFramework exposes early/late/very-late sync stages because the ideal point is engine-specific.
- Find the engine's frame boundaries. Use existing instrumentation first (Reflex markers, frame-index globals). Otherwise hook "begin sim frame" and "begin render frame" (vrframework ch. 07, anvil).
- Turn off in-game VSync and frame caps. The runtime's `xrWaitFrame` becomes the throttle (anvil and starfield instruct Windowed / VSync Off / Frame cap Off).
- Motion smoothing / ASW interacts badly with AER (Luke Ross advises disabling it). With synchronized stereo it is an acceptable safety net.
- Performance: supersample through `xrSwapchain` size or a resolution-scale setting, and provide a runtime slider (starfield2vr: 0.1x-5.0x). PreyVR showed a native engine can be **CPU-bound**, where cutting pixels barely helps. RFG's Geo-Mod physics may also be CPU-bound, so profile before optimizing the GPU. Foveated rendering via VRS on D3D11 is vendor-specific (OpenXR-Toolkit implements it as a layer). Consider it late.

### 5.2 Post-processing and screen-space effects
- Common casualties are TAA, motion blur, DoF, lens flare, SSAO/SSR, vignette, chromatic aberration, letterboxing and screen-space god rays. They either use previous-frame history (breaks under AFR) or unproject with a mono inverse matrix (breaks if only the projection is changed).
- Fix options:
  1. Disable them. Byte-patch or use cvars; anvil patched out TAA.
  2. Keep per-eye history by ping-ponging the history RTs by parity (starfield2vr).
  3. Supply correct per-eye inverse matrices to the post shaders, via 3DMigoto-style shader patching.
- Mods consistently force motion blur and DoF off (MGS5VR, anvil, starfield, Dishonored).

### 5.3 Shadows and culling
- Shadow cascades are fitted to the view frustum. If fitted per eye they differ, which shows as blinking or mismatched shadows. **Fit once per stereo pair from a combined (union) frustum and share** (cyberpunk-vr-port). Anvil and cyberpunk use a *far-plane discriminator* so shadow and reflection cameras are left alone.
- Engine culling is built for the game camera's FOV. The HMD FOV is wider (Index ~108-130 deg), and in AFR/third-person mode the head can look where the game camera isn't. Widen the culling FOV or feed the culling camera the union frustum, or you get a "void behind the player" / missing water and effects when looking away from the character. Anvil lists this as a known limitation: "Water and some visual effects are not visible when looking in the opposite direction to the character".

### 5.4 HUD/UI to world space
- The recipe (UEVR, REFramework, TefMeister):
  1. Detect the UI pass. It is usually the last draws before Present, orthographic, with no depth test.
  2. Redirect it to an offscreen RT (full-res, cleared to transparent).
  3. Present that RT as an **OpenXR quad layer** or a world-locked/head-locked quad at a comfortable distance, with scale and distance sliders.
  4. Keep the world 3D render free of UI.
- Diegetic options: a wrist HUD (Dishonored-VR, MGS5VR arm HUD), a crosshair replaced by a laser or a 3D reticle at hit depth (Luke Ross: dynamic crosshair depth).
- **Three stereo regimes** (TefMeister, three independent projects):
  - world: per-eye offset
  - near viewmodel: smaller separation or its own projection, otherwise you see two guns
  - 2D: fixed depth, identical in both eyes, otherwise it slides and won't fuse
- Menus: freeze head tracking into the camera while menus/cutscenes are open (anvil disables view injection while `bIsShowingUI`). Allow controller-as-mouse pointing (UEVR).
- HUDs authored in 16:9 canvas space need correction on the square-ish per-eye target (PreyVR's viewport-fraction fix).

### 5.5 Head vs aim decoupling, third-person to first-person
- **Camera:**
  1. Place the camera at the character's head bone (or eye socket).
  2. Hide or scale the head/neck/hair meshes. RE2's first-person mod shows head-outline artifacts on some characters.
  3. Apply the HMD pose relative to a body-yaw anchor, not the game's orbit camera.
  4. Disable camera-collision push-in and camera shake, or keep them very attenuated.
- **Aim:** override the game's aim ray (the camera-forward-derived target) with the controller ray from the weapon muzzle. Keep the game's firing logic, but redirect the direction and origin at the function that constructs the shot. See cyberpunk-vr-port ("bullets follow the real weapon muzzle, not the camera") and PreyVR (shots "converge from the weapon's authored muzzle helper toward the reticle ray", which is a known residual error).
- **Body yaw vs head yaw:**
  - The character facing follows the stick or snap turn plus the HMD yaw beyond a threshold.
  - Movement direction is head- or controller-relative (an option).
  - Keep the "Decoupled Pitch" idea: game pitch never tilts the horizon.
- **Weapon attachment:** each frame after animation, override the weapon's world transform with the controller pose plus a per-weapon offset (UEVR UObjectHook "attach + adjust + permanent"). Two-handed weapons use the second controller as the aim reference. TefMeister notes that "the second controller hides behind the first" (hand crosstalk). PreyVR has an unresolved "right controller drags the left hand" bug.
- **Arms and body:**
  - Option 1: floating hands only. This is the easiest (RE4 in REFramework: "only hands are visible, no arms or body").
  - Option 2: two-bone arm IK driving the game skeleton's arm bones after animation. See the engine's own IK in PreyVR and Dishonored-VR (hands on the game's rig), and VRIK for Skyrim VR (https://www.nexusmods.com/skyrimspecialedition/mods/23416 is the VRIK Player Avatar by prog; Nexus blocked the scripted fetch, but it appeared in search results).
- **Keep a third-person "cinema" fallback:** for vehicles, cutscenes, the jetpack/ragdoll, and hard animations, switch to a stereo third-person camera or a virtual screen. This is a common pattern (starfield2vr toggles 1P/3P, and Luke Ross cannot override every animation camera). RFG has many vehicles and walkers, so plan for it.

### 5.6 Input
- **Native path:** read OpenXR actions and write into the game's input structs (hook XInput/DInput or the game's action layer). REFramework hooks XInput and DInput. MGS5VR has a remappable INI with conflict checking and live reload.
- **Quick path:** a virtual Xbox pad via **ViGEmBus** (https://github.com/nefarius/ViGEmBus; used by starfield2vr) to map controllers to gamepad input while the native mapping matures.
- Recenter binding, a seated/standing height calibration (Dishonored-VR's F5 sets standing height), and haptics on fire, impact and destruction.

### 5.7 Comfort
- Offer snap turn plus smooth turn with configurable speed, head- or controller-relative locomotion, optional vignette during artificial motion, a horizon-locked camera (no game-driven pitch/roll/shake), and a recenter option. Meta's guidance: https://developers.meta.com/horizon/resources/locomotion-comfort-usability/
- **RFG-specific hazards:**
  - explosion camera shake
  - ragdoll / knockdown camera
  - vehicle camera motion
  - hammer-swing root motion
  - jetpack (Guerrilla has none, but the DLC might)

  Suppress game-driven camera motion and provide "comfort cams" (fade to black or a stationary third-person view during knockdowns).

---

## 6. Lessons learned and pitfalls (from postmortems, READMEs and devlogs)

1. **Eye-label off-by-one under AFR:** the image is correct but goes to the wrong eye, which reads as "odd scale/depth", not as a bug. Latch the eye with the frame when you inject it, and read that latched value at submission (TefMeister, far-cry-2-vr).
2. **Frame-parity slip:** the engine renders two "left" frames in a row after a hitch, and the eyes desync. Keep engine/render/present counters and self-correct (vrframework ch. 07; REFramework's `m_left_eye_interval`).
3. **Per-frame state double-advancing:** particles, clocks, wind, shadow fits, audio and random seeds must advance once per pair (cyberpunk-vr-port; KisakCOD-VR via TefMeister; the-darkness-vr). In AFR, the sim moving between eyes made a moving car ~130 ms apart between eyes, so they had to "hold the world still on the second eye's frame".
4. **"Same object, same code path":** if two things must agree (lens and mask, picture and crosshair, left and right of a pair), publish them as one frame-stamped snapshot. theHunter VR spent two days and ~20 test cycles on a scope flicker caused by two routes, one of which could silently fail.
5. **Verify your instruments:** a slider displayed as `0.00` was actually 0.004, which voided four eliminations (theHunter postmortem). Assert numerically that the two eye matrices differ each frame. Transform setters that early-out on "unchanged matrix" can silently collapse stereo to mono.
6. **Projection-only eye offset is invisible to the CPU:** culling, lighting rects and LOD disagree with the picture per eye. Move the offset into the camera.
7. **Wrong basis/handedness means the world "swims" when you turn your head.** Build the `engine<->runtime` basis matrices first, log the matrices, and watch for row/column-major transposes.
8. **Main-camera discrimination:** shadow, reflection, cubemap and UI cameras also build matrices. Filter by far plane, RT size or camera name.
9. **Viewmodel needs its own separation** (two guns). HUD needs a fixed depth (it slides).
10. **Temporal AA under AFR smears:** disable it or double-buffer the history per eye.
11. **Proxy-DLL export completeness:** export everything the exe imports, load the real DLL from System32 by full path, and co-exist with other mods' proxies.
12. **Vtable patches can be overwritten** by overlays (Steam, Discord, RTSS), and D3D state-block/Reset paths can disarm hooks. Prefer inline hooks on the implementation functions (SafetyHook) over vtable slot writes, and restore hooks before unloading.
13. **A read-only diagnostic hook on a hot path is not harmless** (re-village-scope-vr). Remove probes after use.
14. **Silent no-ops:** a test whose input (e.g. head yaw) is silently zero "passes". Always run a positive control.
15. **Runtime quirks:** SteamVR OpenXR per-view pose handling has differed from the spec historically (Luke Ross 2020: wrong baseline plus a vertical offset on the Index). The Oculus runtime was reported to ignore projection-view poses in 2023 (https://community.khronos.org/t/oculus-runtime-ignores-projection-layer-views-pose/110078). Test with real located views and look for vertical disparity.
16. **Don't overpromise:** Dishonored-VR's author burned out on "problems I can't reproduce". Build logging, crash dumps, exe-version gating and an in-headset settings overlay (F10 style) from day one. PreyVR refuses unknown builds.
17. **Forced in-game settings:** document or force Windowed mode, VSync off, frame cap off, motion blur off, DoF off, and dynamic resolution off (anvil, starfield, MGS5VR, Dishonored).
18. **Legal and distribution:** ship no game files. Keep the mod free. Publishers have acted against monetized mods (Take-Two vs Luke Ross's GTA V). Respect code licenses: REFramework MIT is fine to reuse with attribution, UEVR is all rights reserved, and RSL is GPL-2.0 (copyleft if you link or copy).
19. **Headset time is the scarcest resource:** use VRto3D, xr-sim, or a desktop SbS mirror plus synthetic head-pose scripts to measure (e.g. % black pixels during a scripted yaw sweep, which measures the culling void) without wearing the HMD.
20. **Treat a wearer's report as primary evidence.** Symptom descriptions like "two guns" or "HUD slides" map directly to a regime/transform bug (TefMeister HUD table).

---

## 7. Source index (all verified in this session)

- Flat2VR Discord: https://discord.com/invite/flat2vr
- Flat2VR Notion: https://beastsaber.notion.site/Flat2VR-Modding-Group-8eb9ae0535144eac843f428abb104de9
- Flat2VR Studios: https://www.flat2vrstudios.com/
- Beyond Flatscreen: https://beyondflatscreen.vercel.app/
- REFramework: https://github.com/praydog/REFramework
- UEVR: https://github.com/praydog/UEVR (docs: https://docs.uevr.io/)
- vrframework: https://github.com/elliotttate/vrframework
- anvilengine2vr: https://github.com/mutars/anvilengine2vr
- starfield2vr: https://github.com/mutars/starfield2vr
- R.E.A.L. GTA V: https://github.com/LukeRoss00/gta5-real-mod
- R.E.A.L. Patreon: https://www.patreon.com/realvr
- OpenVR issue #1253: https://github.com/ValveSoftware/openvr/issues/1253
- MotherVR: https://github.com/Nibre/MotherVR
- MotherVR interview: https://roadtovr.com/mothervr-mod-alien-isolation-oculus-rift-nibre-zack-fannon/
- PreyVR: https://github.com/phunkaeg/PreyVR
- VR modding playbook: https://github.com/phunkaeg/vr-modding-playbook
- xr-sim: https://github.com/phunkaeg/xr-sim
- xr-tape: https://github.com/phunkaeg/xr-tape
- Cross-engine research: https://github.com/TefMeister/flat-to-vr-cross-engine-research
- MGS5VR: https://github.com/nikamigaming-create/MGS5VR
- cyberpunk-vr-port: https://github.com/dariulone/cyberpunk-vr-port
- Cyberpunk2077-VR-OpenSource: https://github.com/pinducoding/Cyberpunk2077-VR-OpenSource
- theHunterCotW-VR: https://github.com/vaas993/theHunterCotW-VR
- Dishonored-VR: https://github.com/GingasVRFO/Dishonored-VR , https://github.com/VR-Stereo-Hub/Dishonored-VR
- NomaiVR: https://github.com/Raicuparta/nomai-vr
- Two Forks VR: https://github.com/Raicuparta/two-forks-vr
- UUVR: https://github.com/Raicuparta/uuvr
- Rai Pal: https://github.com/Raicuparta/rai-pal
- 3DMigoto: https://github.com/bo3b/3Dmigoto
- geo-11: https://github.com/ThreeDeeJay/geo-11
- vorpX: https://www.vorpx.com/more-headtracking-z-buffer-vs-geometry-3d/
- VRto3D: https://github.com/oneup03/VRto3D
- OpenXR SDK: https://github.com/KhronosGroup/OpenXR-SDK-Source
- OpenXR-Toolkit: https://github.com/mbucchia/OpenXR-Toolkit
- Quad-Views-Foveated: https://github.com/mbucchia/Quad-Views-Foveated
- OpenComposite: https://gitlab.com/znixian/OpenOVR
- Revive: https://github.com/LibreVR/Revive
- SafetyHook: https://github.com/cursey/safetyhook
- MinHook: https://github.com/TsudaKageyu/minhook
- Detours: https://github.com/microsoft/Detours
- kiero: https://github.com/Rebzzel/kiero
- Zydis: https://github.com/zyantific/zydis
- Ultimate ASI Loader: https://github.com/ThirteenAG/Ultimate-ASI-Loader
- ImGui: https://github.com/ocornut/imgui
- ViGEmBus: https://github.com/nefarius/ViGEmBus
- RenderDoc: https://renderdoc.org/
- Cheat Engine: https://github.com/cheat-engine/cheat-engine
- ReClass.NET: https://github.com/ReClassNET/ReClass.NET
- x64dbg: https://github.com/x64dbg/x64dbg
- Ghidra: https://github.com/NationalSecurityAgency/ghidra
- RFG modding org: https://github.com/rfg-modding
- RSL docs: https://rsl.readthedocs.io/
- Half-Life 2: VR Mod: https://store.steampowered.com/app/658920/HalfLife_2_VR_Mod/
- HL2VRU: https://github.com/vittorioromeo/HL2VRU
- Team Beef: https://www.teambeefvr.com/
- RTCWQuest: https://github.com/Team-Beef-Studios/RTCWQuest
- Meta comfort guide: https://developers.meta.com/horizon/resources/locomotion-comfort-usability/
- Khronos per-view pose thread: https://community.khronos.org/t/oculus-runtime-ignores-projection-layer-views-pose/110078
- SteamVR per-view pose report: https://steamcommunity.com/app/250820/discussions/8/3001046778344834329/
- theHunter flicker postmortem: https://github.com/vaas993/theHunterCotW-VR/blob/main/docs/THE_FLICKER_POSTMORTEM.md
