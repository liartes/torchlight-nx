# Third-party notices

torchlight-nx (an unofficial Switch port of Torchlight) is built on the work of other projects. Their licenses apply to their code and to the parts of this
repository derived from it. The rest of this repository is under the GPL-3.0 (`LICENSE`).

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [NFSMW-NX](https://github.com/StevensND/nfsmw-nx) by StevensND (commit `66611e3`), itself derived from [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip | The Switch port this one follows: the modified SDK with its Horizon layer, the build and toolchain setup, the native renderer approach, and the methods behind `src/torchlight_crt.cpp`, `src/torchlight_pgo.cpp`, `src/torchlight_manette.cpp`, `src/torchlight_ogre_natif.cpp` and `switch/tools/` | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | Code generator, Xbox 360 kernel and runtime (linked into the NRO and the PC build) | BSD-3-Clause |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico, on top of [Mesa](https://mesa3d.org) | The Vulkan driver (NVK) and shader compiler (NAK) for Horizon, linked into the NRO | MIT (Mesa's licenses per file) |
| [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) by ChanseyIsTheBest | The method and tools behind `switch/tools/appels_indirects.py`, `switch/tools/appels_indirects_promus.py`, `switch/tools/constantes_lecture_seule.py`, `switch/tools/chargements_flottants.py`, `switch/tools/registres_locaux_feuilles.py`, `switch/tools/registres_locaux_boucles.py` and `src/torchlight_appels_indirects.cpp` (indirect calls that usually go to one function turned into guarded direct calls; constants of the game's read-only data folded at build time; single-precision loads byte-swapped in the vector unit; registers as locals in leaf functions and in call-free loops) | GPL-3.0 |
| [torchlight-recomp](https://github.com/YaPeL/torchlight-recomp) by YaPeL | The analysis behind skipping more of the game's Xbox Direct3D work in `src/torchlight_trace.cpp` (the vertex and index buffer bindings, the polygon mode and point sprite render states), and the static geometry bucket culling of `src/torchlight_blocs.cpp` (method, layout and box building, from its `src/hooks/bucket_cull*`), and the RTSS per-object listener's lookup in `src/torchlight_ogre_natif.cpp` (from its `src/hooks/rtss_listener_hooks.cpp`) | GPL-3.0 |
| [OGRE](https://www.ogre3d.org) 1.7 (Torus Knot Software and the OGRE contributors) | The native renderer replays the calls of the game's Ogre render system; `src/torchlight_ogre_natif.cpp` rewrites a few Ogre functions (frustum, matrices, animation, automatic shader parameters) with the same results | MIT |

### Changes to these projects

The files in `switch/patches/` are changes to the projects above, to apply on top of the listed commits. Each patch
is under the license of the project it changes, so it can be reused there:

| Patch | Applies to | License |
|---|---|---|
| `nfsmw-sdk.patch` | `sdk/` of NFSMW-NX (the ReXGlue SDK with its Horizon layer): file reads, input, save data, virtual keyboard, presentation | BSD-3-Clause |
| `glslang-verrou.patch` | glslang (`sdk/thirdparty/glslang`) | BSD-3-Clause |
| `mesa-switch-build.patch`, `mesa-switch-zcull-toggle.patch`, `mesa-switch-gpfifo-reserve.patch` | mesa-switch, branch built from `1a8c1a66d6f` | MIT |

## Libraries

Libraries linked into the NRO, fetched from the submodules of ReXGlue SDK v0.10.0 (`tools/fetch_thirdparty.py` of
NFSMW-NX):

| Library | License |
|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil, used by the SDK), from `wmarti/FFmpeg` at `0604b464c7cb`, as configured by NFSMW-NX | LGPL-2.1-or-later (configured without GPL parts) |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression of the game executable) | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) (compiles the game's HLSL shaders on the console) | BSD-3-Clause and others (see its LICENSE.txt) |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| [libnx](https://github.com/switchbrew/libnx) and the devkitA64 runtime (newlib, libstdc++) | ISC; newlib and GCC runtime licenses |

### FFmpeg and libmspack (LGPL)

The NRO links FFmpeg and libmspack statically. They are used unmodified from the SDK tree of NFSMW-NX (its own
changes to FFmpeg are listed in its THIRD_PARTY_NOTICES.md). Everything needed to rebuild and relink the NRO with a
modified version of either library is described in `README.md` (Building) and is available from the projects above.

## Game data

This repository contains no assets or program code from Torchlight. The package checksums in
`src/torchlight_paquet_complet.inc` are SHA-256 hashes, used to verify the player's own copy; they contain no game
data. The game's shaders are compiled on the player's console from the sources the game itself generates.

## Trademarks

Torchlight is a trademark of its respective owner (the game was developed by Runic Games). Nintendo Switch is a
trademark of Nintendo. Xbox 360 and Xbox Live Arcade are trademarks of Microsoft. This project is not affiliated
with or endorsed by any of them.
