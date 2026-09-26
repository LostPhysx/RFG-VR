# 04 - Developer Toolchain and Reference Code (C++/Windows)

Project: 6-DOF VR mod for **Red Faction Guerrilla Re-Mars-tered** (Steam AppID 667720, x64, D3D11), delivered as an injected C++ DLL using **OpenXR** on a **Valve Index** (SteamVR as the OpenXR runtime).

Scope: tools and libraries only. RFG-specific modding, the OpenXR spec and the wider flat2vr community are covered in other research files. All URLs below were opened or returned by search while writing this file (September 2026). Star counts and versions are what the pages showed then.

---

## TL;DR / Summary

- **Hooking library: SafetyHook** (cursey, BSL-1.0, vcpkg `safetyhook` 0.7.0, C++23, needs Zydis). praydog's REFramework uses it (fetched from git, per `cmake.toml`). It does inline hooks, mid-function hooks (useful for grabbing the camera matrix from registers) and VMT hooks. **MinHook** (BSD-2, v1.3.4, vcpkg) is the fallback. You won't need Detours or PolyHook2.
- **kiero is archived** (May 2026). Don't build on it. Use a dummy D3D11 device and swapchain to get the vtable, the same way `D3D11Hook.cpp` does in REFramework and UEVR.
- **Best code to study: praydog's REFramework (MIT).** It has the same shape as our project: an injected DLL, a D3D11 hook, and OpenXR plus OpenVR runtimes behind one interface. The key files are `src/D3D11Hook.cpp`, `src/mods/VR.cpp`, `src/mods/vr/D3D11Component.cpp`, `src/mods/vr/runtimes/OpenXR.cpp` and `src/mods/vr/OverlayComponent.cpp`. **UEVR's LICENSE file is now "Copyright (c) 2022-2025 praydog. All rights reserved."** So read UEVR for ideas, but don't copy code from it. Copy only from REFramework (MIT), and keep the attribution.
- **Newer open-source D3D11 + OpenXR injected mods are good templates.** KCD1VR (MIT, CMake + MinHook + OpenXR loader, DXGI Present intercept, stereo swapchain, head-locked HUD quad) and BioShock Trilogy VR (MIT, 645 commits, the most mature) are both close to what we need. `elliotttate/vrframework` (MIT) is a written 17-part guide plus a scaffold taken from REFramework.
- **RE tools (all free):** Ghidra 12.1.x (JDK 21) with the built-in `RecoverClassesFromRTTIScript` for MSVC classes and Sigga or MakeSig for signatures. Also x64dbg, Cheat Engine, ReClass.NET and RenderDoc. Keep IDA Free (cloud x64 decompiler, non-commercial) or Binary Ninja Free (x86_64 decompiler, non-commercial) as second-opinion decompilers. **PIX and Nsight Graphics no longer support D3D11 well, so use RenderDoc.**
- **Steam DRM:** Steamless handles SteamStub v3.x on x64. Whether `rfg.exe` actually has SteamStub is **not confirmed here** (PCGamingWiki and SteamDB both blocked fetches). Check this on day 1.
- **ReShade add-on API is a possible host, but I don't recommend it as the main host.** It gives robust D3D11 hooking, depth-buffer detection (example `09-depth`), `get_native()` to reach `ID3D11Device`, and its own SteamVR support. But add-ons need the unsigned "full add-on support" build, and we still need our own camera and engine hooks. Treat ReShade and Special K (GPL-3.0) as **conflict risks**: both hook DXGI. Uninstall them while developing.
- **Build:** CMake with a vcpkg manifest and the `x64-windows-static` triplet (matches REFramework's `/MT`). Use MSVC 2022, C++23, `/MP /EHa /bigobj`, and PDBs in Release. For quick iteration, use a two-DLL "host + hot-reloadable logic" split (see `ergrelet/dll-hot-reload` and DetourModKit's hot-reload docs).

---

## 1. Reverse-engineering tools

