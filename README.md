<h1 align=center>torchlight-nx</h1>

<p align=center><b>An unofficial, fan-made Nintendo Switch port of the Xbox Live Arcade version of Torchlight (2011).</b></p>

> [!IMPORTANT]
> torchlight-nx is a non-commercial fan project. It is not affiliated with, endorsed by or supported by the owners
> of Torchlight, Runic Games, Microsoft or Nintendo. It contains no game files: it runs only with your own copy of
> the game. Please don't ask the game's publishers for help with it.

It's not an emulator. The PowerPC code of the game's `default.xex` is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and compiled for the Switch, and the game is drawn by a native
Vulkan renderer running on NVK (Mesa). It runs at the console's **stock clocks**, with no overclock needed: around
30-35 FPS in town and 40-47 FPS in the mines.

> [!NOTE]
> You need your own copy of Torchlight for Xbox Live Arcade: the game package from your Xbox 360.

## What you need

- A Nintendo Switch running custom firmware ([Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)).
- The Torchlight package from your Xbox 360 hard drive: the file
  `35F62CE3FE43845B1625A000921D5A6C48CCC7C058` (197 MB), found in `Content/0000000000000000/58410A7E/000D0000/`.
  Copy it to your computer the way you prefer (for example by FTP from a modded console, or with a USB drive and an
  Xbox 360 content tool). Only the full version of the game is supported.
- About 400 MB free on the SD card during the installation (200 MB once the package is deleted).

## How to install

