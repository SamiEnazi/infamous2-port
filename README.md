<div align="center">

# ⚡ inFAMOUS 2: native Windows port

**Your own PS3 copy of inFAMOUS 2, recompiled into a real Windows program.**<br>
No emulator while you play.

[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)](#-what-you-need)
[![Renderer: Vulkan](https://img.shields.io/badge/renderer-Vulkan-AC162C?logo=vulkan&logoColor=white)](#-how-it-works)
[![2.1x the frame rate of RPCS3](https://img.shields.io/badge/vs%20RPCS3-2.1%C3%97%20the%20fps-2a78d6)](#-performance-native-vs-rpcs3)
[![Status: early public test](https://img.shields.io/badge/status-early%20public%20test-eb6834)](#-status)
[![License: MIT](https://img.shields.io/badge/license-MIT-1baf7a)](LICENSE)
[![Made with Claude Code](https://img.shields.io/badge/made%20with-Claude%20Code-D97757?logo=claude&logoColor=white)](#-made-with-claude-code)

</div>

## 🚀 Performance: native vs RPCS3

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/perf-dark.svg">
  <img alt="Frames per second at the same spot in the city: RPCS3 0.0.42 42.9 fps, this port 91 fps" src="docs/perf-light.svg" width="760">
</picture>

![The same spot from the same save: this port at 91 fps (left), RPCS3 at 43 fps (right)](docs/same-spot.jpg)

| Same PC, same save, same spot | RPCS3 0.0.42 | **This port** |
|---|:-:|:-:|
| 🏙️ City: the dock right after loading | 43 fps (39–46) | **91 fps** |
| 🎥 Camera turned to three more views | not measured | **95 / 119 / 127 fps** |
| 🔧 Developer build with profile-guided optimisation, dock view | | 99 fps |

<sub>
Measured on 9 Nov 2026 on an i5-13600K, RTX 2070 Super and 32 GB RAM, Windows 11, at the game's 720p.
The PC was a normal desktop with a browser open in the background. RPCS3 ran with its default settings:
LLVM recompilers, Vulkan, 100% resolution. Story videos and cutscenes run at 30 fps in both, as on the PS3.
The developer build's 99 fps was measured on a clean desktop the same morning.
</sub>

### 🎮 In the city on this port

![Four camera views in the city on this port: 91, 95, 119 and 127 fps](docs/city-views.jpg)

## 🧭 What this is

Setup translates the game's PS3 code into PC code **on your machine** and compiles it. The game then
runs directly on your CPU, with its graphics drawn through Vulkan.

> 🧪 **Early public test.** It boots, plays and saves, and it is fast on the PC it was built on, but
> it has only been played from the start to about mission 4, on one PC. Expect bugs; please
> [report them](../../issues).

> 🔒 **No game content is included, and none is downloaded.** You need your own copy of the game,
> and RPCS3 for Setup. Builds of the game (the `.exe`) are never shared, because they contain the
> game's code. Each player builds their own.

## 📋 Status

| | |
|---|---|
| ✅ Tested copy | Europe disc **BCES01143** + the **1.04** update. US and other discs share the same 1.04 code but are untested. |
| 🖥️ Tested PC | i5-13600K, RTX 2070 Super, 32 GB, Windows 11, 1080p |
| ⚠️ Known issues | • Short whole-game freezes (0.3–1 s) now and then.<br>• The Beast fight can drop into the 40s.<br>• **No save-slot screen yet:** a manual save always writes the same slot, and Load picks the save itself. `tools\play.ps1` backs your saves up before every session. |

## 🧰 What you need

- 💿 **Your copy of inFAMOUS 2**
  - The disc's files: the folder that holds `PS3_GAME`.
  - The **1.04 update**, installed in RPCS3.
- 🕹️ **[RPCS3](https://rpcs3.net)** with the **PS3 firmware** installed (*File > Install Firmware*, using
  `PS3UPDAT.PUP` from playstation.com).
  - It is needed **only during Setup**. Setup uses it to decrypt your game, then copies into the port's
    folder the update, the firmware files the game reads (fonts, system data) and the game's HDD install.
  - After Setup you can remove RPCS3. Keep your game disc folder where it is.
  - Start the game once in RPCS3 until the main menu: that installs 2.4 GB of data that Setup copies.
- 💻 **A PC with:**
  - Windows 10/11 64-bit
  - a CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer)
  - a Vulkan graphics card
  - 16 GB RAM
  - about 30 GB free disk space
- 🛠️ **Build tools.** Setup checks for them and can install the missing ones with winget:
  - Git
  - Python 3.10+
  - Visual Studio 2022 Build Tools (C++)
  - LLVM (clang-cl)
  - CMake
  - Ninja

## 📦 Install

```
git clone --recursive https://github.com/SamiEnazi/infamous2-port
```

(The ZIP download does not include the two submodules: use `git clone --recursive`, or run
`git submodule update --init` afterwards.)

Then run **`Setup.cmd`**:

1. 📁 Pick your **game disc folder** and your **RPCS3 folder**.
2. 🧩 **Install missing tools** if anything is red, then restart Setup.
3. 🏗️ **Build the game.** It takes 10–40 minutes, one time only (about 10 minutes on the test PC).
4. ▶️ **Play**, or use the new desktop shortcut. Its icon is taken from your own copy.

## 🎮 Playing

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

## 🔬 How it works

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

## 🤖 Made with Claude Code

This port was built in **2 days and 9 hours**, in one long conversation between **Sami** and
**Claude** (Anthropic's Claude Opus 5.5, working in [Claude Code](https://claude.com/claude-code)).
That's from the first message (6 Nov 2026, 22:42) to the first public release (9 Nov 2026, 07:15).

| | |
|---|---|
| 🧑 **Sami** | The idea, his own copy of the game, the direction, the playtesting and the feedback. |
| 🤖 **Claude** | The Windows build, the performance work on the main CPU code, the SPU jobs and the graphics thread, the test and profiling tools, Setup, and this README. The lead worked with up to four sub-agents in parallel. |
| ⏱️ **Time** | 2 days 9 hours, from the first message to public |
| 💬 **Messages** | About 190 from Sami, and 9,560 model responses from the lead plus 32 sub-agents |
| 🔤 **Tokens** | 7.8 million generated, 3.55 billion processed in total (3.51 billion of those were re-reads of the cached project context) |

## 🙏 Thanks and credits

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

## ⚖️ Legal

This is a fan project for people who own the game. It is not affiliated with or endorsed by Sony
Interactive Entertainment or Sucker Punch Productions. *inFAMOUS* is a trademark of Sony Interactive
Entertainment LLC. The screenshots show the game running on this port and in RPCS3, for comparison.

This repository contains no game code, assets, firmware or keys. Everything game-specific is read from
your own copy on your own PC. Do not share builds of the game, or files taken from it.

The code in this repository is MIT-licensed (see `LICENSE`). The toolkit and the port base keep
their own MIT licenses.
