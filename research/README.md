# Research

| File | Contents |
|---|---|
| [00-local-findings.md](00-local-findings.md) | **Engineering log**: the game's exe, the VR runtime, and the reverse engineering behind every engine hook of the mod, including dead ends. |
| [01-rfgr-modding-resources.md](01-rfgr-modding-resources.md) | Background: RFG:R modding projects (Sledge, RSL, Reconstructor, RFGRHook), file formats, community address tables, camera/first-person/stereo mods. |
| [02-openxr-resources.md](02-openxr-resources.md) | Background: OpenXR spec, SDK, D3D11 binding, Index input, SteamVR specifics and pitfalls. |
| [03-flat2vr-techniques-and-resources.md](03-flat2vr-techniques-and-resources.md) | Background: flat-to-VR techniques and reference mods, third- to first-person conversion. |
| [04-toolchain-and-reference-code.md](04-toolchain-and-reference-code.md) | Background: reverse-engineering tools, hooking libraries, reference code bases. |

Documents 01-04 were compiled on 2026-09-25, before development started. They assume a 64-bit game in places (`rfg.exe` is 32-bit) and their plans and recommendations predate the mod; `00-local-findings.md` and the code are authoritative. Open work is tracked in the GitHub issues.

## Licensing of reference code

| Source | License | Use |
|---|---|---|
| REFramework, anvilengine2vr, KCD1VR, BioShock VR, vrframework, PreyVR, MGS5VR, RFGR-Shaders, OpenXRSamples | MIT | Reusable with attribution |
| Reconstructor | MPL-2.0 | File-level copyleft |
| RSL1, Special K, OpenComposite | GPL | Reference only |
| UEVR | All rights reserved | Ideas only |
| Sledge, RFGR_Types, RFGRHook | none | Reference only |

The mod itself contains no code from these projects; addresses and type layouts from Sledge, RSL and RFGR_Types were used as starting points and verified against the exe.