1. Extract `torchlight-nx-<version>.zip` to the root of your SD card. You get `sdmc:/switch/torchlight/torchlight.nro`.
2. Copy your package file into the same folder, `sdmc:/switch/torchlight/`. Its name does not matter.
3. Launch Torchlight with full memory, in one of these two ways:
   - **Recommended: a forwarder.** Create a Home menu icon for `torchlight.nro` (for example with
     [Sphaira](https://github.com/ITotalJustice/sphaira)) and start it from there. Torchlight then runs as its own
     application, with its own kernel resources.
   - **Title override:** hold **R** while starting any installed game. This works too, but Torchlight then uses the
     kernel resources of that game, and some games reserve none, so they are taken from the pool the system itself
     uses.

   Launching from the Album (applet mode) does not provide enough memory; the game tells you so and quits.

On the first launch, the game checks your package and installs its files in `sdmc:/switch/torchlight/` (about
30 seconds). Every file is verified against the checksums of the original game. If the installation is interrupted,
just launch the game again: it resumes. At the end, you can delete the package to free 197 MB.

Your folder then looks like this:

```text
/switch/torchlight/
  torchlight.nro
  default.xex, pak.zip, ...   (the game files, installed from your package)
  installation_paquet.txt     (written once the installation is complete)
  torchlight/                 (your saves, achievements and the shader cache)
  logs/                       (log files)
```

> [!NOTE]
> The game's shaders are compiled by the console itself the first time they are needed, then kept in a cache. Expect
> some stutter during the first minutes of play; it goes away as the cache fills up.

**Updating:** replace `torchlight.nro` with the new one. Your game files and saves stay where they are.

## Controls

The buttons follow the letters of the Xbox 360 controller (A is A, B is B, ...), not their positions: on the Switch,
A and B, and X and Y, are swapped compared to the Xbox 360. The in-game prompts still show the Xbox 360 icons.

## Language

The game uses the language of your console when it has it: French (including Canadian French), German or Spanish
(including Latin American Spanish). Any other console language gives English. To choose the language yourself, add
`user_language = N` to `torchlight.toml` (see below): 1 English, 3 German, 4 French, 5 Spanish.

## Achievements and main menu

- **Achievements** unlock as on the Xbox 360 and are saved on the SD card (`torchlight/achievements/`). The
  **Achievements** entry of the main menu lists them, unlocked or not: scroll with the D-pad or the left stick,
  close with **B**. They are not linked to any online service.
- **Main menu background:** it shows the area of the character you last played, right from the start of the game:
  play a character once, and the next launches open on its area. `torchlight_menu_fond = false` in
  `torchlight.toml` turns this off (the menu then starts in town, until you return to it from a game).

## Settings

An optional `torchlight.toml` placed next to `torchlight.nro` overrides the built-in settings. The defaults are the
ones in [`switch/torchlight.toml`](switch/torchlight.toml), which explains each of them. You don't need it to play.

The port targets the stock clocks. Overclocking (for example with sys-clk) is your choice and is not required.

## Known issues

- The button icons are the Xbox 360 ones.
- Shader compilation stutter during the first minutes (see above).
- Docked (TV) mode has only been tested by forcing it on a Switch Lite (with Reverse-NX-RT), not on a real dock:
  docking or undocking during play has not been tested yet.

## Reporting a problem

Please give the version of the port and, after a crash, the files of `sdmc:/switch/torchlight/logs/` (in particular
`logs/rex/rex_crash.log`).

## Building from source

The Switch build uses the SDK and tools of the [NFSMW-NX](https://github.com/StevensND/nfsmw-nx) port, with the
changes in [`switch/patches/`](switch/patches):

- **NFSMW-NX** at commit `66611e3`, with `nfsmw-sdk.patch` applied to `sdk/` and `glslang-verrou.patch` applied to
  `sdk/thirdparty/glslang`. Fetch the SDK's third-party libraries with its `tools/fetch_thirdparty.py`.
- **[mesa-switch](https://github.com/danfromtico/mesa-switch)** at `1a8c1a66d6f` with NFSMW-NX's Mesa patch and
  `mesa-switch-build.patch` / `mesa-switch-zcull-toggle.patch` / `mesa-switch-gpfifo-reserve.patch`, built with its `build-switch.sh` (Docker), then
  `switch/assemble_nvk_sdk.sh`.
- **Docker** with the `devkitpro/devkita64` image, and clang 20, CMake and Ninja on the host (for the code
  generator).
- **Your own `default.xex`**: the recompiled game code is generated on your machine and never distributed.

Then, with the game files in `assets/`:

```sh
switch/gen_manifest.sh                       # Switch manifest from torchlight_manifest.toml
cd switch && ~/workspace/nfsmw-nx/sdk/out/linux-amd64/rexglue codegen torchlight_manifest.toml && cd ..
JOBS=4 switch/build_switch.sh                # switch/out/torchlight.nro
```

The repository expects NFSMW-NX and mesa-switch next to it (`~/workspace/nfsmw-nx`, `~/workspace/mesa-switch`).
The release build (`switch/build_joueurs.sh`: LTO, profile-guided optimization, no developer tools) also needs PGO
profiles from a play session (`pgo/`, see `CMakeLists.txt`); without them, use `build_switch.sh` with
`CMAKE_OPTIONS="-DTORCHLIGHT_LTO=ON -DTORCHLIGHT_OUTILS=OFF"`.

A PC (Linux) build with the stock ReXGlue SDK 0.10.0 is also available: see `run.sh`. The development notes
(`CLAUDE.md`, `switch/RENDU_NATIF.md`) are in French.

## Acknowledgements

This port would not exist without the work of these projects and their developers. Thank you!

**The foundations of the port**

- [NFSMW-NX](https://github.com/StevensND/nfsmw-nx) by StevensND: the Need for Speed: Most Wanted Switch port this
  one is based on (Horizon SDK layer, toolchain, native renderer approach), and
  [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip before it.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay: the static recompiler and the Xbox 360 runtime.
- [Xenia](https://github.com/xenia-project/xenia) by Ben Vanik and the Xenia contributors: the Xbox 360 research
  and kernel implementation ReXGlue builds on.
- [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico and [Mesa](https://gitlab.freedesktop.org/mesa/mesa):
  the NVK Vulkan driver running on the Switch.
- [OGRE](https://github.com/OGRECave/ogre): the engine Torchlight runs on, whose rendering calls the native
  renderer replays.

**Switch homebrew**

- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere), [libnx](https://github.com/switchbrew/libnx) and
  [devkitPro](https://github.com/devkitPro): the custom firmware, the Switch library and the toolchain.

**Libraries**

- [glslang](https://github.com/KhronosGroup/glslang) and [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools):
  the game's HLSL shaders compiled on the console.
- [Dear ImGui](https://github.com/ocornut/imgui): the installation window and menus.
- [FFmpeg](https://github.com/FFmpeg/FFmpeg) and [libmspack](https://github.com/kyz/libmspack): media decoding and
  decompression in the runtime.
- [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) and
  [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers).
- [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog),
  [toml++](https://github.com/marzer/tomlplusplus), [xxHash](https://github.com/Cyan4973/xxHash),
  [SIMDe](https://github.com/simd-everywhere/simde), [o1heap](https://github.com/pavel-kirienko/o1heap),
  [utfcpp](https://github.com/nemtrif/utfcpp) and [CLI11](https://github.com/CLIUtils/CLI11).
- [LLVM](https://github.com/llvm/llvm-project): Clang compiles the code generator, and its tools helped analyse the
  game's PowerPC code.

## Legal

torchlight-nx is an unofficial, non-commercial fan project, with no affiliation with Runic Games, Microsoft,
Nintendo or the current owners of Torchlight. Torchlight is a trademark of its respective owner, used here only to
say which game this port works with. This repository contains no assets or program code from the original game. Users must provide their own legally obtained
copy, from which the game files are installed on their own console. Running homebrew requires custom firmware,
which violates Nintendo's terms of service and can get a console banned: your call.

Source code is provided under the GPL-3.0 License (see [LICENSE](LICENSE)), inherited from NFSMW-NX. The changes to
other projects in `switch/patches/` are under the licenses of those projects (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
