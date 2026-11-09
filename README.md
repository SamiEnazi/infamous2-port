# inFAMOUS 2 — native Windows port (static recompilation)

Play **your own copy** of inFAMOUS 2 (PS3) as a native Windows program. Setup translates the game's
PS3 code into PC code **on your machine**, compiles it, and the game then runs directly on your CPU,
with its graphics drawn through Vulkan. There is no emulator while you play.

> **Early public test.** It boots, plays and saves, and it is fast on the PC it was built on, but
> it has only been played from the start to about mission 4, on one PC. Expect bugs; please
> [report them](../../issues).

**No game content is included, and none is downloaded.** You need your own copy of the game,
plus RPCS3 to decrypt it. Builds of the game (the `.exe`) are never shared, because they contain
the game's code. Each player builds their own.

## Status

| | |
|---|---|
| Tested copy | Europe disc **BCES01143** + the **1.04** update. US/other discs share the same 1.04 code but are untested. |
| Tested PC | i5-13600K, RTX 2070 Super, 32 GB, Windows 11, 1080p |
| Speed there | City from a save: about 99–141 fps. Opening and tutorial fight: about 120–153 fps (uncapped, see note below). |
| Known issues | Short whole-game freezes (0.3–1 s) now and then. The Beast fight can drop into the 40s. There is **no save-slot screen yet**: a manual save always writes the same slot, and Load picks the save itself (`tools\play.ps1` backs your saves up before every session). |

The tested build also uses profile-guided optimisation (about 20% faster). Your Setup build is a normal
optimised build; a one-click training step for that is planned.

## What you need

- **Your copy of inFAMOUS 2**
  - The disc's files: the folder that holds `PS3_GAME`.
  - The **1.04 update** installed in RPCS3.
- **[RPCS3](https://rpcs3.net)** with the **PS3 firmware** installed (*File > Install Firmware*, using
  `PS3UPDAT.PUP` from playstation.com). Setup uses it to decrypt your game, and the port reads the
  firmware's fonts and system files.
  - Start the game once in RPCS3 until the main menu: it installs 2.4 GB of data that Setup copies.
- **A PC with:**
  - Windows 10/11 64-bit
  - a CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer)
  - a Vulkan graphics card
  - 16 GB RAM
  - about 30 GB free disk space
- **Build tools.** Setup checks for them and can install the missing ones with winget:
  - Git
  - Python 3.10+
  - Visual Studio 2022 Build Tools (C++)
  - LLVM (clang-cl)
  - CMake
  - Ninja

## Install

```
git clone --recursive https://github.com/SamiEnazi/infamous2-port
```

(The ZIP download does not include the two submodules; use `git clone --recursive`, or run
`git submodule update --init` afterwards.)

Then run **`Setup.cmd`**:

1. Pick your **game disc folder** and your **RPCS3 folder**.
2. **Install missing tools** if anything is red, then restart Setup.
3. **Build the game.** This takes 15–40 minutes, one time only.
4. **Play**, or use the new desktop shortcut. Its icon is taken from your own copy.

## Playing

- **Start:** `Play inFAMOUS 2.cmd`, or the desktop shortcut.
- **Keys:**

  | Key | Action |
  |---|---|
  | **F1** | fps counter |
  | **F2** (or hold L3+R3) | PC settings: frame cap, vsync, fullscreen, anti-aliasing, upscaling, sharpening, volume, controls |
  | **F9** | mark a stutter in the session log |
  | **Alt+Enter** | fullscreen |

- **Controllers:** any pad SDL knows (DualShock 4/5, Xbox, 8BitDo...). Buttons work by position, as on
  a PlayStation pad.
- **Keyboard and mouse:**
  - WASD to move, mouse for the camera
  - right button to aim, left button to shoot
  - Space = Cross, Shift = Circle, F = Square, Q = Triangle
  - To remap, edit `input.toml`.
- **Your saves:** `game\gamedata\dev_hdd0\home\00000001\savedata`, backed up to `saves-backup\` before every session.
- **Session logs:** each session writes `play\<date-time>\`. Please attach it to bug reports.

## How it works

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) translates the game's PowerPC (PPU) and SPU code, and
  three PS3 firmware modules taken from **your** RPCS3, into C/C++. clang-cl then compiles it.
- The toolkit's runtime does the rest:
  - stands in for the PS3 system: files, threads, audio, input, saves
  - draws the PS3 graphics chip's commands with Vulkan
  - runs some SPU jobs on the GPU instead (the deferred lighting and SSAO)
- This repo holds the Windows build, the setup, and the tools used to test it.
  - The toolkit fork lives in [`toolkit/`](https://github.com/SamiEnazi/ps3recomp/tree/infamous2).
  - The game-specific port base lives in [`community/erikomis-infamous2-recomp`](https://github.com/erikomis/infamous2-recomp).
  - Our changes to the port base are in `patches/port/`.

## Thanks and credits

This port stands on other people's work:

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** by Ned Heller / sp00nznet and its
  contributors, and **[Noah Peterson's fork](https://github.com/NoahBPeterson/ps3recomp)**: the
  recompiler and runtime this port is built on (MIT). Our fork with the Windows/Vulkan work is
  [SamiEnazi/ps3recomp](https://github.com/SamiEnazi/ps3recomp/tree/infamous2).
- **[erikomis/infamous2-recomp](https://github.com/erikomis/infamous2-recomp)** by Erik Martins: the
  inFAMOUS 2 port this one starts from (MIT).
- **[ThiagoFraga761/infamous2-pc](https://github.com/ThiagoFraga761/infamous2-pc)**: the Vulkan SSAO and
  lighting passes and Windows groundwork we adopted (MIT).
- **[RPCS3](https://github.com/RPCS3/rpcs3)** and its team. RPCS3 decrypts the game and firmware. It was
  also our reference while building the port: we compared our output against it, and its research and
  wiki taught us much of what we know about the PS3. **No RPCS3 code is included** here (RPCS3 is GPL).
- Third-party libraries used by the build, each under its own license:
  - [SDL2](https://libsdl.org) (zlib)
  - [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0)
  - [glslang](https://github.com/KhronosGroup/glslang) (BSD-3 and others)
  - [Dear ImGui](https://github.com/ocornut/imgui) (MIT)
  - [SMAA](https://github.com/iryoku/smaa) (MIT)
  - AMD FidelityFX FSR 1 / CAS (MIT)
  - NVIDIA FXAA 3.11
  - [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) (zlib)
  - at3_standalone (LGPL-2.1)

  Their license files are in `toolkit/third_party/`.
- Built by Sami Enazi ([@SamiEnazi](https://github.com/SamiEnazi)), with the help of Claude Code.

## Legal

This is a fan project for people who own the game. It is not affiliated with or endorsed by Sony
Interactive Entertainment or Sucker Punch Productions. *inFAMOUS* is a trademark of Sony Interactive
Entertainment LLC.

This repository contains no game code, assets, firmware or keys. Everything game-specific is read from
your own copy on your own PC. Do not share builds of the game, or files taken from it.

The code in this repository is MIT-licensed (see `LICENSE`). The toolkit and the port base keep
their own MIT licenses.
