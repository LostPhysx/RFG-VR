# 02 - OpenXR Resources for the RFG Re-Mars-tered 6DOF VR Mod

Scope: Red Faction Guerrilla Re-Mars-tered, an injected DLL (flat2vr style), OpenXR, Valve Index through SteamVR.

**The game is 32-bit.** `rfg.exe` is PE32 x86 with `LARGE_ADDRESS_AWARE`, so it gets 4 GB of user address space on 64-bit Windows. It imports d3d11, D3DCOMPILER_43, XINPUT1_3, DINPUT8 and MSVCR100. **The mod DLL must be built for x86.** Section 0a covers what that means for OpenXR.
Research date: 2026-09-25. Every URL listed here was fetched or checked (HTTP 200) on that date. Where a claim is my own inference and not something I verified, it is labelled **(inference)**.

---

## 0. Key takeaways (read this first)

0. **32-bit is workable, but only recently.** Until **SteamVR 2.17** (stable 2026-09-10), SteamVR's OpenXR runtime was 64-bit only, so a 32-bit process found no runtime. 2.17 "Added support for 32-bit OpenXR applications" and registers a 32-bit runtime next to the 64-bit one. On this machine: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` → `...\SteamVR\steamxr_win32.json` → `bin\vrclient.dll`. Khronos ships a prebuilt **Win32 `openxr_loader.dll`** (static CRT, imports only KERNEL32/ADVAPI32) and Win32 validation layers. The 32-bit path is brand new, though, so treat it as the **main project risk**: keep OpenVR (32-bit `openvr_api.dll`) and an out-of-process 64-bit bridge as fallbacks. See section 0a.
1. **Spec version.** OpenXR is at **1.1.63** (SDK `release-1.1.63`, 2026-09-02). Headers and loader come from `KhronosGroup/OpenXR-SDK`. `hello_xr` (inside `OpenXR-SDK-Source`) is still the canonical D3D11 sample. There is **no** `KhronosGroup/OpenXR-SDK-Samples` repo (it returns 404).
2. **SteamVR supports what we need.** The Khronos OpenXR-Inventory entry for SteamVR (from runtime 2.14.4) lists `XR_KHR_D3D11_enable`, `XR_KHR_composition_layer_depth`, `XR_KHR_visibility_mask`, `XR_EXT_hand_tracking` (+ `hand_joints_motion_range`, `hand_tracking_data_source`), `XR_EXT_palm_pose`, `XR_EXT_dpad_binding`, `XR_VALVE_analog_threshold`, `XR_EXT_local_floor`, `XR_EXT_debug_utils`, `XR_KHR_win32_convert_performance_counter_time`, `XR_FB_display_refresh_rate` and `XR_META_recommended_layer_resolution`. It does **not** list cylinder, equirect or cube layers. Quad layers are core, so they work.
3. **Use the game's own `ID3D11Device` for the session.** Put it in `XrGraphicsBindingD3D11KHR`. UEVR does exactly this with the device it hooks. You must first call `xrGetD3D11GraphicsRequirementsKHR`, otherwise you get `XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`. The game's adapter LUID must match `adapterLuid`, and its feature level must be at least `minFeatureLevel`. After that you can `CopySubresourceRegion` the game's eye render targets straight into the swapchain images with no cross-device sharing.
4. **Swapchain images come back TYPELESS.** On D3D, `xrEnumerateSwapchainFormats` never returns typeless formats. The textures `xrCreateSwapchain` returns, however, *are* typeless, so you must create RTVs and SRVs with an explicit concrete format. `hello_xr` does this.
5. **sRGB is the most common source of "too bright / too dark" bugs.** The spec says `*_SRGB` formats mean the data is non-linear encoded and every other format is treated as linear. SteamVR lists sRGB formats first. Recommended path: create a `*_UNORM_SRGB` swapchain and copy the game's gamma-encoded `*_UNORM` backbuffer bytes in raw (same typeless family, so the copy is legal). That is effectively what UEVR does. Runtimes disagree on this (see SDK-Source #467 and openvr #1766), so **verify with a grey ramp**.
6. **Frame loop.** `xrWaitFrame` throttles and can be called from any thread (but not concurrently). `xrBeginFrame` and `xrEndFrame` may be on other threads if you synchronise them yourself. Every `xrWaitFrame` needs a matching `xrBeginFrame`, or the next wait deadlocks. The projection-layer `pose` and `fov` you pass to `xrEndFrame` **must be the ones you rendered with**.
7. **The SteamVR binding UI is unreliable for OpenXR apps.** Steam threads from 2022 show custom bindings failing to load and the interaction profile flipping to `XR_NULL_PATH` after edits. Ship good suggested bindings for `/interaction_profiles/valve/index_controller` plus fallbacks, and plan an **in-mod remap config**.
8. **Index profile quirks.** There is no `squeeze/click` (only `squeeze/value` and `squeeze/force`) and no app-usable menu button (`/input/system/*` "may not be available"). You get `a`/`b` click and touch, trigger click/value/touch, thumbstick and trackpad x/y (trackpad also has force and touch), grip and aim poses, and haptics.
9. **Projection.** Build per-eye asymmetric matrices from `XrFovf` using tangents. OpenXR is right-handed, +Y up, −Z forward. For a left-handed D3D game, negate Z of positions and map quaternions `(x,y,z,w) -> (-x,-y,z,w)`. Section 6 has code, including a reversed-Z infinite-far LH matrix.
10. **OpenVR is still a real option, and it has supported 32-bit for years.** UEVR supports both runtimes and says "OpenVR usually has the highest compatibility, OpenXR usually higher performance". On Index-only SteamVR, OpenVR has a mature input/binding UI and skeletal input. OpenXR is portable (Quest via VD/Link, WMR, Pimax and others) and is Valve's direction. Recommendation: **put the runtime behind an abstraction (like UEVR's `VRRuntime`) and implement OpenXR first.**

---

## 0a. 32-bit (x86) target: runtime, loader, fallbacks, memory

### 0a.1 SteamVR's 32-bit OpenXR runtime

| Resource | URL | Relevance | Takeaways |
|---|---|---|---|
| SteamVR 2.17 release notes (quoted by GamingOnLinux) | https://www.gamingonlinux.com/2026/09/steamvr-2-17-arrives-ready-to-go-for-the-steam-frame/ (official post: https://store.steampowered.com/news/app/250820/view/679635225181946198) | **High** | Verbatim: "Added support for 32-bit OpenXR applications. Users will be prompted to set SteamVR as their default OpenXR runtime after installing this Beta, which will set up the 32-bit runtime alongside the 64-bit runtime." The same release fixed `XR_EXT_view_configurations_changed` returning width for both width and height, and fixed the session field in `XrEventDataUserPresenceChangedEXT`. |
| VR.org: "SteamVR 2.17 Registers a 32-Bit OpenXR Runtime" | https://vr.org/articles/steamvr-2-17-stable-32-bit-openxr-runtime-2026 | Med | 2.17 went stable on 2026-09-10. Before that, 32-bit apps "found no registered runtime at all". The loader reads `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1` for 32-bit processes. It does not document any limitations. |
| Khronos forum: "openxr_loader.dll doesn't use SteamVR (…32-bit/x86 support isn't implemented)" | https://community.khronos.org/t/openxr-loader-dll-doesnt-use-steamvr-edit-because-32-bit-x86-support-isnt-implemented/108980 | Med (history) | 2022: mbucchia confirmed WMR and Oculus runtimes supported 32-bit, while SteamVR, Varjo and PimaxXR did not. The only advice then was to port to 64-bit. This explains why no existing flat2vr project has exercised the 32-bit SteamVR OpenXR path. |
| openvr issue #1687 (32-bit apps) | https://github.com/ValveSoftware/openvr/issues/1687 | Med | Maintainer reply: "OpenVR supports 32-bit applications. On the other hand, the OpenXR implementation in SteamVR does not support them" (written before 2.17). |
| Proton RFE #6808 (32-bit OpenXR) | https://github.com/ValveSoftware/Proton/issues/6808 | Low | Linux/Proton side of the same gap. Irrelevant on Windows. |