| Tool | URL | License / cost | Maturity | Relevance | Concrete use in this project |
|---|---|---|---|---|---|
| **Ghidra** 12.1.4 (Sep 2026) | https://github.com/NationalSecurityAgency/ghidra/releases | Apache-2.0, free | Very mature; needs JDK 21, and PyGhidra is now the default scripting | **High** | Main static analysis of `rfg.exe`: find the camera, render and projection setup, player transform, and input code. Scripting is available for bulk tasks. |
| Ghidra `RecoverClassesFromRTTIScript` (built in) | https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/Decompiler/ghidra_scripts/classrecovery/RTTIWindowsClassRecoverer.java | Apache-2.0 | Labelled "prototype". Known issues: StackOverflow on large binaries (#6832), exceptions (#8853) | **High** (if RFG ships RTTI) | Rebuilds MSVC class hierarchies and vftables, which makes camera and renderer classes easy to find by name. |
| Ghidra-Cpp-Class-Analyzer (astrelsky) | https://github.com/astrelsky/Ghidra-Cpp-Class-Analyzer | MIT | **Archived Oct 2023**; the features are now in Ghidra | Low | Skip it and use the built-in script. |
| Sigga (Ghidra sigmaker) | https://github.com/lexika979/Sigga | see repo | Active | **High** | Makes unique AOB signatures for hook targets, so our DLL finds functions at runtime instead of relying on hard-coded RVAs. |
| Ghidra-MakeSig / nosoop makesig.py | https://github.com/jimppan/Ghidra-MakeSig , https://github.com/nosoop/ghidra_scripts/blob/master/makesig.py | see repos | Stable | Med | Alternatives to Sigga. |
| x64dbg-sigmaker | https://github.com/u16rogue/x64dbg-sigmaker | see repo | Small | Low-Med | Makes signatures from inside the debugger. |
| **x64dbg** | https://github.com/x64dbg/x64dbg | GPL-3.0 (plugins exempt), free | Very mature (49.6k stars) | **High** | Live debugging, breakpoints on camera and matrix writes, crash triage of our DLL, checking hook trampolines. |
| ScyllaHide | https://github.com/x64dbg/ScyllaHide | GPL-3.0 | Mature | Low-Med | Only needed if the game or SteamStub does anti-debug checks. Keep it installed but off by default. |
| ret-sync | https://github.com/bootleg/ret-sync | GPL-3.0 (Binja plugin MIT) | Mature | Med | Keeps x64dbg and Ghidra on the same address, with ASLR rebasing handled. Speeds up "what writes this matrix" work a lot. |
| **IDA Free** | https://hex-rays.com/ida-free | Free, **non-commercial only**; cloud x86/x64 decompiler; no IDAPython/SDK | Mature | Med | Second-opinion decompiler for tricky functions (MSVC x64 exception handling is supported). |
| **Binary Ninja Free** | https://binary.ninja/free/ | Free for non-commercial use; decompiles x86_64; v6.0 "Krypton" (Sep 2026) includes an MCP server in Free | Mature | Med | Another decompiler; its MCP server lets an LLM help with analysis. |
| **Cheat Engine** | https://github.com/cheat-engine/cheat-engine | Custom CE license, free; users report installer adware, so use the GitHub release or build from source (Lazarus) | Very mature | **High** | Scan for camera position, FOV and view matrix (floats in [-1,1] that change when the camera moves); "find what writes/accesses" to get the owning functions; pointer scans; Structure Dissect. |
| View-matrix scanning technique | https://zero-irp.github.io/ViewProj-Blog/part-3-finding-and-reversing-matrices/ , https://guidedhacking.com/threads/how-to-find-the-view-matrix-with-cheat-engine.7903/ | articles | - | Med | Recipes for finding the View, Proj and ViewProj matrices, which usually sit at static or global addresses. |
| **ReClass.NET** | https://github.com/ReClassNET/ReClass.NET | MIT | Mature (2.2k stars) | **High** | Maps camera, player and renderer structs live, including matrix and vtable node types, then **exports C++ headers** we can drop straight into the mod. |
| **RenderDoc** | https://github.com/baldurk/renderdoc , docs: https://renderdoc.org/docs/window/capture_attach.html | MIT | Very mature | **High** | Captures a D3D11 frame: pass structure, which cbuffers hold view and projection, depth format (reverse-Z?), where HUD/UI draws happen, backbuffer format. **Injecting into a running process does not work** because the game creates its API device first. Instead, launch the exe with `steam_appid.txt`, or launch Steam through RenderDoc with "Capture Child Processes" on ([guide](https://peterhanshawart.blogspot.com/2019/07/renderdoc-and-steam-capturing-steam.html)). Its guessed projection is unreliable with reverse-Z ([issue #169](https://github.com/baldurk/renderdoc/issues/169)). |
| PIX on Windows | https://devblogs.microsoft.com/pix/debugging-d3d11-apps-using-d3d11on12/ | Free | Mature | Low | GPU capture is **D3D12 only**. D3D11 works only through "Force D3D11On12", and Microsoft says some features work poorly. Only useful for CPU timing captures. |
| NVIDIA Nsight Graphics | https://developer.nvidia.com/nsight-graphics , notes: https://developer.nvidia.com/nsight-graphics/getting-started/release-note-v2025.5 | Free | Mature | Low | Graphics Capture supports only D3D12 and Vulkan. The Frame Debugger is being renamed "OpenGL Frame Debugger", so D3D11 is effectively unsupported. Use RenderDoc. |
| API Monitor (rohitab) v2 alpha-r13 | http://www.rohitab.com/downloads | Freeware, closed | Old (2013-era) but still works | Low-Med | Logs D3D11/DXGI/XInput/file calls at startup: device creation flags, swapchain descriptors, which DLLs load (helps pick a proxy DLL). |
| **Special K** | https://github.com/SpecialKO/SpecialK | GPL-3.0 | Mature | Low (reference) / **conflict risk** | Reference for thorough DXGI/D3D11 hooking and local (`dxgi.dll`/`d3d11.dll`) vs global injection. GPL, so read it and don't copy. It's a conflict risk if users run SK together with our mod. |
| **ReShade** | https://github.com/crosire/reshade , API: https://github.com/crosire/reshade/blob/main/REFERENCE.md | BSD-3 (some files MIT) | Very mature (5.5k stars, v6.8.0) | Med | See section 1a. |

### 1a. ReShade as a host for the VR code: evaluation

For:
- Robust D3D9-12 and DXGI hooking, with events such as `present`, `create_swapchain`, `draw` and `bind_render_targets_and_depth_stencil`.
- `device->get_native()` returns the real `ID3D11Device*`, which we can hand to `XrGraphicsBindingD3D11KHR`.
- Example add-ons include `09-depth` (depth-buffer detection), `06-shader_replace`, `04-api_trace` and `16-swapchain_override` ([list](https://github.com/crosire/reshade/tree/main/examples)).
- ReShade has had its own SteamVR/OpenVR support since 5.0 ([release post](https://reshade.me/releases/7749-5-0)).

Against:
- Add-ons need the **unsigned "full add-on support" build** ([forum](https://reshade.me/forum/general-discussion/8487-reshade-vs-reshade-with-addon-support-difference)), which makes installation harder for users.
- The API is abstracted (fine, but it's one more layer).
- We still need SafetyHook engine hooks for the camera, projection and input, so ReShade saves little.
- It ties our release cadence to ReShade's.

Verdict: **Use ReShade as a dev tool and reference (the depth add-on, `04-api_trace`), not as the host.** Build a standalone DLL. Later we might offer a ReShade add-on build for users who already run ReShade.

---

## 2. Hooking, injection, proxy DLLs and DRM

| Tool / lib | URL | License | Maturity | Relevance | Use |
|---|---|---|---|---|---|
| **SafetyHook** | https://github.com/cursey/safetyhook | BSL-1.0 | Active, 755 stars; vcpkg `safetyhook` 0.7.0; needs **C++23** and Zydis; FetchContent with `-DSAFETYHOOK_FETCH_ZYDIS=ON`; amalgamated build available | **High** | Main hook library. Inline hooks for engine functions such as camera update and projection build; **mid-hooks** to read or patch registers in the middle of a function (for example, grab the view matrix pointer); VMT hooks for `IDXGISwapChain::Present`/`ResizeBuffers` and `ID3D11DeviceContext` methods. Thread-safe install (suspends threads, fixes IP and RIP-relative code). REFramework uses it. |
| **MinHook** | https://github.com/TsudaKageyu/minhook | BSD-2 | v1.3.4 (Mar 2025), 6k stars, vcpkg | Med (fallback) | Simple, proven inline hooks, used by RSL (RFG's script loader), KCD1VR and kiero. No mid-hooks. |
| Microsoft Detours | https://github.com/microsoft/Detours | MIT | v4.0.1, vcpkg `detours` | Low | Fine, but its transaction API is clunkier than SafetyHook's. |
| PolyHook 2 | https://github.com/stevemk14ebr/PolyHook_2_0 | MIT | 1.9k stars, vcpkg `polyhook2`; pulls in Zydis, asmjit and asmtk | Low | Has more hook types (breakpoint, IAT/EAT, VFunc) than we need, and heavy dependencies. |
| kiero | https://github.com/Rebzzel/kiero | MIT | **Archived May 2026** ("switch to kiero2") | Low | Only worth reading for the dummy-device vtable trick and `METHODSTABLE.txt` indices. Don't depend on it. |
| kananlib (cursey) | https://github.com/cursey/kananlib | BSL-1.0 | Active, used by REFramework | **Med-High** | Runtime scanning: IDA-style patterns, string and xref search, RIP-relative displacement refs, function bounds from `.pdata`, **MSVC RTTI vtable scanning**, emulation. Lets hooks survive game patches. |
| **Ultimate ASI Loader** | https://github.com/ThirteenAG/Ultimate-ASI-Loader | MIT | Mature, 2.3k stars, x86 and x64 | **High** | Drop-in proxy (`dinput8.dll`, `version.dll`, `winmm.dll`, `dxgi.dll`, `d3d11.dll`, xinput, ...). Loads `*.asi` from the root, `scripts/` or `plugins/`, and writes crash dumps. Easiest way for users to install; our DLL is just `RFGVR.asi`. |
| UltimateProxyDLL (techiew) | https://github.com/techiew/UltimateProxyDLL | Unlicense | Small, header-only, MSVC | Med | Build our own proxy with `upd::create_proxy(...)`. Supports dxgi, d3d11, dinput8, XInput and others. |
| perfect-dll-proxy (mrexodia) | https://github.com/mrexodia/perfect-dll-proxy | BSL-1.0 | 373 stars | Med | Python generator that forwards exports to absolute `GLOBALROOT\System32` paths with no asm stubs. A clean way to make a `version.dll`/`winmm.dll` proxy. |
| Other proxy generators | https://github.com/iMoD1998/DLL-Proxy-Generator , https://github.com/Just1uke/dll-proxy-generator | see repos | - | Low | Alternatives. |
| Xenos injector | https://github.com/DarthTon/Xenos | MIT | 2.7k stars; Win7-10 era; built on Blackbone | Low-Med | Manual-map or LoadLibrary injection while developing, if we don't want a proxy. Kernel features aren't needed. |
| wkhughes/Injector | https://github.com/wkhughes/Injector | see repo | Small | Low | Injector for mod development that can also eject the DLL, which helps with reload loops. |
| **Steamless** | https://github.com/atom0s/Steamless | **CC BY-NC-ND 4.0** (tool only, doesn't affect our code) | Mature, 5k stars | **High (day 1)** | Unpacks SteamStub v3.0.0-v3.1.2 on x64. Use it on a **copy** of `rfg.exe` so Ghidra sees real code. Steamworks API calls stay, so the game still needs Steam running. **Never ship a modified exe**: the mod must hook the retail exe at runtime. |

**How SteamStub affects this project:** SteamStub encrypts or compresses `.text` and decrypts it at runtime. Static RVAs stay valid after unpacking, but on disk the packed exe looks like garbage to Ghidra. At runtime, once the stub has run, the in-memory image is normal, so a mod DLL loaded through a proxy after startup can scan and hook as usual. Watch for one catch: a proxy like `version.dll` may load *before* the stub finishes, so start signature scans lazily (for example on the first D3D11 device creation or the first Present), not in `DllMain`. Also add `steam_appid.txt` (containing `667720`) next to the exe so x64dbg and RenderDoc can launch it directly.

---

## 3. UI, debug and config libraries

| Lib | URL | License | Version | Relevance | Use |
|---|---|---|---|---|---|
| **Dear ImGui** | https://github.com/ocornut/imgui | MIT | 1.92.x (vcpkg 1.92.9; features `dx11-binding`, `win32-binding`, `docking-experimental`) | **High** | Debug and config overlay. Backends: `backends/imgui_impl_dx11.*` and `imgui_impl_win32.*`. **In VR**, render ImGui into an offscreen `ID3D11Texture2D` and submit it as an `XrCompositionLayerQuad` (REFramework's `OverlayComponent.cpp` does this), with controller-ray or mouse input. |
| **spdlog** | https://github.com/gabime/spdlog | MIT | vcpkg `spdlog` | **High** | File plus in-game ring-buffer logging. REFramework vendors it and builds it with `/Zc:templateScope-` on MSVC 19.35+. |
| **toml++** | https://github.com/marzer/tomlplusplus | MIT | vcpkg `tomlplusplus` 3.4.0 | Med-High | User config (`rfgvr.toml`): IPD scale, world scale, snap turn, HUD distance. Header-only. |
| SimpleIni (via DetourModKit) | (dependency listed in DetourModKit) | MIT | - | Low-Med | INI alternative if users prefer INI. |
| DetourModKit | https://github.com/tkhquang/DetourModKit | MIT | Early (3 stars) but tested, C++23 | Med (reference) | Toolkit on top of SafetyHook with AOB "candidate ladders", guarded memory reads, INI hot-reload, XInput hotkeys, async logger, and **documented DLL hot-reload teardown** (`docs/hot-reload/README.md`). Good design reference. |

---

## 4. Math and XR libraries

| Lib | URL | License | Relevance | Notes |
|---|---|---|---|---|
| **OpenXR SDK / loader** | https://github.com/KhronosGroup/OpenXR-SDK (read-only mirror; development in https://github.com/KhronosGroup/OpenXR-SDK-Source) | Apache-2.0 | **High** | vcpkg port **`openxr-loader` 1.1.63**. REFramework instead pulls OpenXR-SDK with FetchContent at a pinned commit. Link statically or ship `openxr_loader.dll` next to our DLL, **not** in the game folder root where it might clash. Enable `XR_KHR_D3D11_enable`. |
| **DirectXMath** | https://github.com/microsoft/DirectXMath | MIT | **High** | Header-only SIMD, vcpkg `directxmath` (release Jun 2026). Row-major, like D3D, so it probably matches the game's matrix layout. Use it for XrPosef to engine-matrix conversion and per-eye asymmetric projections. |
| glm | https://github.com/g-truc/glm | MIT / Happy Bunny | Med | 1.0.3 (Dec 2025). REFramework and UEVR use it. Needs `GLM_FORCE_DEPTH_ZERO_TO_ONE` and a left-handed setup to match D3D. Choose **one** of DirectXMath or glm; I lean towards DirectXMath. |
| OpenVR SDK | https://github.com/ValveSoftware/openvr | BSD-3 | Low | Not needed, since SteamVR exposes OpenXR. Only useful for reading REFramework's OpenVR path. |

---

## 5. Build system and project setup

| Item | URL | Relevance | Notes |
|---|---|---|---|
| CMake + **vcpkg manifest mode** | vcpkg ports verified: `safetyhook`, `minhook`, `imgui`, `openxr-loader`, `tomlplusplus`, `detours`, `polyhook2`, `directxmath`, `spdlog` | **High** | One `vcpkg.json`, triplet **`x64-windows-static`** so the CRT is linked statically (`/MT`, as in REFramework Release). No runtime DLLs are dropped into the game folder. |
| CPM.cmake | https://github.com/cpm-cmake/CPM.cmake | Med | MIT, 4.1k stars. `CPMAddPackage("gh:cursey/safetyhook#<tag>")` if you'd rather pin git commits the way REFramework does. `CPM_SOURCE_CACHE` gives offline builds. |
| cmkr (TOML to CMake) | used by REFramework (`cmake.toml`) | Low | Only worth knowing so you can read REFramework's build. |
| **MSVC settings** (from REFramework `cmake.toml`) | https://raw.githubusercontent.com/praydog/REFramework/master/cmake.toml | **High** | `cxx_std_23` (SafetyHook needs C++23), `/MP`, Release `/MT`, `/EHa` (SEH-safe hooks), `/bigobj`, `/GS-`. Add: `/Zi` plus `/DEBUG:FULL` in Release so we have PDBs for crash dumps; `/utf-8`; `/W4`. Output a `.asi` or proxy DLL named per build config. |
| **DLL hot-reload** | https://github.com/ergrelet/dll-hot-reload (MIT, 50 stars; reloads a DLL when it changes on disk, optional Blackbone manual map), DetourModKit hot-reload docs: https://github.com/tkhquang/DetourModKit/blob/main/docs/hot-reload/README.md | **Med-High** | Recommended pattern: a **stable host DLL** (proxy, D3D11 hooks, OpenXR instance/session/swapchains, ImGui context) plus a **reloadable logic DLL** (camera math, stereo logic, UI panels), built into a staging folder and copied to a shadow path before `LoadLibrary`, so the linker never hits a locked file. Tear down in order: disable logic-owned SafetyHook hooks, wait for in-flight calls, `FreeLibrary`. Never tear down the XR session on reload. |

### Minimal `vcpkg.json` sketch
```json
{
  "name": "rfg-vr",
  "version": "0.1.0",
  "dependencies": [
    "safetyhook", "minhook", "openxr-loader", "spdlog", "tomlplusplus", "directxmath",
    { "name": "imgui", "features": ["dx11-binding", "win32-binding"] }
  ]
}
```
(Configure with `-DVCPKG_TARGET_TRIPLET=x64-windows-static` and `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`.)

---

## 6. Reference codebases

| Project | URL | License | Maturity | Relevance | What to study (key files) |
|---|---|---|---|---|---|
| **REFramework** (praydog) | https://github.com/praydog/REFramework | **MIT** | Very mature (5.5k stars, 2.2k+ commits) | **High** | `src/D3D11Hook.cpp/.hpp` (dummy-device vtable hooking of Present/ResizeBuffers), `src/HookManager.cpp`, `src/WindowsMessageHook.cpp`, `src/DInputHook.cpp`, `src/mods/VR.cpp` (the VR module: pose timing, stereo/AFR, projection override), `src/mods/vr/D3D11Component.cpp` (copying game RTs into XR swapchains on D3D11), `src/mods/vr/OverlayComponent.cpp` (ImGui/UI as a quad layer), `src/mods/vr/runtimes/OpenXR.cpp`, `OpenVR.cpp`, `VRRuntime.hpp` (runtime abstraction), `src/mods/vr/Bindings.cpp` (action bindings), `cmake.toml` (dependencies and flags). **Code we may reuse, with MIT attribution.** |
| **UEVR** (praydog) | https://github.com/praydog/UEVR | **LICENSE: "Copyright (c) 2022-2025 praydog. All rights reserved."** | Very mature (4.5k stars) | **High (read only)** | `src/hooks/D3D11Hook.cpp`, `src/hooks/XInputHook.cpp`, `src/mods/vr/D3D11Component.cpp`, `src/mods/vr/OverlayComponent.cpp`, `src/mods/vr/runtimes/OpenXR.cpp`, `src/mods/VR.cpp`, `FFakeStereoRenderingHook.cpp` (UE-specific stereo injection, useful as a concept for "lie to the engine about stereo"). Study only; implement from REFramework or from scratch. |
| **KCD1VR** | https://github.com/farmerarmor/KCD1VR | MIT | v0.1.67, 5 stars, experimental | **High** (architecture match) | CryEngine D3D11 mod: intercepts DXGI Present, copies both eye rects into a 2-slice OpenXR swapchain, submits one projection layer; asymmetric per-eye projection, real IPD, head-locked HUD quad; **CMake 3.24 + MinHook + OpenXR loader pinned**. Small codebase and very close to our target. |
| **BioShock Trilogy VR** | https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr | MIT | 125 stars, 645 commits, "playable, tuned" | **High** | Mature injected D3D11 + OpenXR mod: camera-path hooks, 6DOF, motion controllers with per-weapon aim profiles, in-headset calibration, `xinput1_3.dll` injection vector, CMake + VS2022. Best example of **gameplay** integration (aiming, locomotion). |
| vrframework (elliotttate) | https://github.com/elliotttate/vrframework | MIT (upstream REFramework copyright kept) | 16 stars; guide complete, code partly stubbed | Med-High | 17-part guide (`guides/00-README.md`) on injection, DX hooking, frame timing, stereo strategies and HUD; engine-adapter SPI; `PORTING.md` maps stubs to REFramework sources. Good onboarding reading. |
| OpenXR-Simulator (elliotttate) | https://github.com/elliotttate/OpenXR-Simulator | MIT | 37 stars, active | Med | OpenXR runtime that runs in a desktop window (D3D11 supported, side-by-side preview, Index profile emulation). Register it with `register-runtime.ps1`. Lets you iterate without putting on the Index. Switch the active runtime back to SteamVR afterwards. |
| **vrperfkit** (fholger) | https://github.com/fholger/vrperfkit | MIT | 1.5k stars, OpenVR/Oculus only, D3D11 only | Med | Clean, small **`dxgi.dll` proxy + D3D11 hook** code: `src/proxy/`, `src/d3d11/`, `src/hooks.cpp`, `src/openvr/` (hooks `IVRCompositor::Submit`), `src/config.cpp` (yaml). Good model for proxy and hook structure. |
| **OpenComposite** (znixian) | https://gitlab.com/znixian/OpenOVR | GPL-3.0+ | Mature (1.2k commits) | Low-Med | OpenVR-to-OpenXR translation. Its `OpenOVR/Drivers/Backends/OpenXRBackend` and D3D11 compositor code show real OpenXR D3D11 swapchain handling. GPL, so read only. |
| **OpenXR-Toolkit** (mbucchia) | https://github.com/mbucchia/OpenXR-Toolkit | MIT | **Discontinued 2024** (author says don't install it) | Med (code reading) | `XR_APILAYER_MBUCCHIA_toolkit/` shows D3D11 interop, swapchain interception and upscaling inside an OpenXR layer. |
| OpenXR-Layer-Template (mbucchia) | https://github.com/mbucchia/OpenXR-Layer-Template | MIT | Stable | Low-Med | Generated dispatch for API layers. Useful only if we later want a layer for debugging (for example, logging our own xr* calls). |
| maluoi/OpenXRSamples | https://github.com/maluoi/OpenXRSamples (`SingleFileExample/main.cpp`) | MIT | 100 stars | **High** (learning) | **Single-file C-style OpenXR + D3D11** app. The smallest reference for instance, session, swapchain and frame loop with D3D11. |
| openxr-tutorial.com (Khronos) | https://openxr-tutorial.com/windows/d3d11/3-graphics.html | CC/Apache (Khronos) | Official | **High** (learning) | Step-by-step D3D11 swapchain creation and use. |
| Microsoft OpenXR-MixedReality BasicXrApp | https://github.com/microsoft/OpenXR-MixedReality/blob/main/samples/BasicXrApp/OpenXrProgram.cpp | MIT | Mature | Med | Well-structured C++17 D3D11 OpenXR program. |
| hyperlogic/openxrstub | https://github.com/hyperlogic/openxrstub | see repo | Small | Low | Another minimal OpenXR app. |
| MotherVR (Nibre) | https://github.com/Nibre/MotherVR | Not stated; **source not published** (issue #193 asks for it) | Released mod | Low | Architecture idea only (`dxgi.dll` proxy, in-game options menu). No code. |
| Luke Ross R.E.A.L. VR | https://www.patreon.com/realvr , https://github.com/LukeRoss00/gta5-real-mod | **Closed source** (framework free since Mar 2026) | Mature | Low | Proof that AER/AFR-style injected VR works across engines. No code to study. |
| DrBeef HL2VR_d3d9 | https://github.com/DrBeef/HL2VR_d3d9 | see repo | Released | Low | Example of a D3D9 proxy VR DLL (old API). |
| RFG-specific (cross-ref) | https://github.com/rfg-modding (RSL: GPL-2.0, MinHook + kiero + ImGui + LuaJIT, proxy loader; `RFGR_Types` = data types for RSL2), https://github.com/Moneyl/RFGR-Extended-Camera (MIT, archived; free camera) | mixed | - | **High** (see research 01-03) | Existing RFGR hooking and camera code and type definitions. RSL is GPL, so read only. Check `RFGR_Types` for camera/player structs before starting ReClass work. |

---

## 7. Recommended starting stack (with justification)

| Layer | Choice | Why |
|---|---|---|
| Loader / injection | **Ultimate ASI Loader** for users; our own `version.dll`/`winmm.dll` proxy (perfect-dll-proxy or UltimateProxyDLL) or Xenos while developing | UAL is the standard way users install mods and handles many proxy names. Avoid `dxgi.dll`/`d3d11.dll` as our proxy name so we don't collide with ReShade, Special K or vrperfkit. Pick the final name after checking `rfg.exe` imports. |
| Hooking | **SafetyHook** (inline + mid + VMT) + **kananlib** for scans; MinHook as fallback | Modern, thread-safe, and proven by REFramework on the exact D3D11+OpenXR use case. Mid-hooks are ideal for grabbing camera and matrix state without reconstructing whole functions. Signatures keep us working across game patches. |
| D3D11 hook | Our own dummy device + swapchain vtable hook of `Present`/`ResizeBuffers` (modelled on REFramework `D3D11Hook.cpp`) | kiero is archived. The pattern is about 300 lines and fully under our control. |
| XR | **OpenXR loader (vcpkg `openxr-loader`)**, `XR_KHR_D3D11_enable`, sharing the game's `ID3D11Device` | SteamVR's OpenXR runtime drives the Index natively. Sharing the device avoids cross-device copies. The REFramework `runtimes/OpenXR.cpp` + `D3D11Component.cpp` pair is the reference. |
| Math | **DirectXMath** | Same conventions as D3D and the game, header-only, SIMD. |
| UI | **Dear ImGui** (dx11 + win32), rendered to texture and shown as a flat overlay plus an `XrCompositionLayerQuad` in VR | Industry standard for mods; REFramework's OverlayComponent shows the VR quad path. |
| Logging / config | **spdlog** + **toml++** | Both MIT, both on vcpkg, and small to integrate. |
| Build | **CMake + vcpkg manifest, `x64-windows-static`, MSVC 2022, C++23, `/MT /EHa /MP /bigobj`, PDBs in Release** | Reproducible; static CRT means nothing extra in the game folder; `/EHa` makes SEH-guarded hooks behave. |
| Dev loop | Host DLL + hot-reloadable logic DLL; OpenXR-Simulator for work without the headset | Game restarts plus headset on/off cycles are the biggest time sink in flat2vr work. |
| RE | Ghidra 12 (+RTTI recovery, Sigga), x64dbg (+ret-sync), Cheat Engine, ReClass.NET, RenderDoc; IDA Free or Binja Free as second opinions | All free. This set covers static, dynamic, memory-structure and GPU-frame analysis. |

Avoid: kiero (archived), PIX and Nsight (no real D3D11 support), ReShade as the main host, copying UEVR code (all rights reserved), copying GPL code (Special K, OpenComposite, RSL) into a mod we may want to license permissively.

---

## 8. Suggested first RE steps once the game is installed

1. **Record the build.** Note the Steam build ID from `steamapps/appmanifest_667720.acf` and the SHA-256 of `rfg.exe`. Back up the exe. Consider turning off auto-update for the game while developing.
2. **Check for SteamStub.** Run Steamless on a **copy** of `rfg.exe`. If it unpacks, load `rfg.unpacked.exe` in Ghidra. For running and debugging, put `steam_appid.txt` (`667720`) next to the exe so it starts without the Steam launcher (Steam must still be running).
3. **Inspect the PE.** Check imports (which of `dinput8`, `xinput1_x`, `version`, `winmm`, `d3d11`, `dxgi` it loads; this picks our proxy name), RTTI presence (search strings for `.?AV`), any PDB path, and the ASLR flag. API Monitor can confirm which DLLs load at startup and the D3D11 device and swapchain creation parameters.
4. **Remove conflicting injectors** (ReShade, Special K, RTSS or other overlays) and note whether the Steam overlay is on.
5. **Capture frames in RenderDoc** (launch the exe directly, or Steam with child-process capture). Write down: render pass order, G-buffer and depth formats, **reverse-Z or not**, which VS cbuffer and offset holds View / Proj / ViewProj, where the HUD and UI draws start (a clean split point for HUD-to-quad), post-process chain, and backbuffer size and format.
6. **Find the camera in Cheat Engine.** Get camera position (move and scan), FOV (change it in the options menu), and view-matrix candidates (rotate the camera and scan floats in [-1,1]). Use "Find out what writes to this address" to get the writer functions and note their RVAs.
7. **Map structs in ReClass.NET.** Map the camera object (position, orientation matrix, FOV, near/far), the player entity (transform, aim direction) and the renderer/view object. Compare with `rfg-modding/RFGR_Types` and RSL's camera code. Export C++ headers.
8. **Analyse in Ghidra.** Run auto-analysis, then `RecoverClassesFromRTTIScript`. Jump to the writer RVAs from step 6 and find the camera update, projection-matrix build and view-matrix build functions, plus input polling (XInput or DInput) and the HUD render entry. Rename and type everything, and keep the Ghidra project under version control (export or `.gzf`).
9. **Make signatures.** Use Sigga for each hook target and check each one is unique. Store them in a single `signatures.hpp` with a comment naming the build.
10. **Build the "hello hook" DLL.** Proxy loads it, spdlog writes a log file, SafetyHook hooks `IDXGISwapChain::Present`, and an ImGui overlay shows live camera values read through the struct from step 7. This proves the whole toolchain end to end.
11. **Override the camera flat.** Use a SafetyHook inline or mid hook on the view-matrix build to apply an offset and rotation from ImGui sliders. If the game camera moves correctly, the 6-DOF pose injection point is confirmed.
12. **Stand up OpenXR.** In the same DLL, create the instance and session with the game's `ID3D11Device`. First show the flat backbuffer as a quad layer in the Index (a mono "cinema" mode), then move on to per-eye projection and view (AFR, or double rendering if the engine allows it). Use OpenXR-Simulator for quick checks without the headset.
13. **Keep x64dbg ready** for crashes. Load our PDB and use ret-sync with Ghidra. Only turn on ScyllaHide if an anti-debug check shows up.