Locally confirmed facts (from the coordinator's inspection):
- `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` = `...\SteamVR\steamxr_win32.json`, whose `library_path` is `bin\vrclient.dll` (32-bit, present).
- The 64-bit key points to `steamxr_win64.json` → `vrclient_x64.dll`.

**Known issues and limitations of the 32-bit path.** I found **no public bug reports or documented limitations yet**; it has been stable for about two weeks. Things to verify early on the target machine **(inference / test plan)**:
1. Run `hello_xr` built for Win32 with `-g D3D11`, or a minimal x86 test, and compare its extension list against the x64 list. The Inventory JSON was generated from the 64-bit runtime.
2. Check `XR_KHR_D3D11_enable`, swapchain format order, `XR_KHR_composition_layer_depth`, hand tracking and haptics under x86.
3. Measure the address-space footprint of `vrclient.dll` and its dependencies after `xrCreateSession`, using VMMap or Process Explorer.
4. Check stability over long sessions, and confirm that users running SteamVR older than 2.17 get a clear error. The loader returns `XR_ERROR_RUNTIME_UNAVAILABLE` / `XR_ERROR_RUNTIME_FAILURE` when the WOW6432Node key is missing, and we should show "update SteamVR to 2.17+ and set it as OpenXR runtime". Users who never accepted the "set as default OpenXR runtime" prompt after 2.17 may also lack the 32-bit key.

### 0a.2 The OpenXR loader for x86

- **Prebuilt, official.** The `openxr_loader_windows-1.1.63.zip` release asset (https://github.com/KhronosGroup/OpenXR-SDK-Source/releases) contains `Win32/bin/openxr_loader.dll` (PE machine 0x14c, i386), `Win32/lib/openxr_loader.lib`, and `Win32/bin/api_layers/XrApiLayer_core_validation.dll`, `XrApiLayer_api_dump.dll` and `XrApiLayer_best_practices_validation.dll`. The **NuGet package `OpenXR.Loader.1.1.63.nupkg`** contains `native/Win32/release/{bin,lib}` plus `build/native/OpenXR.Loader.targets` for MSBuild. I checked the DLL: it imports only `KERNEL32.dll` and `ADVAPI32.dll`, so there is no MSVC runtime dependency to clash with the game's MSVCR100.
- **vcpkg:** the `openxr-loader` port (https://vcpkg.io/en/package/openxr-loader.html) supports every Windows triplet except UWP and ARM, so `vcpkg install openxr-loader:x86-windows` (or `x86-windows-static`) is supported.
- **From source:** OpenXR-SDK-Source with CMake `-A Win32` (see `BUILDING.md`). A plain CMake build produces a **static** loader library by default; add `-DDYNAMIC_LOADER=ON` to get the DLL. Linking statically into our x86 mod DLL avoids shipping or locating `openxr_loader.dll` in the game folder.
- **x86 header gotcha:** with `XR_PTR_SIZE == 4`, `openxr_platform_defines.h` / `openxr.h` define **every handle as `typedef uint64_t`**. On x86, `XrInstance`, `XrSession`, `XrSpace`, `XrSwapchain`, `XrAction` and so on are all the same type. You lose type safety, and C++ overloads keyed on handle type collide (for example `destroy(XrSpace)` vs `destroy(XrSwapchain)`). Pass-by-value is a 64-bit integer; this is ABI-correct but has a different struct layout from x64. Wrap handles in small strong-typed structs, or use **OpenXR-Hpp**, whose handle classes restore type safety.
- **Calling convention:** `XRAPI_CALL` is `__stdcall` on Win32. Always use the `PFN_xr*` typedefs from the headers for anything fetched with `xrGetInstanceProcAddr`, or you corrupt the stack.

### 0a.3 Fallback options if the 32-bit OpenXR path is not good enough

| Option | How | Pros | Cons |
|---|---|---|---|
| **A. In-process OpenXR x86** (primary) | Win32 loader → `steamxr_win32.json` → 32-bit `vrclient.dll` | Simplest. Same-device texture copy. Lowest latency | Brand-new runtime path, untested by other mods. Needs SteamVR 2.17+. The runtime's DLLs consume address space inside the 4 GB process |
| **B. In-process OpenVR x86** | Ship 32-bit `openvr_api.dll` from `ValveSoftware/openvr` `bin/win32` (v2.15.6, 2026-03-27). `VR_Init(VRApplication_Scene)`, `IVRCompositor::WaitGetPoses/Submit(Texture_t{ID3D11Texture2D*, TextureType_DirectX})` | 32-bit support proven for years. Works on SteamVR < 2.17. Mature SteamVR Input binding UI and skeletal input. Submit takes any D3D11 texture, so there are no swapchains | SteamVR only. Also loads `vrclient.dll` in-process (similar address-space cost to option A) |
| **C. Out-of-process 64-bit bridge** | The x86 mod DLL renders eyes into textures created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE \| D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` (or legacy `MISC_SHARED` with `IDXGIResource::GetSharedHandle`, a global handle usable as a plain integer). `CreateSharedHandle` → `DuplicateHandle` into a 64-bit helper exe, which opens them with `ID3D11Device1::OpenSharedResource1` on the **same adapter (LUID)** and runs a normal x64 OpenXR session. Sync with `IDXGIKeyedMutex` or a shared `ID3D11Fence` (`D3D11_FENCE_FLAG_SHARED`, `ID3D11Device5::OpenSharedFence`, Win10 1703+). Poses and frame timing go back to the game over shared memory, and input the same way | Works with any 64-bit OpenXR runtime and SteamVR version. Keeps the runtime's DLLs and allocations **out of the 32-bit address space**. Kernel handles work across bitness | More complex. Adds roughly 1 frame of pose latency unless carefully pipelined (the game must render with poses predicted for the helper's `predictedDisplayTime`). Cross-process GPU sync. Two processes to manage. The helper's `xrWaitFrame` must drive the game's frame pacing over IPC |

Docs for C: `ID3D11Device::OpenSharedResource` https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource · `ID3D11Fence::CreateSharedHandle` https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-createsharedhandle · `ID3D11Device1::OpenSharedResourceByName` https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresourcebyname. Prior art for "in-game DLL reads textures produced by another process": OpenKneeboard's injectables (https://openkneeboard.com/internals/injectables/), which copy images out of shared memory into the game's rendering.

**Recommendation:** put the runtime behind an interface, as planned, and implement **A** first. Keep **B** as a cheap second backend: OpenVR x86 is proven, and the interface is small. Only build **C** if A and B both hit address-space exhaustion or blocking bugs.

### 0a.4 Memory in a 4 GB (LAA) address space

- **Per-texture VRAM cost** at Index-typical sizes. SteamVR's recommended size at 100% is roughly 2016×2240 per eye, but it varies with the SS setting. One RGBA8 eye image ≈ 18 MB. With 3 swapchain images × 2 eyes, OpenXR colour ≈ 108 MB, plus depth if submitted (≈108 MB more for D32/D24S8), plus our own intermediate eye RTs. The game's **own** render targets (G-buffer, HDR, post chain) scale with render resolution too; rendering at VR resolution can multiply them several times over.
- **VRAM is not process address space, but it can consume it.** Microsoft's "Windows and Video Memory" (https://learn.microsoft.com/en-us/archive/blogs/tmulcahy/windows-and-video-memory) says **"Large mapable surfaces consume large amounts of virtual address space"**: D3D reserves VA when a *mappable* resource (dynamic/staging/CPU-access) is created, while non-mappable default-usage surfaces do not on Vista SP1 and later. So: create every render target and swapchain-copy texture as `D3D11_USAGE_DEFAULT` with no CPU access, and avoid large staging readbacks.
- **Driver reservations:** an NVIDIA forum case (https://forums.developer.nvidia.com/t/directx11-excessive-virtual-memory-usage-with-createtexture2d/154505) showed Commit Size rising by 298 MB for one 18 MB 8×MSAA RGBA32F render target. NVIDIA's answer was that this is a reservation, not real memory. In a 64-bit process that doesn't matter; **in a 32-bit process, reserved VA is exactly the scarce resource (inference).** Avoid MSAA on large VR targets, avoid unnecessary float formats, and measure.
- **Runtime DLLs in-process:** options A and B both load SteamVR's `vrclient.dll` and its dependencies (and on NVIDIA, extra UMD components) into the 32-bit process. Their size and heap use are unmeasured (**to test with VMMap**). This is the main argument for option C if the game already runs near its limit.
- **Fragmentation:** in 32-bit processes, OOM failures often appear well before 4 GB because large *contiguous* reservations fail. Allocate VR resources **once at startup, before the game fragments its heap**, and never recreate swapchains per resolution change more than necessary. UEVR, for example, recreates on resolution change; we should rate-limit that.
- Tools: Sysinternals VMMap / Process Explorer to see what occupies the address space. Watch "Dedicated GPU memory" separately in Task Manager.

---

## 1. Official Khronos resources

| Resource | URL | Relevance | What it is and takeaways |
|---|---|---|---|
| OpenXR Registry (landing) | https://registry.khronos.org/OpenXR/ | High | Hub for the spec (HTML/PDF), reference pages, loader doc and CTS guide. Two spec flavours: "API + all published extensions" and "API + ratified (KHR) extensions". |
| OpenXR 1.1 spec (all extensions, single HTML) | https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html | High | Titled "OpenXR 1.1.63". Over 10 MB, so read it in a browser or use the man pages and adoc sources below. Chapters you need: Rendering (swapchains, frame loop, layers), Input (actions), Spaces, Semantic Paths (interaction profiles). |
| Reference ("man") pages | https://registry.khronos.org/OpenXR/specs/1.1/man/html/openxr.html | High | One page per function or struct. Examples: `XrGraphicsBindingD3D11KHR`, `XrSwapchainImageD3D11KHR`, `XrCompositionLayerDepthInfoKHR`, `XrCompositionLayerQuad`, `xrWaitFrame`, `xrLocateViews`, `XrFovf`, `XrHapticVibration`, `xrSuggestInteractionProfileBindings`. Pattern: `.../man/html/<Name>.html`. |
| Spec sources (AsciiDoc) | https://github.com/KhronosGroup/OpenXR-Docs | High (practical) | Easiest way to grep the exact normative text. Useful files: `specification/sources/chapters/rendering.adoc`, `session.adoc`, `spaces.adoc`, `semantic_paths.adoc`, `extensions/khr/khr_d3d11_enable.adoc`. |
| OpenXR-SDK (pre-generated headers + loader) | https://github.com/KhronosGroup/OpenXR-SDK (releases: https://github.com/KhronosGroup/OpenXR-SDK/releases) | High | **Consume this one.** Contains `openxr.h`, `openxr_platform.h` (define `XR_USE_GRAPHICS_API_D3D11` and `XR_USE_PLATFORM_WIN32` before including) and the prebuilt Windows loader. The NuGet `OpenXR.Loader` package and the SDK-Source `openxr_loader_windows-*.zip` both include **Win32 (x86)** builds, which is what we need. Latest releases: 1.1.63 (2026-09-02), 1.1.62 (2026-08-01), 1.1.61 (2026-07-06). |
| OpenXR-SDK-Source | https://github.com/KhronosGroup/OpenXR-SDK-Source | High | Loader source, `hello_xr`, the API layers `XR_APILAYER_LUNARG_core_validation` and `api_dump`, and `src/common/xr_linear.h` (math helpers). Its own README recommends plain OpenXR-SDK if you only need to build an app. |
| hello_xr | https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr | High | `graphicsplugin_d3d11.cpp` is the reference D3D11 path. It creates the device on `adapterLuid`, filters feature levels by `minFeatureLevel`, prefers `R8G8B8A8_UNORM_SRGB` > `B8G8R8A8_UNORM_SRGB` > `R8G8B8A8_UNORM` > `B8G8R8A8_UNORM` for colour and `D32_FLOAT` > `D24_UNORM_S8_UINT` > `D16_UNORM` > `D32_FLOAT_S8X24_UINT` for depth, and creates RTVs with the swapchain's create-info format because "swapchain is typeless". `openxr_program.cpp` shows the full event, session and frame loop. |
| xr_linear.h | https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/common/xr_linear.h | High | `XrMatrix4x4f_CreateProjectionFov`: the canonical asymmetric-FOV projection. It is right-handed and column-major; setting `farZ <= nearZ` gives an infinite far plane. It also has pose→matrix and inverse helpers. Section 6 ports it to LH. |
| OpenXR Tutorials (openxr-tutorial.com) | https://openxr-tutorial.com/ ; D3D11 track: https://openxr-tutorial.com/windows/d3d11/2-setup.html , https://openxr-tutorial.com/windows/d3d11/3-graphics.html , https://openxr-tutorial.com/windows/d3d11/4-actions.html , https://openxr-tutorial.com/windows/d3d11/5-extensions.html ; source: https://github.com/KhronosGroup/OpenXR-Tutorials | High | Official step-by-step tutorial with a **Windows/D3D11 track**. Chapter 3 covers swapchain format selection, the usage-flag→bind-flag table, depth swapchains and frame ordering. Chapter 4 covers actions and bindings. Chapter 5 covers extensions such as hand tracking. It is the best learning path for someone new to OpenXR. |
| OpenXR-Guide | https://github.com/KhronosGroup/OpenXR-Guide (e.g. `chapters/frame_submission.md`) | Med-High | Khronos best-practice guide. Frame submission: one `xrBeginFrame` per `xrWaitFrame`, no overlapping calls, locate poses at `predictedDisplayTime` in LOCAL/STAGE, and advance animation by the delta between predicted display times. |
| Loader design doc | https://registry.khronos.org/OpenXR/specs/1.1/loader.html (sources in `OpenXR-SDK-Source/specification/loader/*.adoc`) | High | Runtime and layer discovery (details in section 5). |
| Conformance (CTS usage + conformant products) | https://registry.khronos.org/OpenXR/conformance/cts_usage.pdf ; https://www.khronos.org/conformance/adopters/conformant-products/openxr ; https://github.com/KhronosGroup/OpenXR-CTS | Low | Only useful for checking a runtime's conformance status. SteamVR is `conformance_submission: 11` in the Inventory. |
| OpenXR-Inventory | https://github.com/KhronosGroup/OpenXR-Inventory (SteamVR: `runtimes/valve_steamvr.json`) | High | Public, machine-readable list of extensions per runtime. The SteamVR list is quoted in the takeaways (last updated 2025-12-16, SteamVR 2.14.4). Recheck it with OpenXR Explorer on the target machine. |
| OpenXR-Hpp | https://github.com/KhronosGroup/OpenXR-Hpp | Low-Med | C++ projection (`openxr.hpp`) with RAII and typed enums. You generate it from scripts. It is optional; the C API is fine and is what UEVR and hello_xr use. |
| "Interaction in OpenXR" (Khronos class, with notes) | https://www.khronos.org/developers/linkto/interaction-in-openxr-with-notes | Med | Conceptual walkthrough of actions, action sets, suggested bindings and interaction profiles. |
| OpenXR 1.1 press release | https://www.khronos.org/news/press/khronos-releases-openxr-1.1-to-further-streamline-cross-platform-xr-development | Low | 1.1 (April 2024) promoted extensions to core, for example `xrLocateSpaces` and the `grip_surface` pose (formerly `XR_EXT_palm_pose`). |

### 1a. Exact spec text worth knowing (from OpenXR-Docs sources)

- **Colour space** (rendering.adoc): "Images intended to be interpreted as being non-linear-encoded ("sRGB") must be created using an API-specific "sRGB" format (e.g. DXGI_FORMAT_R8G8B8A8_UNORM_SRGB …) … All other formats will be treated as linear values." It also says to avoid submitting linear 8-bit data because of banding.
- **Typeless** (rendering.adoc): "With a Direct3D-based graphics API, the swapchain returned by xrCreateSwapchain will be a typeless format if the requested format has a typeless analogue. Applications are required to reinterpret the swapchain as a compatible non-typeless type." And: "xrEnumerateSwapchainFormats never returns typeless formats."
- **D3D11 bind flags** (khr_d3d11_enable.adoc): `COLOR_ATTACHMENT`→`D3D11_BIND_RENDER_TARGET`, `DEPTH_STENCIL_ATTACHMENT`→`D3D11_BIND_DEPTH_STENCIL`, `UNORDERED_ACCESS`→`D3D11_BIND_UNORDERED_ACCESS`, `SAMPLED`→`D3D11_BIND_SHADER_RESOURCE`. `TRANSFER_SRC`, `TRANSFER_DST` and `MUTABLE_FORMAT` are *ignored* on D3D11. "The runtime may set additional bind flags but must not restrict usage." The image origin is top-left.
- **Frame throttling** (rendering.adoc): "xrWaitFrame must be callable from any thread, including a different thread than xrBeginFrame/xrEndFrame", "Calling xrWaitFrame must be externally synchronized", "A subsequent xrWaitFrame call must block until the previous frame has been begun with xrBeginFrame", and "Applications should call xrBeginFrame right before executing any graphics device work for a given frame".
- **Depth layer** (`XrCompositionLayerDepthInfoKHR`): `minDepth < maxDepth`, both in [0,1]. `nearZ != farZ`, both in (0, +inf]. **`nearZ > farZ` means reversed-Z.** The depth swapchain must have `faceCount == 1`.
- **Layer blending**: without `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` the layer alpha is treated as 1. If the texture is not premultiplied, set `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`.
- **Spaces** (spaces.adoc): VIEW, LOCAL and STAGE are all "+Y up, +X to the right, and −Z forward". STAGE's origin is on the floor at the centre of the play area.

---

## 2. D3D11 specifics

| Resource | URL | Relevance | Takeaways |
|---|---|---|---|
| `XR_KHR_D3D11_enable` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D11_enable.html | High | Revision 11. Structs: `XrGraphicsBindingD3D11KHR {device}`, `XrGraphicsRequirementsD3D11KHR {adapterLuid, minFeatureLevel}`, `XrSwapchainImageD3D11KHR {ID3D11Texture2D* texture}`. The function `xrGetD3D11GraphicsRequirementsKHR` must be loaded through `xrGetInstanceProcAddr`. |
| `XrGraphicsBindingD3D11KHR` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrGraphicsBindingD3D11KHR.html | High | A wrong adapter or feature level returns `XR_ERROR_GRAPHICS_DEVICE_INVALID`. |
| `XrSwapchainImageD3D11KHR` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchainImageD3D11KHR.html | High | Pass an array of these, each with `type` set, to `xrEnumerateSwapchainImages` cast to `XrSwapchainImageBaseHeader*`. |
| `XR_KHR_D3D12_enable` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable.html | Low | Not needed because RFG-R is D3D11. It only matters if we ever interop to D3D12. UEVR has both paths (`D3D11Component`/`D3D12Component`). |
| `XR_KHR_composition_layer_depth` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_composition_layer_depth.html ; struct: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerDepthInfoKHR.html | Med | Chain `XrCompositionLayerDepthInfoKHR` into each `XrCompositionLayerProjectionView::next`. UEVR sets `minDepth=0`, `maxDepth=1`, `nearZ=FLT_MAX` and `farZ=near_plane_in_meters` for reversed-Z with a far plane at infinity. On SteamVR the benefit is unproven: UEVR's docs say depth "significantly reduces latency on Oculus headsets using OpenXR with the native Oculus runtime" and leave it off by default. A 2023 Steam thread reports SteamVR reprojection broken with GL depth but working through D3D11. Treat it as optional. |
| `XrCompositionLayerQuad` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerQuad.html | High (HUD/menus) | Core, so no extension is needed. `pose` plus `size` in metres. Only the front face is visible. The quad normal is +Z of its pose. It is ideal for RFG's HUD, map and menus: capture the UI into its own swapchain and float it in LOCAL/STAGE, or head-lock it in VIEW space (head-locking is less comfortable). The 2021 SteamVR feature thread says at most 16 layers. |
| `XR_KHR_visibility_mask` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_visibility_mask.html | Low-Med | Hidden-area mesh per eye. SteamVR supports it. Stencilling it out saves GPU, but only if we control the scene pass. |
| Microsoft OpenXR best practices | https://learn.microsoft.com/en-us/windows/mixed-reality/develop/native/openxr-best-practices ; sample: https://github.com/microsoft/OpenXR-MixedReality/tree/master/samples/BasicXrApp | Med | The clearest written explanation of the sRGB RTV trick: "request an sRGB swapchain format but use the linear format for the render-target view" if your shader already outputs gamma-encoded values. It also recommends depth submission, reversed-Z (links NVIDIA's "Depth Precision Visualized": https://developer.nvidia.com/content/depth-precision-visualized), using the first enumerated format, and rendering at `recommendedImageRect*`. Its HoloLens-specific advice (texture arrays, avoid quads) matters less on SteamVR. |
| DisplayXR issue: TYPELESS copy crash | https://github.com/DisplayXR/displayxr-unity/issues/326 | Low (cautionary) | A runtime handed back `R8G8B8A8_TYPELESS` and a `CopySubresourceRegion` from `_UNORM_SRGB` into it hit an NVIDIA driver crash. Fix: make intermediate textures the TYPELESS parent too. Lesson: keep your intermediate or bridge textures in the **same typeless family** as the swapchain. |

### 2a. Texture sharing when the game owns the device

- **Preferred: same device.** Pass the game's hooked `ID3D11Device*` to the binding, as UEVR does in `D3D11Component::OpenXR::initialize`: `binding.device = hook->get_device()`. Then, **on the game's render thread** (inside the Present hook), call `ID3D11DeviceContext::CopySubresourceRegion` or `CopyResource` from the game's eye render target into `XrSwapchainImageD3D11KHR::texture`. No shared handles or keyed mutexes are involved.
- `CopyResource` needs identical dimensions and formats from the same typeless group. `R8G8B8A8_UNORM` → a swapchain created as `R8G8B8A8_UNORM_SRGB` (but actually typeless) is fine. B8G8R8A8 → R8G8B8A8 is **not** a legal copy; use a fullscreen blit shader. The same applies when you need scaling or cropping, or when the game's RT size differs from `recommendedImageRectWidth/Height`.
- **(inference)** SteamVR's D3D11 path uses the device's immediate context during `xrAcquire/Wait/ReleaseSwapchainImage` and `xrEndFrame`. The D3D11 immediate context is not thread-safe, so make every swapchain and `xrEndFrame` call from the thread that owns the game's immediate context (the Present thread), or serialise with a mutex. UEVR guards its D3D11 OpenXR component with a `std::recursive_mutex`. **Save and restore the game's D3D11 pipeline state** around your own draws; UEVR has a state-backup struct for this.
- The alternative (a separate device on the same adapter, shared through `IDXGIResource1::CreateSharedHandle` + `ID3D11Device1::OpenSharedResource1` + keyed mutex) adds latency and complexity. Only use it if the game device fails the requirements, which is unlikely on a single-GPU desktop.
- **Double-wide vs per-eye swapchains.** UEVR uses one double-wide colour swapchain (`width = eye_w*2`) and sets `imageRect.offset.x` per eye. hello_xr uses one swapchain per view. Both work on SteamVR. `arraySize=2` also works, but copying into array slices from a game that renders to a 2D RT is more awkward.

---

## 3. Input (actions, Index profile, haptics, hand tracking)

| Resource | URL | Relevance | Takeaways |
|---|---|---|---|
| Spec: Input chapter + Semantic Paths | spec HTML above; sources `input.adoc`, `semantic_paths.adoc` in https://github.com/KhronosGroup/OpenXR-Docs | High | **Index profile `/interaction_profiles/valve/index_controller`** (both hands): `/input/system/click,touch` (*may not be available to apps*), `/input/a/click,touch`, `/input/b/click,touch`, `/input/squeeze/value,force`, `/input/trigger/click,value,touch`, `/input/thumbstick/x,y,click,touch`, `/input/trackpad/x,y,force,touch`, `/input/grip/pose`, `/input/aim/pose`, `/output/haptic`. There is **no squeeze/click and no menu**. |
| Grip vs aim pose (spec text) | same | High | **Grip**: the position is at the palm centroid inside the controller handle. −Z runs through the tube formed by the non-thumb fingers (little finger to thumb). +X is normal to the palm. Use grip to attach weapons and hands. **Aim**: a pointing ray, "+Y up, +X right, −Z forward", often from the controller tip; use it for UI laser pointers. OpenXR 1.1 adds `/input/grip_surface/pose` (palm surface); SteamVR exposes the older `XR_EXT_palm_pose`. |
| Unity OpenXR "Valve Index Controller Profile" docs | https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.15//manual/features/valveindexcontrollerprofile.html | Low | A readable table of the same paths. Useful cross-check. |
| `xrSuggestInteractionProfileBindings` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrSuggestInteractionProfileBindings.html | High | Call once per profile. Suggest for Index **and** fallbacks (`/interaction_profiles/khr/simple_controller`, `oculus/touch_controller`, `htc/vive_controller`) so SteamVR can remap other controllers. Then call `xrAttachSessionActionSets` **exactly once** per session; action sets are immutable after that. |
| `XR_VALVE_analog_threshold` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_VALVE_analog_threshold.html | Med | `XrInteractionProfileAnalogThresholdVALVE` chains into the suggested bindings. It sets on/off thresholds and optional haptics when a float input such as `squeeze/value` or `trigger/value` is bound to a boolean action. This is useful for Index grip because there is no squeeze click. |
| `XR_EXT_dpad_binding` | (supported by SteamVR per Inventory) | Med | Lets you split thumbstick or trackpad into d-pad boolean actions, for example weapon wheel or quick-select. |
| `XrHapticVibration` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrHapticVibration.html | High | Call `xrApplyHapticFeedback(session, {action, subactionPath=/user/hand/right}, (XrHapticBaseHeader*)&vib)` with `vib.duration` in ns (or `XR_MIN_HAPTIC_DURATION`), `vib.frequency = XR_FREQUENCY_UNSPECIFIED` and `vib.amplitude` in 0..1. Use it for weapon fire, sledgehammer impacts and explosions. |
| `XR_EXT_hand_tracking` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_hand_tracking.html ; https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_hand_joints_motion_range.html | Med | Revision 4. Calls: `xrCreateHandTrackerEXT(hand)`, then `xrLocateHandJointsEXT` for 26 joints. On Index, SteamVR **synthesises** joints from finger-curl sensors. Use `XR_EXT_hand_joints_motion_range` to choose `..._CONFORMING_TO_CONTROLLER_EXT` (fingers wrap the handle) or `UNOBSTRUCTED`. `XR_EXT_hand_tracking_data_source` shows whether the data comes from a controller. Good for finger poses on the player's hands; not required for MVP. |
| Khronos forum: Index finger tracking | https://community.khronos.org/t/how-is-a-limited-finger-tracking-controller-like-index-controller-supposed-to-work/108265 | Med | Gotchas: `isActive` can be false for the first frames, and the reporter's real bug was using `XR_HAND_LEFT_EXT` for both trackers. OpenXR has no equivalent of OpenVR's `EVRSkeletalTrackingLevel`, so you cannot query how good the finger tracking is. |
| Steam thread: skeletal input & OpenXR | https://steamcommunity.com/app/250820/discussions/8/2448217320134903029/ | Low | Background on how curl values are turned into joint poses. |
| Steam threads: SteamVR binding UI with OpenXR | https://steamcommunity.com/app/250820/discussions/3/5089647632457888477/ ; https://steamcommunity.com/app/250820/discussions/8/3423312514218854296/ ; https://steamcommunity.com/app/250820/discussions/8/3193619419608654311/ | High (risk) | Reports from 2022 say custom bindings are "almost completely broken with OpenXR": edits set the interaction profile to null, input stops, and bindings don't persist. Current SteamVR may have improved, but that is unverified. Mitigations: excellent default suggested bindings, handling `XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED`, and an in-mod remap file. |
| openvr issue #1827 (tracker binding naming mismatch) | https://github.com/ValveSoftware/openvr/issues/1827 | Low | Example of SteamVR mapping OpenXR path names to its own names wrongly ("menu" vs "application_menu"). Assume path-name bugs are possible. |

**Action setup sketch (C API):**
```cpp
XrPath L = path("/user/hand/left"), R = path("/user/hand/right");
XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO}; strcpy(asci.actionSetName,"gameplay"); strcpy(asci.localizedActionSetName,"Gameplay");
xrCreateActionSet(instance,&asci,&set);
auto mk=[&](const char* n, XrActionType t, bool hands){ XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
  strcpy(ci.actionName,n); strcpy(ci.localizedActionName,n); ci.actionType=t;
  XrPath sp[2]={L,R}; if(hands){ci.countSubactionPaths=2; ci.subactionPaths=sp;} XrAction a; xrCreateAction(set,&ci,&a); return a; };
gripPose = mk("grip_pose", XR_ACTION_TYPE_POSE_INPUT, true);
aimPose  = mk("aim_pose",  XR_ACTION_TYPE_POSE_INPUT, true);
fire     = mk("fire",      XR_ACTION_TYPE_FLOAT_INPUT, true);   // trigger/value
move     = mk("move",      XR_ACTION_TYPE_VECTOR2F_INPUT, true);// thumbstick
haptic   = mk("haptic",    XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
// suggest bindings for /interaction_profiles/valve/index_controller (+ simple/touch/vive fallbacks)
// xrAttachSessionActionSets(session, {set})   <- once per session
// per hand: xrCreateActionSpace(session,{gripPose, subactionPath=L/R, poseInActionSpace=identity})
// per frame: xrSyncActions (only works while FOCUSED) -> xrGetActionState* -> xrLocateSpace(handSpace, stageSpace, predictedDisplayTime)
```

---

## 4. SteamVR runtime specifics & known quirks

| Resource | URL | Relevance | Takeaways |
|---|---|---|---|
| SteamVR OpenXR sub-forum | https://steamcommunity.com/app/250820/discussions/8/ | High | Valve developers occasionally answer here. It is the best place to search for runtime bugs. |
| "[Resource] List of supported OpenXR features" (2021-2022) | https://steamcommunity.com/app/250820/discussions/8/3121550424355682585/ | Med | History of when extensions were added: `XR_EXT_dpad_binding` in 1.21.5, `XR_FB_display_refresh_rate` (read-only on Index) in 1.22.2, `XR_EXT_palm_pose` in 1.23.4. At the time: at most 16 layers and no cylinder, equirect or cube layers. The Inventory entry above is more current. |
| "OpenXR bugs and spec violations" | https://steamcommunity.com/app/250820/discussions/3/4034725980849397816/ | High | Reported issues: `UNORDERED_ACCESS` usage originally ignored (fixed in 2.2.3); oversized recommended resolution (from a GPU benchmark at startup); **resource state transitions done in `xrWaitSwapchainImage` rather than acquire** (D3D12 debug-layer noise); dimensions rounded to multiples of 4 rather than 16; a 1:1 aspect ratio. sRGB formats are now enumerated first. |
| openvr #1766: extra linear→sRGB conversion | https://github.com/ValveSoftware/openvr/issues/1766 | High (risk) | On SteamVR 1.27.5 (Linux/Vulkan, Index) colours came out too bright for both sRGB and float swapchains. It is not confirmed on Windows D3D11, but it is the reason for the grey-ramp test. |
| SDK-Source #467: runtimes disagree on colour | https://github.com/KhronosGroup/OpenXR-SDK-Source/issues/467 | Med | The same hello_xr looked different on Meta, Valve and Pico. Valve treats the choice of sRGB format as a statement about the data's gamma. |
| Steam: xrEndFrame blocks instead of xrWaitFrame | https://steamcommunity.com/app/250820/discussions/8/3001046778348674981/ | Med | Old (2020-21) but instructive. Valve moved throttling into `xrWaitFrame` in 1.15.10, and `xrEndFrame` still briefly blocks for a GPU flush. Later the blocking shifted to `xrBeginFrame` in D3D11 mode (1.16.6). **Profile each call; do not assume where time is spent.** |
| Steam: async reprojection broken with GL depth | https://steamcommunity.com/app/250820/discussions/8/3845556684657690924/ | Low | Depth plus reprojection works through D3D11 but was broken through GL (2023-24). We are D3D11, so this is fine. |
| Runtime manifest / switching | 64-bit apps: `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` → `...\SteamVR\steamxr_win64.json` (e.g. https://steamcommunity.com/app/250820/discussions/8/2448217320142984311/). **32-bit apps (RFG): `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` → `...\SteamVR\steamxr_win32.json` (`bin\vrclient.dll`), registered by SteamVR 2.17+** | High | Set it from SteamVR Settings → OpenXR → "Set SteamVR as OpenXR runtime" (2.17+ registers both), or with OpenXR Explorer. Per-process override: `XR_RUNTIME_JSON=<path to steamxr_win32.json>`, handy for testing without changing the system default. For our x86 process it **must** be the win32 manifest. |
| Rectus openxr-steamvr-passthrough | https://github.com/Rectus/openxr-steamvr-passthrough | Low | Real-world OpenXR API layer on SteamVR, including depth-blending use of `XR_KHR_composition_layer_depth`. Worth reading for its D3D11 layer code. |

**SteamVR-relevant facts:** `XR_FB_display_refresh_rate` is read-only on Index, so the refresh rate (80/90/120/144 Hz) is chosen in SteamVR. `XR_META_recommended_layer_resolution` is available. Motion Smoothing / reprojection is SteamVR-controlled; the app cannot turn it on or off through OpenXR (no extension for it in the inventory). The recommended resolution follows the SteamVR supersampling setting.

---

## 5. Tooling, debugging, API layers

| Resource | URL | Relevance | Takeaways |
|---|---|---|---|
| OpenXR Explorer (maluoi) | https://github.com/maluoi/openxr-explorer | High | v1.7 (2025-05). GUI and CLI: shows the active runtime, extensions, formats and view configurations with spec links, and switches runtimes (`xrsetruntime` CLI). Run it on the Index machine to confirm the actual extension list and swapchain format order. |
| Core validation + api_dump layers | https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/api_layers/README_core_validation.md ; https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/api_layers/README_api_dump.md | High | `XR_APILAYER_LUNARG_core_validation` enforces valid-usage rules. `XR_APILAYER_LUNARG_api_dump` logs every call. **Prebuilt Win32 versions ship in `openxr_loader_windows-*.zip` (`Win32/bin/api_layers/`)**; alternatively build them from SDK-Source. Point `XR_API_LAYER_PATH` at the manifests, and set `XR_ENABLE_API_LAYERS=XR_APILAYER_LUNARG_core_validation` (semicolon-separated on Windows). Output controls: `XR_CORE_VALIDATION_EXPORT_TYPE` and `XR_CORE_VALIDATION_FILE_NAME`. Development only. |
| Loader env/registry (loader doc) | https://registry.khronos.org/OpenXR/specs/1.1/loader.html | High | Environment variables: `XR_RUNTIME_JSON`, `XR_API_LAYER_PATH`, `XR_ENABLE_API_LAYERS`, `XR_LOADER_DEBUG` (`=all` for verbose loader logs). Registry: `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` and `AvailableRuntimes` (**32-bit processes read `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\...` instead, and the same applies to implicit and explicit layer keys, so x64-only layers are not injected into RFG**); implicit layers under `HKLM` or `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`, explicit layers under `...\ApiLayers\Explicit`. **Third-party implicit layers (OpenXR Toolkit, OBS/overlay layers, Vive layers) get injected into our process too.** If something breaks, check for them first; the Godot jam thread notes Vive layers breaking controller tracking. |
| `XR_EXT_debug_utils` | https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_debug_utils.html | Med | SteamVR supports it. Register a messenger to get runtime and validation messages in our log. |
| OpenXR-Layer-Template (mbucchia) | https://github.com/mbucchia/OpenXR-Layer-Template | Med | VS2019+, NuGet and Python. A clean template for writing an API layer (`xrNegotiateLoaderApiLayerInterface`, dispatch). Useful if we want a debug layer that dumps our frames, or as a reference for `xrGetInstanceProcAddr` chaining. It is not a replacement for our game-side hooks. |
| mbucchia's other layers (archived) | https://github.com/mbucchia/_ARCHIVE_XR_APILAYER_NOVENDOR_fov_modifier ; https://github.com/mbucchia/_ARCHIVE_XR_APILAYER_NOVENDOR_nis_scaler ; https://github.com/mbucchia/Quad-Views-Foveated ; https://github.com/mbucchia/OpenXR-Vk-D3D12 | Low | Readable examples of D3D11/D3D12 swapchain interception and FOV modification. |
| OpenXR Toolkit | https://github.com/mbucchia/OpenXR-Toolkit (releases: https://github.com/mbucchia/OpenXR-Toolkit/releases) | Low | Upscaling, foveated rendering, hand-to-controller and similar. **Development and support have ended** (https://forums.mudspike.com/t/the-end-of-the-openxr-toolkit/16520). Worth knowing because users may have it installed as an implicit layer, which is a debugging hazard. |
| RenderDoc | https://github.com/baldurk/renderdoc/issues/2921 | Med | Pure OpenXR apps have no Present, so RenderDoc cannot delimit frames. **Our case is easier**: the game still calls `IDXGISwapChain::Present`, so RenderDoc captures the game frame, including our copies and blits into the XR swapchain images. We will not see what the compositor does. Launch through RenderDoc with the mod injected, or use its in-app API (`renderdoc_app.h`) to trigger captures. **Tested 2026-09-26: not usable for RFG in-level; the 32-bit process runs out of address space (see 00-local-findings).** |
| OpenComposite (OpenVR→OpenXR) | https://gitlab.com/znixian/OpenOVR | Low-Med | Shows how OpenVR concepts (Submit, poses, input) map to OpenXR. Relevant if we write OpenVR first and want OpenXR later, or the reverse. |

---

## 6. Math: poses, projection, handedness, reversed-Z

**Conventions.** OpenXR spaces are right-handed: +X right, +Y up, −Z forward. Units are metres. `XrPosef` = `{XrQuaternionf orientation (x,y,z,w), XrVector3f position}`. `XrFovf` angles are in radians. `angleLeft` and `angleDown` are usually **negative**, and FOVs are **asymmetric** per eye. Always use `views[i].pose` per eye, which already contains the IPD offset. Do not assume the eyes are parallel.

**Canonical RH projection** (xr_linear.h, `GRAPHICS_D3D` path, column-major, clip z in [0,1]):
```
tanL=tan(fov.angleLeft) tanR=tan(fov.angleRight) tanU=tan(fov.angleUp) tanD=tan(fov.angleDown)
W = tanR - tanL ; H = tanU - tanD       // D3D: +Y up in clip space
m[0]=2/W   m[8]=(tanR+tanL)/W
m[5]=2/H   m[9]=(tanU+tanD)/H
m[10]=-far/(far-near)   m[14]=-(far*near)/(far-near)
m[11]=-1   (all others 0)               // if far<=near: m[10]=-1, m[14]=-near (infinite far)
```

**Left-handed (D3DX/DirectXMath, row-vector) equivalent.** This is the probable RFG convention, but it must be verified in RE (see the engine research doc):
```cpp
// Standard LH, depth 0..1 (near->0):
XMMATRIX P = XMMatrixPerspectiveOffCenterLH(n*tanL, n*tanR, n*tanD, n*tanU, n, f);

// Reversed-Z, infinite far, LH, row-vector (near->1, far->0), best precision for big open worlds:
XMMATRIX ProjRevInfLH(const XrFovf& fov, float n){
  float tL=tanf(fov.angleLeft), tR=tanf(fov.angleRight), tU=tanf(fov.angleUp), tD=tanf(fov.angleDown);
  return XMMATRIX( 2/(tR-tL), 0,          0, 0,
                   0,         2/(tU-tD),  0, 0,
                  -(tR+tL)/(tR-tL), -(tU+tD)/(tU-tD), 0, 1,
                   0,         0,          n, 0 );   // depth = n / z_view
}
// Matching depth layer: minDepth=0,maxDepth=1, nearZ=+INF (or FLT_MAX, as UEVR does), farZ=n  (nearZ>farZ => reversed)
```
Check: NDC x = (2·x/z − (tR+tL))/(tR−tL) gives ±1 at x/z = tR or tL. Depth is 1 at z=n and approaches 0 as z→∞.

**RH (OpenXR) → LH (+Z forward) conversion**, by mirroring Z:
```cpp
XMFLOAT3 pos = { p.position.x, p.position.y, -p.position.z };
XMFLOAT4 rot = { -p.orientation.x, -p.orientation.y, p.orientation.z, p.orientation.w };
```
If the game is Z-up or uses another axis order, apply its basis change after this step. Establish the axis order by RE before writing any pose code.

**Per-eye view matrix:**
`world_from_eye = world_from_playspace(game camera/player yaw + position, metres→game units) * playspace_from_eye(views[i].pose)`, and `view_i = inverse(world_from_eye)`.
Keep the player's horizontal root motion (thumbstick locomotion) in `world_from_playspace`. Apply real head motion from the HMD 1:1 at scale. Snap or smooth turning rotates `world_from_playspace` around the HMD's position, not the play-space origin.

**UEVR reference:** `OpenXR.cpp` computes `tan(angle*)` per eye and builds its engine matrix from them, and submits depth with `nearZ=FLT_MAX, farZ=near/world_to_meters`. Source: https://github.com/praydog/UEVR (`src/mods/vr/runtimes/OpenXR.cpp`, `src/mods/vr/D3D11Component.cpp`).

---

## 7. Helper libraries, reference implementations, and OpenVR vs OpenXR

| Resource | URL | Relevance | Takeaways |
|---|---|---|---|
| UEVR (praydog) | https://github.com/praydog/UEVR ; docs https://docs.uevr.io/usage/overview.html | **High (closest analogue)** | **x64 only**, so its code needs porting to x86 (hook library, calling conventions, 64-bit handle typedefs; see 0a.2). Injected DLL, D3D11 and D3D12, OpenVR and OpenXR behind a `VRRuntime` interface. Worth copying: hooking the game device, a double-wide sRGB swapchain (`B8G8R8A8_UNORM_SRGB`), frame-state pipelining (`synchronize_frame` → `xrWaitFrame` on the game thread; begin and end at submit with a frame-count queue, predicting an extra period when the game thread runs ahead), optional depth, and rendering modes (native stereo, synchronized sequential, AFR). It also tags the OpenXR application name with the exe name, and warns that `XR_ERROR_LIMIT_REACHED` on `xrCreateInstance` means another OpenXR instance or plugin is already in the process. |
| REFramework (praydog) | https://github.com/praydog/REFramework | Med | Same author and the same OpenXR/OpenVR runtime split (`src/mods/vr/runtimes/OpenXR.cpp`) for RE Engine D3D11/D3D12 games. A second data point for the architecture. |
| StereoKit | https://github.com/StereoKit/StereoKit ; https://stereokit.net/ | Low-Med | A lightweight C/C# OpenXR engine (v0.3.11). Clean reference for action setup, hand tracking, quad/UI layers and D3D11 on Windows. Use it for reading, not linking. |
| Monado | https://monado.dev/ ; https://gitlab.freedesktop.org/monado/monado | Low | Open-source OpenXR runtime (Linux, Windows, Android). Read its compositor to see how a runtime consumes D3D11 swapchains. It is not our target runtime. |
| OpenVR SDK | https://github.com/ValveSoftware/openvr ; docs https://github.com/ValveSoftware/openvr/wiki/API-Documentation | Med | The alternative API (see the table below). |
| Flat2VR-adjacent OpenXR D3D11 wrappers | https://github.com/mbucchia/dr2vrfix-openxr | Low | A small example of a `d3d11.dll` proxy combined with OpenXR. |

**OpenVR vs OpenXR for this mod**

| Criterion | OpenXR | OpenVR |
|---|---|---|
| Runtime reach | Any OpenXR runtime: SteamVR, Meta, VD, WMR/Oasis, Pimax | Only SteamVR (or through OpenComposite on OpenXR) |
| **32-bit (x86) support** | SteamVR only since **2.17 (Sept 2026)**, brand new. Meta and WMR runtimes had it earlier; Varjo and PimaxXR did not (2022) | **Yes, long-standing** (`bin/win32/openvr_api.dll`; maintainer confirmed in openvr #1687) |
| Frame submission | Runtime-owned swapchains, so a copy is required. Explicit wait/begin/end timing | `IVRCompositor::Submit(texture)` with any D3D11 texture. `WaitGetPoses` for timing. Simpler to bolt on |
| Input | Standard action system. SteamVR binding UI unreliable for OpenXR apps | Mature SteamVR Input: action manifest, binding UI, community bindings, skeletal input with `EVRSkeletalTrackingLevel` |
| Future | Khronos standard; Valve's stated direction | Legacy but still maintained (repo active in 2026) |
| Existing mods | UEVR and REFramework support both; UEVR: "OpenVR usually has the highest compatibility, OpenXR usually gives higher performance when it works"; VD users should pick OpenXR | Many older flat2vr mods are OpenVR only |

**Recommendation:** OpenXR as the primary runtime, behind a thin `IVrRuntime` interface (poses, per-eye FOV, submit, input and haptics), so an OpenVR backend can be added if SteamVR OpenXR input bindings become a blocker.

---

## 8. Recommended minimal OpenXR integration flow for an injected D3D11 mod

**Init.** Do this off `DllMain` because of loader lock: for example, a worker thread that waits until the hooked device exists, or lazy init on the first Present.
1. **Build the mod as x86.** Statically link the Win32 loader (preferred), or load the **Win32** `openxr_loader.dll` from the mod's folder by full path with `LoadLibraryEx`. Do not rely on the game's DLL search path. Before creating the instance, check that `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` exists. If it doesn't, tell the user to update to SteamVR 2.17+ and set it as the OpenXR runtime, or fall back to the OpenVR backend.
2. `xrEnumerateInstanceExtensionProperties` → enable `XR_KHR_D3D11_enable` (required). Enable `XR_EXT_debug_utils`, `XR_KHR_composition_layer_depth`, `XR_KHR_visibility_mask`, `XR_EXT_hand_tracking`, `XR_EXT_hand_joints_motion_range`, `XR_VALVE_analog_threshold`, `XR_EXT_dpad_binding` and `XR_KHR_win32_convert_performance_counter_time` only if they are present.
3. `xrCreateInstance`: request `XR_API_VERSION_1_1` first. On `XR_ERROR_API_VERSION_UNSUPPORTED`, retry with `XR_API_VERSION_1_0`. Put a clear `applicationName` such as "RFGR-VR".
4. `xrGetSystem(XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)`. If you get `XR_ERROR_FORM_FACTOR_UNAVAILABLE`, the headset is off or SteamVR is not running; retry later.
5. `xrGetD3D11GraphicsRequirementsKHR` (through `xrGetInstanceProcAddr`) and check the game device: `IDXGIDevice→GetAdapter→GetDesc().AdapterLuid == adapterLuid` and `device->GetFeatureLevel() >= minFeatureLevel`.
6. `xrCreateSession` with `XrGraphicsBindingD3D11KHR{device = game device}`.
7. `xrEnumerateViewConfigurationViews(PRIMARY_STEREO)` → `recommendedImageRectWidth/Height`. `xrEnumerateSwapchainFormats` → choose `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`, or the SRGB variant matching the game backbuffer family (BGRA vs RGBA).
8. Create the colour swapchain(s): `usage = COLOR_ATTACHMENT | SAMPLED` (+`TRANSFER_DST` for clarity), `sampleCount=1`, `faceCount=1`, `arraySize=1`, `mipCount=1`. Then `xrEnumerateSwapchainImages` into `XrSwapchainImageD3D11KHR[]`. Create RTVs with the explicit concrete format because the images are typeless. Optionally create a depth swapchain and a HUD quad swapchain.
9. Create reference spaces: STAGE for standing (or LOCAL / `XR_EXT_local_floor` for seated) and VIEW (for head-locked HUD).
10. Create action sets and actions, suggest bindings (Index + fallbacks), `xrAttachSessionActionSets` once, and create action spaces for the grip and aim poses.

**Per frame** (one possible design, following UEVR):
```
[any thread, every frame]  while xrPollEvent: handle
     SESSION_STATE_CHANGED: READY->xrBeginSession(PRIMARY_STEREO); STOPPING->xrEndSession; EXITING/LOSS_PENDING->teardown
     REFERENCE_SPACE_CHANGE_PENDING (recenter), INTERACTION_PROFILE_CHANGED (xrGetCurrentInteractionProfile)
[game thread, start of frame, before camera update]
     xrWaitFrame -> frameState (predictedDisplayTime, predictedDisplayPeriod, shouldRender)
     xrLocateViews(STAGE, predictedDisplayTime) -> views[2] (pose, fov)  (store with this frame's index)
     xrSyncActions (if FOCUSED); locate hand spaces at predictedDisplayTime; feed camera/aim/weapon
     override game camera: view_i/proj_i from views[i] (sec. 6)
[render thread: game renders eye 0 and eye 1 (twice per frame, or AFR) into our RTs]
[render thread, in IDXGISwapChain::Present hook]
     xrBeginFrame
     if shouldRender:
        xrAcquireSwapchainImage -> xrWaitSwapchainImage(XR_INFINITE_DURATION)
        CopySubresourceRegion(eyeRT[i] -> swapchainTex[idx] at x offset i*w)   (or blit shader)
        xrReleaseSwapchainImage
        (HUD) acquire/wait/copy UI RT/release on quad swapchain
     xrEndFrame{displayTime = SAME predictedDisplayTime, blend OPAQUE,
                layers = [projection{space=STAGE, views[i].pose/fov exactly as rendered, subImage rects}, quad HUD]}
        (if !shouldRender: xrEndFrame with 0 layers)
     call original Present (consider SyncInterval=0 so desktop vsync doesn't double-throttle against xrWaitFrame)
```

Rules to keep:
- Exactly one `xrBeginFrame` for every `xrWaitFrame`.
- Never call `xrWaitFrame` concurrently with itself.
- All swapchain calls and `xrEndFrame` go on the immediate-context thread (inference, section 2a).
- Save and restore D3D11 state around your work.
- If `xrBeginFrame` returns `XR_FRAME_DISCARDED`, still call `xrEndFrame`.
- `xrSyncActions` returns `XR_SESSION_NOT_FOCUSED` when the dashboard is up; handle it quietly.

**Stereo strategies** (decided in the engine research, listed here for completeness): (a) render the scene twice per game frame (synchronized sequential, correct but costs 2x GPU); (b) AFR, one eye per frame at double frame rate (cheap but can cause eye-desync artefacts); (c) native stereo, if the engine can do instanced or two-view rendering (unlikely for RFG). UEVR offers all three.

---

## 9. Gotchas checklist

1. **Loader lock:** never create an OpenXR instance in `DllMain`.
2. **One instance per process:** `XR_ERROR_LIMIT_REACHED` means something else in the process already made one (UEVR hit this with engine OpenXR plugins).
3. **API version:** a runtime that only supports 1.0 rejects `apiVersion=1.1`. Fall back to 1.0.
4. **Graphics requirements call is mandatory** before `xrCreateSession`, and the game device must be on `adapterLuid`. Laptops with hybrid graphics are a risk.
5. **Typeless swapchain textures:** create views with explicit formats. Copies only work within the same typeless family. BGRA↔RGBA needs a shader blit.
6. **sRGB double or missing gamma:** gamma-encoded data needs an `_SRGB` swapchain (raw copy). If the shader writes gamma values into an `_SRGB` RTV, colours are double-encoded (washed out). SteamVR has a history of colour bugs (openvr #1766). **Test with a grey ramp in the headset.**
7. **Alpha in HUD quads:** set `BLEND_TEXTURE_SOURCE_ALPHA_BIT` (and `UNPREMULTIPLIED_ALPHA_BIT` if the UI is not premultiplied). Otherwise the quad is opaque.
8. **Projection layer pose/FOV mismatch** with what was rendered causes swimming and judder. Store the views per frame index and don't re-query at submit.
9. **Asymmetric FOV:** never build a symmetric projection from the average FOV. Use all four tangents per eye.
10. **Handedness:** negate Z and fix up the quaternion as shown; after that, handle RFG's own axis convention.
11. **Reversed-Z depth submission:** `nearZ > farZ` (`nearZ = inf` or `FLT_MAX`). The depth swapchain must have `faceCount = 1`. On SteamVR the benefit is uncertain, so keep it optional.
12. **Session state machine:** nothing renders until READY → `xrBeginSession`. Input only works while FOCUSED. Losing focus (dashboard) must not crash or freeze the game.
13. **`xrAttachSessionActionSets` is once-only:** define every action set up front, including menu and vehicle sets. Switch between them with the active sets passed to `xrSyncActions`.
14. **Index input:** there is no squeeze click (use `squeeze/value` with `XR_VALVE_analog_threshold` or your own hysteresis) and no menu button (use B, a long-press, or a thumbstick click). Players may use other controllers, so always provide fallback profiles.
15. **SteamVR binding UI is unreliable for OpenXR:** ship an in-mod remap option.
16. **Hand tracking on Index is synthesised:** `isActive` can be false for the first frames, and it is easy to create both trackers with the same hand enum by mistake.
17. **Implicit API layers** (OpenXR Toolkit is now unsupported, plus others) can break or alter things. Check `HKLM`/`HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` when debugging user reports.
18. **Frame timing on SteamVR:** blocking has moved between `xrWaitFrame`, `xrBeginFrame` and `xrEndFrame` across versions. Instrument all three. Disable the game's own vsync or frame limiter so it does not fight `xrWaitFrame`.
19. **Resolution:** SteamVR's recommended size comes from its supersampling setting and is often large. Let users scale it, and round to multiples of 4.
20. **Refresh rate on Index** is read-only through `XR_FB_display_refresh_rate` and is set in SteamVR. Budget for 90 Hz, or 80 Hz with reprojection.
21. **RenderDoc:** capture the game's Present frame. The compositor side is invisible.
22. **D3D11 immediate context is not thread-safe:** serialise XR swapchain calls with the game's rendering (inference; this matches UEVR's mutex design).
23. **x86 target:** handles are all `uint64_t` (no type safety). `XRAPI_CALL` is `__stdcall`, so always use the `PFN_xr*` typedefs. A 32-bit runtime is needed (SteamVR 2.17+, WOW6432Node key). Watch address space (section 0a.4).
24. **x86 VR resources:** allocate once and early, `USAGE_DEFAULT`, no CPU access, no MSAA on big targets. Mappable/staging textures reserve process VA.

---

## 10. Source list (all verified 2026-09-25)

Khronos: https://registry.khronos.org/OpenXR/ · https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html · https://registry.khronos.org/OpenXR/specs/1.1/man/html/openxr.html · https://registry.khronos.org/OpenXR/specs/1.1/loader.html · https://registry.khronos.org/OpenXR/conformance/cts_usage.pdf · https://www.khronos.org/conformance/adopters/conformant-products/openxr · https://github.com/KhronosGroup/OpenXR-SDK · https://github.com/KhronosGroup/OpenXR-SDK/releases · https://github.com/KhronosGroup/OpenXR-SDK-Source · https://github.com/KhronosGroup/OpenXR-Docs · https://github.com/KhronosGroup/OpenXR-Tutorials · https://openxr-tutorial.com/ · https://github.com/KhronosGroup/OpenXR-Guide · https://github.com/KhronosGroup/OpenXR-CTS · https://github.com/KhronosGroup/OpenXR-Inventory · https://github.com/KhronosGroup/OpenXR-Hpp · https://www.khronos.org/developers/linkto/interaction-in-openxr-with-notes · https://www.khronos.org/news/press/khronos-releases-openxr-1.1-to-further-streamline-cross-platform-xr-development

Man pages used: XR_KHR_D3D11_enable, XrGraphicsBindingD3D11KHR, XrGraphicsRequirementsD3D11KHR, XrSwapchainImageD3D11KHR, xrEnumerateSwapchainFormats, xrCreateSwapchain, XR_KHR_composition_layer_depth, XrCompositionLayerDepthInfoKHR, XrCompositionLayerQuad, XR_KHR_visibility_mask, XR_VALVE_analog_threshold, XrHapticVibration, xrSuggestInteractionProfileBindings, XR_EXT_hand_tracking, XR_EXT_hand_joints_motion_range, XR_EXT_debug_utils, XR_KHR_D3D12_enable, XR_EXT_local_floor, xrLocateViews, XrFovf, xrWaitFrame, XR_KHR_win32_convert_performance_counter_time (all under https://registry.khronos.org/OpenXR/specs/1.1/man/html/).

Valve/SteamVR: https://steamcommunity.com/app/250820/discussions/8/ · https://steamcommunity.com/app/250820/discussions/8/3121550424355682585/ · https://steamcommunity.com/app/250820/discussions/3/4034725980849397816/ · https://steamcommunity.com/app/250820/discussions/8/3001046778348674981/ · https://steamcommunity.com/app/250820/discussions/8/3845556684657690924/ · https://steamcommunity.com/app/250820/discussions/8/2448217320134903029/ · https://steamcommunity.com/app/250820/discussions/3/5089647632457888477/ · https://steamcommunity.com/app/250820/discussions/8/3423312514218854296/ · https://steamcommunity.com/app/250820/discussions/8/3193619419608654311/ · https://steamcommunity.com/app/250820/discussions/8/2448217320142984311/ · https://github.com/ValveSoftware/openvr · https://github.com/ValveSoftware/openvr/wiki/API-Documentation · https://github.com/ValveSoftware/openvr/issues/1766 · https://github.com/ValveSoftware/openvr/issues/1827

Tools/layers: https://github.com/maluoi/openxr-explorer · https://github.com/mbucchia/OpenXR-Layer-Template · https://github.com/mbucchia/OpenXR-Toolkit · https://github.com/mbucchia/OpenXR-Toolkit/releases · https://forums.mudspike.com/t/the-end-of-the-openxr-toolkit/16520 · https://github.com/mbucchia/_ARCHIVE_XR_APILAYER_NOVENDOR_fov_modifier · https://github.com/mbucchia/_ARCHIVE_XR_APILAYER_NOVENDOR_nis_scaler · https://github.com/mbucchia/Quad-Views-Foveated · https://github.com/mbucchia/OpenXR-Vk-D3D12 · https://github.com/mbucchia/dr2vrfix-openxr · https://github.com/Rectus/openxr-steamvr-passthrough · https://github.com/baldurk/renderdoc/issues/2921 · https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/api_layers/README_core_validation.md · https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/api_layers/README_api_dump.md

32-bit: https://www.gamingonlinux.com/2026/09/steamvr-2-17-arrives-ready-to-go-for-the-steam-frame/ · https://store.steampowered.com/news/app/250820/view/679635225181946198 · https://vr.org/articles/steamvr-2-17-stable-32-bit-openxr-runtime-2026 · https://community.khronos.org/t/openxr-loader-dll-doesnt-use-steamvr-edit-because-32-bit-x86-support-isnt-implemented/108980 · https://github.com/ValveSoftware/openvr/issues/1687 · https://github.com/ValveSoftware/Proton/issues/6808 · https://github.com/KhronosGroup/OpenXR-SDK-Source/releases · https://vcpkg.io/en/package/openxr-loader.html · https://learn.microsoft.com/en-us/archive/blogs/tmulcahy/windows-and-video-memory · https://forums.developer.nvidia.com/t/directx11-excessive-virtual-memory-usage-with-createtexture2d/154505 · https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource · https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-createsharedhandle · https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresourcebyname · https://openkneeboard.com/internals/injectables/

Reference implementations / other: https://github.com/praydog/UEVR · https://docs.uevr.io/usage/overview.html · https://github.com/praydog/REFramework · https://github.com/StereoKit/StereoKit · https://stereokit.net/ · https://monado.dev/ · https://gitlab.freedesktop.org/monado/monado · https://gitlab.com/znixian/OpenOVR · https://learn.microsoft.com/en-us/windows/mixed-reality/develop/native/openxr-best-practices · https://github.com/microsoft/OpenXR-MixedReality/tree/master/samples/BasicXrApp · https://developer.nvidia.com/content/depth-precision-visualized · https://github.com/KhronosGroup/OpenXR-SDK-Source/issues/467 · https://github.com/DisplayXR/displayxr-unity/issues/326 · https://community.khronos.org/t/how-is-a-limited-finger-tracking-controller-like-index-controller-supposed-to-work/108265 · https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.15//manual/features/valveindexcontrollerprofile.html
