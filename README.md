<div align="center">

![inFAMOUS 2 native PC port: native 1080p up to 4K, 2x the fps of RPCS3, any controller, cheats menu, enhanced lighting, HD fire and smoke, save manager](docs/social-preview.png)

# ⚡ inFAMOUS 2: native PC port

**Your own PS3 copy of inFAMOUS 2, recompiled into a real PC program for Windows and Linux.**<br>
No emulator while you play.

[![Latest release](https://img.shields.io/github/v/release/SamiEnazi/infamous2-port?label=latest&color=f2b200)](../../releases/latest)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)](#-what-you-need)
[![Linux: experimental](https://img.shields.io/badge/Linux-experimental-FCC624?logo=linux&logoColor=black)](#-linux-experimental)
[![Renderer: Vulkan](https://img.shields.io/badge/renderer-Vulkan-AC162C?logo=vulkan&logoColor=white)](#-how-it-works)
[![Native 1080p, up to 4K](https://img.shields.io/badge/native-1080p%20%E2%80%A2%20up%20to%204K-6250d6)](#%EF%B8%8F-native-1080p-and-up)
[![2.3x the frame rate of RPCS3](https://img.shields.io/badge/vs%20RPCS3-2.3%C3%97%20the%20fps-2a78d6)](#-performance-native-vs-rpcs3)
[![Any controller](https://img.shields.io/badge/controllers-plug%20%26%20play-1baf7a?logo=steamdeck&logoColor=white)](#-full-controller-support)
[![Status: public test](https://img.shields.io/badge/status-public%20test-eb6834)](#-status)
[![License: MIT](https://img.shields.io/badge/license-MIT-1baf7a)](LICENSE)
[![Made with Claude Code](https://img.shields.io/badge/made%20with-Claude%20Code-D97757?logo=claude&logoColor=white)](#-made-with-claude-code)
[![Visitors](https://komarev.com/ghpvc/?username=SamiEnazi-infamous2-port&label=visitors&color=3fa9ff&style=flat)](https://github.com/SamiEnazi/infamous2-port)

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

</div>

> ▶️ **How to play:** you need your own PS3 copy of inFAMOUS 2 (the Europe or US disc with the 1.04 update,
> or the digital version 01.03) set up in [RPCS3](https://rpcs3.net) with the PS3 firmware. Download
> **Setup.exe** from the [latest release](../../releases/latest), run it and press **Install**: it builds the
> game from your copy once (20–60 minutes), then **Play**. Details: [What you need](#-what-you-need) ·
> [Install](#-install). On **Linux**: [Linux (experimental)](#-linux-experimental).

## 🆕 What's new

**v0.5.4**
- 🧊 **Far fewer freezes the first time you play:** new graphics shaders are now built in the background instead
  of stopping the game. In first-session tests, freezes over 50 ms dropped from 13–25 per scene to one (the first
  frame after loading). While a shader is being built, the last picture can stay on screen for a moment; the game,
  sound and controls keep running.

**v0.5.3**
- 🎥 **Field of view slider:** F2 → Display → Camera → *Field of view*, 70–130% of the game's. Aiming, the mouse
  and widescreen stay right, and cutscenes keep their framing.
- 🌫️ **Depth of field setting:** F2 → Graphics → Game effects → *Depth of field*: On (the PS3 look), Off in
  gameplay (cutscenes keep their focus effects), or Off everywhere.
- 🖥️ **Ultrawide:** at 21:9 and 32:9 the red "hurt" overlay now covers the whole screen, not just the middle.
- ⚡ **Faster again on 4–6 core CPUs:** the heaviest view at the cathedral spire runs about 14% faster on 4 cores
  than in v0.5.2, with smoother slow frames (27 → 20 ms).
- 🎮 **Steam Deck:** confirmed working by a player ([#24](../../issues/24)).

**v0.5.2**
- ⚡ **Faster on every PC:** native versions of the game's heaviest SPU jobs (+13–19% on 4-core CPUs, up to +26%
  on fast ones), six job workers on CPUs with 10 or more cores, and less wasted waiting on 4–6 core CPUs.
- 🚀 **Optimise for this PC:** Setup can train the build on your own PC for a few minutes, then build a faster game
  (+5–11% more). It's a checkbox in Setup, on by default; it adds 10–25 minutes to the first install.
- 🎮 **Steam, Playnite and other launchers:** add the game's own `Infamous2Recomp.exe`; Steam Input and the Steam
  overlay work ([#29](../../issues/29)). [More](#-playing)
- 🖱️ **Mouse:** "look up" in the opening tutorial works with the direct mouse camera ([#27](../../issues/27)).
- 📁 Setup accepts game folders with `[ ]` in their name ([#19](../../issues/19)), and the Turkey disc (BCES01229) is tested.

**v0.5.1**
- ⚡ **Black lightning works:** F2 → Cheats → *Black lightning* now turns Cole's bolts black. Before, it was on but
  changed nothing.
- 📁 **Renaming or moving the port's folder is fine:** the game finds your game folder again instead of showing a
  black screen after the intro logos ([#13](../../issues/13)).

**v0.5.0**
- 🔄 **Updates from inside the game:** a notice says when a new version is out, and **F2 → Update** installs it.
  Later updates rebuild only what changed, so they take minutes. [More](#-updates)
- 🐧 **Linux (experimental):** builds and runs natively, with `tools/setup.sh` and `tools/play.sh`. [More](#-linux-experimental)
- 🏆 **Trophies in Playnite and RecompLauncher:** the port writes its trophies the way RPCS3 does, and reports
  them to Recomp Achievements. [More](#-trophies-in-playnite-and-recomplauncher)
- ✨ **Glow at high resolutions fixed:** no more blocky or doubled glow around lightning and lamps above 720p
  ([#12](../../issues/12)).
- 🎥 **No more stutter at cutscene cuts and loading:** the graphics card's drawing programs are made in the
  background and prepared ahead at the title screen.
- 🗺️ **Minimap icons and world objects:** the map's icons show again, and objects no longer pop in and out
  as the camera turns ([#7](../../issues/7)).
- 🎮 **The right button pictures:** the game shows Xbox or keyboard prompts when you play with those
  ([#10](../../issues/10)).
- 💾 **The "save game error" of the digital (PSN) version is fixed.**
- 📦 **One repository:** the engine and the port base are folders of this repository now, so Setup downloads one
  thing instead of three.

Every version and its changes: [Releases](../../releases).

## 🚀 Performance: native vs RPCS3

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/perf-dark.svg">
  <img alt="Frames per second at the same spot in the city: RPCS3 0.0.42 42.9 fps, this port 99 fps" src="docs/perf-light.svg" width="760">
</picture>

![The same spot from the same save: this port at 99 fps (left), RPCS3 at 43 fps (right)](docs/same-spot.jpg)

| Same PC, same save, same spot | RPCS3 0.0.42 | **This port** |
|---|:-:|:-:|
| 🏙️ City: the dock right after loading (720p) | 43 fps (39–46) | **99 fps** |
| 🎥 Camera turned to three more views (720p) | not measured | **104 / 132 / 139 fps** |
| 🖥️ The same four views at **native 1080p** with every picture extra on | | **99 / 103 / 133 / 140 fps** |
| 🔧 Developer build (profile-guided optimisation and hand-written SPU code, not in the Setup build), the same four views at 1080p with every extra | | 116 / 126 / 161 / 166 fps |

<sub>
This port: measured on 10 Nov 2026 with the build Setup makes today. RPCS3: measured on 9 Nov 2026. Both on an
i5-13600K, RTX 2070 Super and 32 GB RAM, Windows 11. The PC was a normal desktop with a browser open in the
background. RPCS3 ran with its default settings: LLVM recompilers, Vulkan, 100% resolution (720p). Here the CPU
sets the pace, so native 1080p with every extra runs as fast as 720p. Story videos and cutscenes run at 30 fps in
both, as on the PS3. "Picture extras" means soft shadows, full-resolution ambient occlusion, smooth light
gradients and full-resolution effects.
</sub>

**v0.5.2 is faster again.** The build Setup makes now has native versions of the game's heaviest SPU jobs. In the same kind of player build on the test PC: **+14% to +26%** across the city, the dock, the opening fight and the cathedral spire, and **+13% to +19%** with the game held to 4 cores (in that fight, frames slower than 60 fps went from 770 to 121). Setup's *Optimise for this PC* adds about **5–11%** more. The table above is from the 10 Nov build; it will be measured again.

## ✨ What you get

### 🖥️ Native 1080p and up

The game draws natively at **1080p** instead of stretching the PS3's 720p, with sharper edges, a crisp HUD and finer
detail everywhere. F2 → Graphics → *Internal resolution*: 720p / **1080p** / 1440p / 4K. Players play at 1440p too, and native
4K runs at about 66 fps on the test PC's RTX 2070 Super. The game's glow and background blur keep the PS3's look
at every resolution (F2 → Effects → *Glow and blur*).

![720p upscaled (left) vs native 1080p (right)](docs/feat-1080p.jpg)

### 🎮 Full controller support

**Plug in any controller and play.**
- Xbox, DualShock 4, DualSense, Switch Pro, 8BitDo and every pad in the community
  [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) work out of the box.
- Turn it on before or during the game: it's picked up while you play.
- Buttons work by position, as on a PlayStation pad (bottom = ✕, right = ○).
- The game's button pictures follow what you play with: PlayStation, Xbox, or keyboard and mouse (F2 → Controls
  says which one it shows).
- **Remap any button in the game:** F2 → Controls → *Buttons and keys* lists every PS3 control, each with two
  keys, a mouse button and a controller button. Click a cell (✕ on a controller) and press the new one;
  right-click (□) clears it. An input that is already used asks first: *Swap* or *Cancel*. A change applies at
  once and is saved in `input.toml`. *Reset to defaults* puts everything back.

### 🖱️ Direct mouse aiming

The mouse turns the camera **directly**, by how far you move it, as in a PC game. Before, the mouse moved a
pretend stick, and the game's stick speed limit made quick turns and aiming feel slow.
- F2 → Controls → *Mouse camera*: **Direct** (the default) or *Like a stick*
- *Mouse sensitivity*, and a separate *Aiming sensitivity* for while you aim
- in menus, the map and cutscenes the mouse still works like a stick; a controller plays as before

### 💡 Enhanced lighting

Soft sun shadows instead of stair-stepped ones, contact shadows (ambient occlusion) at full resolution instead
of half, and smooth light gradients without banding. Every option is in F2 → Lighting, and **F3** flips
between your picture and the original PS3 look.

The fine stripes on faces and clothes in the Kuo cutscene at the dock are fixed: the game's shadow offset now
reaches the graphics card, as on the PS3.

![The original PS3 look (left) vs the new lighting (right)](docs/feat-lighting.jpg)

### 🔥 HD fire, smoke and effects

The PS3 drew every fire, smoke, spark and explosion into a tiny 512×288 image and stretched it over the screen,
which gave blocky edges and blobby flames. Here they're drawn at full resolution (F2 → Effects). The port also
keeps the glow around lightning, lamps and fire as the PS3 made it at any resolution (F2 → Effects → *Glow and blur*), and it
loads **texture packs** (PNG/DDS), so the community can make HD versions of any texture. Packs are made from your
own game's textures, so none are included.

![Fire and smoke: as the PS3 drew them (top), at full resolution (middle), plus an HD texture pack (bottom)](docs/feat-effects.jpg)

### 🧪 In-game cheats menu

F2 → Cheats:
- **Invincibility**, **infinite energy**, **max battery**
- **super jump** (2× to 10×)
- **karma rank**: pick any of the game's seven ranks (Hero to Infamous) and karma is set inside it when you close the menu, through the game's own karma setter
- **karma lock** (blue/red only), a karma multiplier, **XP for karma**, *Add karma* / *Add XP* buttons
- **unlock all powers**: every power in the game is Cole's at once (rockets, Kinetic Pulse, the Ionic powers, every upgrade, hero and evil together), through the game's own power code, and the story can't take them back while it's on; *karma powers of both sides* alone lets hero and evil karma powers work together. The game saves the powers: try it in a New Save Data slot
- **infinite ionic charges**: Cole always has 3, the game's most, and they refill the moment one is used
- **one-hit kills**, enemy speed, enemies hold fire
- **longer glide**: Cole sinks more slowly while gliding (R1, the Static Thrusters), so a glide lasts up to 3× as long and goes further
- L_'s patches, re-made for the port: **black lightning** (electric arcs drawn with the game's evil-lightning shader), **Static Thrusters flight** (while gliding, Cross climbs, Circle dives, neither hovers; needs the Improved Static Thrusters upgrade) and a faster glide
- **saved spots**: *Save this spot* remembers where Cole stands, which way he faces and the camera; *Go to saved spot* (or **F7**, menu open or closed) puts him back there, through the game's own placing code (two places in New Marais come with it)

Cheats can be remembered between sessions, with a short reminder at start.

![The Cheats page](docs/feat-cheats.jpg)

### 🧩 Mods

The community's inFAMOUS 2 mods work as they are: the ones made for tiffsomniac's **inFAMOUS Mod Manager**
(whole files, texture patches, xpps patches, and mods with options).
- **F2 → Mods → Add a mod from a zip**: the zips in your Downloads folder, checked first (only mod files are
  taken out, never a program), installed into the **`mods`** folder next to the game; or unzip a mod there yourself
  (`mods\<mod>\mod.json`)
- **no Unlocker** and nothing to install: your game files are never changed, the port hands the game the mod's
  files in their place
- **F2 → Mods**: every mod with its preview picture and what it changes, turn each on or off, pick its options;
  the page shows at once what the next start will use, and **Restart to apply** restarts the game to its title
  screen (Continue there to go on from your last save)
- options that all change the same texture (a T-shirt in seven designs) are one at a time, as radio buttons;
  options that change different things (a skin for the sword, the amp and the outfit) are check boxes
- two mods that change the same file: **Use this one instead** picks the one that wins (the page says first what
  the other loses), else the first by folder name
- a texture patch made for another version of the game is never written beside its texture: the mod is left out
  and the page says which option to untick (for example *"15 textures were made for another version of
  cole_gold_amp.xpps: untick Solid Gold Amp"*)

How it works and the mod format: [`engine/docs/MODS.md`](engine/docs/MODS.md).

### 💾 Save manager

The PS3's own "choose a save" screen, which the port didn't have before:
- every save with its picture, mission, completion and date
- **New Save Data**, so you never overwrite by accident
- overwrite and delete, each asking first
- real Yes/No answers to the game's questions, like "save before loading?"

![The Load Game screen](docs/feat-saves.jpg)

### 🏆 Trophies, in Playnite and RecompLauncher

The game's 52 trophies unlock as on a PS3, the platinum with the last of the others, with a notice in your
language. The port keeps them in `game\gamedata\trophies\NPWR01730.json` and also writes them the way RPCS3 does,
in `game\gamedata\dev_hdd0\home\00000001\trophy\NPWR01730_00\` (the names, the icons and `TROPUSR.DAT` with the
unlock times), so trophy trackers made for RPCS3 can read them.

**Playnite** ([Playnite Achievements](https://github.com/justin-delano/PlayniteAchievements) or
[SuccessStory](https://github.com/Lacro59/playnite-successstory-plugin)). Both look for an emulator with RPCS3 in
its name, so the port gets one:
1. Library → Configure Emulators → Add: name it `RPCS3 inFAMOUS 2 port`, installation folder
   `<port folder>\game\gamedata`, and one custom profile whose executable is `<port folder>\Play inFAMOUS 2.cmd`.
2. Add the game by hand: installation folder `<port folder>\game\disc\PS3_GAME` (the plugins find the game's
   `TROPHY.TRP` there), and a play action of the Emulator type with that emulator and profile.
3. Playnite Achievements: turn on its RPCS3 source. SuccessStory: Settings → RPCS3 → turn it on and add
   `<port folder>\game\gamedata\dev_hdd0\home\00000001` as a home user folder.
4. Start the game once (the folder is made then), then refresh the game's achievements.

Already set up for RPCS3? Add `PS3_TROPHY_RPCS3_DIR=<your RPCS3 folder>` to `settings.ini` and the port also
copies its unlocks into RPCS3's own trophy folder for the game. It never locks anything or moves a date later
there, and it keeps a copy of RPCS3's file once (`TROPUSR.DAT.port-backup`). With Playnite Achievements, the
game's Manage Achievements → Overrides → Platform Override: RPCS3, `NPWR01730_00` then finds them.

**RecompLauncher:** started from RecompLauncher, the port reports each trophy to
[Recomp Achievements](https://github.com/elripalda/RecompAchievements). A cheat that changes play (F2 → Cheats)
makes the session's later unlocks Casual. Picking or saving a spot and Black lightning don't.

### 🎨 A menu that looks like the game

The settings menu, the save list and the game's questions share an inFAMOUS-style theme: **Hero** (electric blue)
or **Infamous** (ember red), in F2 → Display.

![The menu in Hero and Infamous themes, the save list, and a Yes/No question](docs/feat-menu.jpg)

### ⚡ Smoother, too

Looking out over the city from a rooftop used to drop the game to **4–10 fps**: the texture cache was too small
and every frame rebuilt hundreds of textures. With a 4× larger cache and buffers that grow instead of stalling,
the same overflow, forced on purpose, went from **13 to 40 fps**. With normal settings the busy street in our
test ran at **75 fps with no frame over 37 ms**. That rooftop view now runs at **80–91 fps** at native 1080p with
every extra on.

Cutscene camera cuts and loading used to stutter while the graphics card's drawing programs (pipelines) were
made on the spot. They are made on helper threads now, and the ones the game used before are prepared ahead at
the title screen (`pipelines.rcp`), so the cuts stay smooth.

Every session also watches itself: a frame that takes longer than 80 ms is recorded together with what each
of the game's threads was doing at that moment (`play\<date-time>\hitches.tsv`, summed up in `summary.txt`), so a
stutter you report comes with its cause.

### 🎮 In the city on this port

![Four camera views in the city at native 1080p with every picture extra on: 99, 103, 133 and 140 fps](docs/city-views.jpg)

## 🧭 What this is

Setup translates the game's PS3 code into PC code **on your machine** and compiles it. The game then
runs directly on your CPU, with its graphics drawn through Vulkan.

> 🧪 **Public test.** It boots, plays and saves, and players run it on Windows 10 and 11 PCs and on Linux.
> Expect bugs; please [report them](../../issues) with the session's `play\<date-time>` folder.

> 🔒 **No game content is included, and none is downloaded.** You need your own copy of the game,
> and RPCS3 for Setup. Builds of the game (the `.exe`) are never shared, because they contain the
> game's code. Each player builds their own.

## 📋 Status

| | |
|---|---|
| ✅ Tested copies | Played and confirmed by players: the **Europe** disc (**BCES01143**), the **Turkey** disc (**BCES01229**) and the **US** disc (**BCUS98125**), each with the **1.04** update, and the **digital (PSN)** version 01.03. |
| 🖥️ Tested PCs | The test PC: i5-13600K, RTX 2070 Super, 32 GB, Windows 11, 1080p to 4K. Players: Windows 10 and 11 PCs, up to an RTX 5070 at 1440p. Linux: Ubuntu 22.04 with an AMD GPU (experimental). |
| ⚠️ Known issues | • Views over a big part of the city (the top of the cathedral spire) are the heaviest: about 83 fps there on the test PC with v0.5.2 (it was 67). On 4- and 6-core CPUs the game's SPU jobs set the pace there, and that is being worked on.<br>• In one long session (about 90 minutes), a big fight had a few freezes of up to one second; the hitch catcher now records what causes them.<br>• On AMD graphics cards, close-up faces in cutscenes can show black spots ([#27](../../issues/27)); a fix is being worked on. |

## 🧰 What you need

- 💿 **Your copy of inFAMOUS 2**
  - The disc's files: the folder that holds `PS3_GAME`.
  - The **1.04 update**, installed in RPCS3.
  - 🛒 **Digital (PSN) copy** (NPUA80638 / NPEA00318, version 01.03, the same code as 1.04; works, tested
    by players): install the game and its license in RPCS3. Then make a folder with a
    `PS3_GAME` folder inside, copy the game's `PARAM.SFO`, `ICON0.PNG` and `USRDIR` from RPCS3's
    `dev_hdd0\game\<ID>` into `PS3_GAME`, and choose that folder in Setup. No update is needed.
- 🕹️ **[RPCS3](https://rpcs3.net)** with the **PS3 firmware** installed (*File > Install Firmware*, using
  `PS3UPDAT.PUP` from playstation.com).
  - It is needed **only during Setup**. Setup uses it to decrypt your game, then copies into the port's
    folder the update, the firmware files the game reads (fonts, system data) and the game's HDD install.
  - After Setup you can remove RPCS3. Keep your game disc folder where it is.
  - Start the game once in RPCS3 until the main menu: that installs 2.4 GB of data that Setup copies.
- 💻 **A PC with:**
  - Windows 10/11 64-bit (or Linux, experimental: [see below](#-linux-experimental))
  - a CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer)
  - a Vulkan graphics card
  - 16 GB RAM
  - about 15 GB free disk space while it builds (about 7 GB after: the game, and the build files Setup keeps so
    updates take minutes)
- 🌐 **Internet.** Setup downloads its build tools once (about 1.3 GB) and keeps them for updates.
  **Nothing is installed on Windows**, nothing needs admin rights, and you need no Git, Python or Visual Studio.

## 📦 Install

1. ⬇️ Download **`Setup.exe`** from the [latest release](../../releases/latest) and run it. Put it in a folder on
   your PC's internal drive, for example `C:\Games\inFAMOUS2`. A USB stick or an exFAT/FAT32 external disk can't
   hold the link Setup makes to your game folder; Setup then installs into `%LOCALAPPDATA%\inFAMOUS2-port` instead.
2. ▶️ Press **Install**. That's all. Setup:
   - 📥 downloads the port's files (about 40 MB) into a folder next to itself
   - 🔎 finds RPCS3 and your game by itself (it asks only if it can't)
   - 🛠️ downloads the build tools into that folder: Python, CMake, Ninja, LLVM and Microsoft's C++ headers and
     libraries (portable copies, about 1.3 GB)
   - 🏗️ builds the game from your copy: 20–60 minutes, one time only (about 15 minutes on the test PC)
   - with **Optimise for this PC** ticked (the default), it then trains the build on your PC: the game opens and
     plays by itself for about 6 minutes (sound off, don't touch it) and is compiled once more with what it
     learned. That adds 10–25 minutes and gave 5–11% more fps in the city on the test PC. If it doesn't work
     out, you get the normal build and Setup says why.
   - 🧹 cleans up, keeps the build tools and build files for quick updates, and leaves a desktop shortcut. Its
     icon is taken from your own copy.
3. 🎮 Press **Play**.

![Setup: one Install button, one progress bar, then Play](docs/setup.png)

> ⚠️ Windows may say *"Windows protected your PC"* the first time, more so while Setup.exe is not signed
> (see [Code signing policy](#-code-signing-policy)): click **More info**, then **Run anyway**. A signed Setup.exe
> names **SignPath Foundation** as its publisher (right-click > Properties > Digital Signatures).

> 📜 Pressing Install accepts Microsoft's [Visual Studio Build Tools license](https://go.microsoft.com/fwlink/?LinkId=2179911)
> for the C++ headers and libraries Setup downloads from Microsoft.

<details>
<summary>🧰 Without Setup.exe (git)</summary>

```
git clone https://github.com/SamiEnazi/infamous2-port
```

(One repository since v0.5.0: the engine and the port base are its `engine/` and `base/` folders, no submodules.)

Then run **`Setup.cmd`** (the PowerShell window), or straight from PowerShell:

```
tools\setup_build.ps1 -Disc <your game folder> -Rpcs3 <your RPCS3 folder> -AcceptMsvcLicense -Cleanup
```

Updating later is the same (`git pull` first), and quick: `-Cleanup` keeps the build tools
and build files, so only what changed is built again. `-NoBuildCache` deletes them too.
</details>

<details>
<summary>🔓 Your own decryption tool (advanced)</summary>

The port never decrypts anything itself. Setup hands the game's 1.04 `EBOOT.BIN` and three firmware modules to
a decryptor, by default RPCS3 (run hidden, nothing to do), and checks that what comes back is a decrypted
program. To use another tool, put a line like this in `config.local.json` in the port's folder:

```
"decryptor": "\"C:\\Tools\\mydecrypt.exe\" \"{in}\" \"{out}\""
```

`{in}` is the encrypted file and `{out}` where the decrypted one must go. A file that is already decrypted
is passed through as it is.
</details>

## 🐧 Linux (experimental)

The port also builds and runs natively on Linux, with no Wine or Proton (tested on Ubuntu 22.04 with an AMD GPU).
It is experimental: less tested than Windows, and there is no Setup.exe. **It runs on the Steam Deck**: a player
built it inside an Ubuntu Distrobox container in Desktop Mode and plays it in Game Mode ([#24](../../issues/24));
in Desktop Mode, start it from that container.

**One script does everything:** download `inFAMOUS2-port-v…-Linux-x64.zip` from the
[latest release](../../releases/latest) (launchers such as Quiver pick it by itself), unpack it, and run
`./setup.sh`. The first time, it finds your game folder in RPCS3's game list (or asks you), builds the game from
your copy (15–60 minutes), then starts it; after that, `./setup.sh` just plays. Or from a `git clone`:

```
git clone https://github.com/SamiEnazi/infamous2-port
cd infamous2-port
./setup.sh                      # or: ./setup.sh --disc <your game folder>
```

(`tools/setup.sh` and `tools/play.sh` still work on their own, with the same options as before.)

`tools/setup.sh` lists what it needs from your distribution (a C/C++ toolchain, python3 with venv, the
Vulkan loader, SDL2) and puts everything else in the port's own folder. Please [report](../../issues) how it
runs on your distribution and GPU.

## 🔄 Updates

- When the game starts it asks GitHub whether a new version is out: in the background, at most every 6 hours,
  and offline it just starts. If there is one, a notice says *Update available: v0.5.1 - F2 > Update*.
- **F2 → Update** shows your version, the new one and what's new. Press **Update when I quit** or **Quit and update
  now**: Setup opens, gets the new version and builds it from your copy, then press **Play**. Your saves,
  settings, mods and controls are kept (the saves are also copied to `%LOCALAPPDATA%\inFAMOUS2-port\save-backups`
  first).
- ⚡ An update that changes a few files takes minutes, not the 20-60 of an install: Setup keeps its build tools and
  build files (about 2.6 GB), downloads no tools and compiles only what changed. `tools\setup_cleanup.ps1` frees
  that space (the next update then takes as long as an install).
- **Check for updates automatically** (F2 → Update) turns the check off; **Check now** asks at once. Running
  `Setup.exe` and choosing *Update to the latest version* still works too.
- A copy installed by RecompLauncher is updated by RecompLauncher.

## 🎮 Playing

- **Start:** `Play inFAMOUS 2.cmd`, or the desktop shortcut. It starts at native 1080p with all the picture
  extras on; F2 changes anything, and your choices are kept.
- **Steam, Playnite and other launchers:** add the game itself, `work\port\build\Infamous2Recomp.exe` in the
  port's folder (in Steam: *Add a Non-Steam Game* → Browse). Started that way, it sets itself up exactly like
  `Play inFAMOUS 2.cmd`, and Steam Input (the Steam Controller, or any controller set up in Steam) and the Steam
  overlay work.
- **Keys:**

  | Key | Action |
  |---|---|
  | **F1** | fps counter |
  | **F2** (or hold L3+R3) | PC settings: Display, Graphics (resolution, anti-aliasing, upscaling, sharpening), Audio (volume, language), Controls (mouse, buttons and keys), HUD, Lighting, Effects, Mods, Update, Cheats |
  | **F3** | your picture vs the original PS3 look |
  | **F7** | go to your saved spot (F2 → Cheats → Saved spots) |
  | **F9** | mark a moment in the session log (a stutter, a glitch, an enemy missing from the minimap) |
  | **Alt+Enter** | fullscreen |

- **Keyboard and mouse:**
  - WASD to move, mouse for the camera (direct by default; F2 → Controls → *Mouse camera*)
  - right button to aim, left button to shoot
  - Space = ✕, Shift = ○, F = □, Q = △
  - To remap keys, mouse buttons or controller buttons: F2 → Controls → *Buttons and keys*.
- **Language:** the game follows your Windows display language (English if the game doesn't have it).
  F2 → Audio → Language picks another one, with the voice and the text set separately, from what your disc
  has: the US disc also has a Latin-American Spanish dub, the Europe disc BCES01143 has English voices with
  menus and subtitles in 16 languages. It applies at the next start.
- **Widescreen:** F2 → Display → Aspect ratio: *Auto* follows your screen's shape, or pick 21:9, 32:9 or
  16:10. 32:9 at 1440p draws 5120×1440 for a super-ultrawide monitor. You see more on the sides (nothing is stretched); the HUD stays in the middle 16:9 and movies get
  black bars. It applies at the next start. 16:9 is the default.
- **Your saves:** `game\gamedata\dev_hdd0\home\00000001\savedata`, backed up to `saves-backup\` before every session.
- **Your settings:** F2's choices go to `%APPDATA%\inFAMOUS2-port\settings.ini` (with `mods.ini` and the saved spots
  in `spots.ini`); your controls are in `input.toml` in the game's folder. With an empty `portable.txt` in the game's
  folder, the settings stay in the game's folder too.
- **Session logs:** each session writes `play\<date-time>\`: frame times every second, your F9 marks, and every
  frame over 80 ms with what the game was doing. Please attach the folder to bug reports.

## 🗑️ Uninstall

Setup installs nothing into Windows: no admin rights, no registry entries, no services. To remove the port:

1. Copy your saves somewhere first if you want to keep them (`game\gamedata\dev_hdd0\home\00000001\savedata`).
2. Delete the desktop shortcut **inFAMOUS 2 (PC port)**.
3. Delete the port's folder (the one with `Play inFAMOUS 2.cmd`). `game\disc` inside it is only a link to your
   game folder: Windows deletes the link, not your game.
4. Delete `%APPDATA%\inFAMOUS2-port` (the F2 settings) and `%LOCALAPPDATA%\inFAMOUS2-port` (Setup's save backups,
   and the port itself when Setup put it there).

RPCS3 and your game are left as they were.

## 🔬 How it works

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) translates the game's PowerPC (PPU) and SPU code, and
  three PS3 firmware modules taken from **your** RPCS3, into C/C++. clang-cl then compiles it.
- The engine's runtime does the rest:
  - stands in for the PS3 system: files, threads, audio, input, saves and the system dialogs
  - draws the PS3 graphics chip's commands with Vulkan, at any internal resolution
  - runs some SPU jobs on the GPU instead (the deferred lighting and SSAO)

Everything is in this one repository (since v0.5.0; before, the engine and the port base were submodules):

| Folder | What it is |
|---|---|
| [`engine/`](engine/) | The engine: the recompiler (`engine/tools/`) and the runtime (`engine/runtime/`, `engine/libs/`). It started as [sp00nznet's ps3recomp](https://github.com/sp00nznet/ps3recomp) and is maintained here now, with the Windows, Vulkan and performance work of this port. MIT, [`engine/LICENSE`](engine/LICENSE). |
| [`base/`](base/) | The port base: [Erik Martins' infamous2-recomp](https://github.com/erikomis/infamous2-recomp) as he published it (commit 8077cda); the build uses its `infamous2-lle/` folder. MIT, [`base/LICENSE`](base/LICENSE). |
| `patches/port/` | Our changes to the port base's files, applied to its copy in `work\port\` at build time (`base/` itself stays as Erik published it). |
| `port_src/` | Files we add to the port. |
| `cheats/` | The game side of the cheats and of the mouse camera. |
| [`natives/`](natives/) | Native versions of the game's heaviest SPU jobs, compiled into the game by default (each has an off switch). |
| `tools/` | The build (`tools/rebuild_win.py`), Setup (`tools/setup_exe/`, `tools/setup_build.ps1`), `tools/play.ps1`, and the tools used to test it. |

## 🤖 Made with Claude Code

This port was built in **2 days and 9 hours**, in one long conversation between **Sami** and
**Claude** (Anthropic's Claude Opus 5.5, working in [Claude Code](https://claude.com/claude-code)).
That's from the first message (6 Nov 2026, 22:42) to the first public release (9 Nov 2026, 07:15).
The same day added native 1080p, the cheats, the lighting, the effects, the save manager and the freeze fix.

| | |
|---|---|
| 🧑 **Sami** | The idea, his own copy of the game, the direction, the playtesting and the feedback. |
| 🤖 **Claude** | The Windows build, the performance work on the main CPU code, the SPU jobs and the graphics thread, the renderer features, the cheats, the save manager, the test and profiling tools, Setup, and this README. The lead worked with up to five sub-agents in parallel. |
| ⏱️ **Time** | 2 days 9 hours, from the first message to public |
| 💬 **Messages** | About 190 from Sami, and 9,560 model responses from the lead plus 32 sub-agents, up to the first release |
| 🔤 **Tokens** | 7.8 million generated, 3.55 billion processed in total up to the first release (3.51 billion of those were re-reads of the cached project context) |

## 💡 Ideas for the community: offline user-created missions

inFAMOUS 2 shipped with a mission editor: players built their own missions (UGC) and shared them online.
The online service that stored and shared those missions is gone, so that part of the game is dead today.

Because this port runs the game's own code on your PC, there is a chance to bring it back offline:
- point the game's mission upload/download at **local files** instead of the old servers
- save, load and play your own created missions without a network
- share missions as plain files, for example in this repository

This is only an idea for now. Nothing is built, and it is not planned yet. Open questions:
- How does the game talk to the UGC service?
- What format are created missions stored in?
- How much of the editor works with no network at all?

If you know how inFAMOUS 2's UGC worked, have notes or captures from back then, or want to think it
through with us, please open an [issue](../../issues).

## 🙏 Thanks and credits

This port stands on other people's work:

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** by Ned Heller / sp00nznet and its
  contributors, and **[Noah Peterson's fork](https://github.com/NoahBPeterson/ps3recomp)**: the
  recompiler and runtime this port is built on (MIT, Copyright (c) 2026 Ned Heller / sp00nznet). This
  project started from it and our fork of it ([SamiEnazi/ps3recomp](https://github.com/SamiEnazi/ps3recomp/tree/infamous2),
  up to v0.4.2); since v0.5.0 that engine is the `engine/` folder of this repository and is maintained here,
  with its full license in [`engine/LICENSE`](engine/LICENSE) and its contributors in `engine/CONTRIBUTORS.md`.
- **[erikomis/infamous2-recomp](https://github.com/erikomis/infamous2-recomp)** by Erik Martins: the
  inFAMOUS 2 port this one started from, and the camera patch behind widescreen (MIT, Copyright (c) 2026 Erik
  Martins). It is the `base/` folder of this repository (his commit 8077cda, unchanged), with its license in
  [`base/LICENSE`](base/LICENSE) and its credits in [`base/CREDITS.md`](base/CREDITS.md).
- **[ThiagoFraga761/infamous2-pc](https://github.com/ThiagoFraga761/infamous2-pc)**: the Vulkan SSAO and
  lighting passes and Windows groundwork we adopted, the texture-cache and buffer-growth ideas behind the
  freeze fix, and the idea of giving the game's Edge geometry code the real picture size (MIT). Also
  the **native versions of the game's heaviest SPU jobs** in [`natives/spu/`](natives/): the Edge geometry
  kernels (skinning, culling, vertex decompression), the BSP polygon clipper and the render-list builder are
  ports of his `edge_kernels_native.c`, `job220_native.c` and `ec5f_native.c` to this game version (MIT,
  Copyright (c) 2026 the infamous2-recomp contributors; his notice is kept in each file), with AVX2 paths
  added here. They are what makes the port fast on 4- and 6-core CPUs.
- **[SIMDe](https://github.com/simd-everywhere/simde)** (MIT): the 68 headers the Edge kernels need are in
  `natives/spu/simde-0.8.2/`, with its license.
- **bungholio** and the [ArtemisPS3](https://github.com/bucanero/ArtemisPS3) cheat-code collection: three of the
  cheats (jump height, AI speed, enemies hold fire) are their codes for this exact game version, turned into
  menu switches, their "Hold L2 + X To Fly" code led us to the game's glide physics, and their "Have All Powers" and
  "Can Use All Powers" codes to the game's power state (see `cheats/NOTES.md`).
- **L_**: the RPCS3 patches "Black Lightning", "Static Thrusters - Flight" and "Static Thrusters - Fast" for this
  exact game version. On the port they are re-made as run-time hooks with the same effect (F2 → Cheats; how in
  `cheats/NOTES.md`).
- **tiffsomniac**: the **inFAMOUS Mod Manager** and its mod format (whole files, texture and xpps patches,
  options). The port reads these mods as they are (F2 → Mods); its own code, written from the format.
- **Yahfz**: the RPCS3 community patch "Disable Mesh Trimming" (one instruction word in the game's Edge
  geometry SPU program), which first showed why faces get holes above 720p. F2 → Graphics → Triangle culling:
  *At the render resolution* (the default) keeps the game's culling but at the real picture size, so no holes
  and a little faster; *Off: whole meshes* is Yahfz's patch; *The PS3's 720p culling* is the original.
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
  - [Recomp Achievements](https://github.com/elripalda/RecompAchievements) (its own license: free for
    games, not MIT; it bundles rcheevos and cJSON, both MIT)

  Their license files are in `engine/third_party/`.

## 🔏 Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

- **What is signed:** `Setup.exe` only. GitHub Actions builds it from this repository
  ([`.github/workflows/setup-exe.yml`](.github/workflows/setup-exe.yml) runs
  [`tools/setup_exe/build.cmd`](tools/setup_exe/build.cmd)), and SignPath signs exactly that build after the
  approver has approved it. The game itself is built on your PC from your own copy and is not signed.
- **Committers and reviewers:** [SamiEnazi](https://github.com/SamiEnazi), the owner of this repository. Changes
  from anyone else (pull requests) are reviewed by him before they are merged.
- **Approvers:** [SamiEnazi](https://github.com/SamiEnazi). Every signing request is approved by hand.
- **Privacy policy:** This program will not transfer any information to other networked systems unless
  specifically requested by the user or the person installing or operating it.
  - Setup.exe goes online only when you press **Install** or **Update** (or a launcher runs its setup step),
    and only to download: the port's files from GitHub, the build tools from their GitHub releases (Python,
    CMake, Ninja, LLVM, SDL), and Microsoft's C++ headers, libraries and Windows SDK from Microsoft. Those sites
    see an ordinary download, under their own privacy statements:
    [GitHub](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement),
    [Microsoft](https://www.microsoft.com/privacy/privacystatement).
  - The update check (see [Updates](#-updates)): when the game starts, `tools\play.ps1` asks GitHub's public API
    which release is the newest (at most every 6 hours, in the background). That question carries no data about
    you, your PC or your game; F2 → Update → *Check for updates automatically* turns it off. A copy installed by
    RecompLauncher or from git never asks.
  - Nothing about you, your PC or your game is sent anywhere: no accounts, no telemetry. The game itself makes
    no network connections (its PSN stand-in stays offline unless you set `PS3_NET_ONLINE` and `PSNR_SERVER`
    yourself). Your saves, settings and session logs stay on your PC; a bug report holds only what you attach.

## ⚖️ Legal

This is a fan project for people who own the game. It is not affiliated with or endorsed by Sony
Interactive Entertainment or Sucker Punch Productions. *inFAMOUS* is a trademark of Sony Interactive
Entertainment LLC. The screenshots show the game running on this port and in RPCS3, for comparison.

This repository contains no game assets, firmware or keys, and no game code beyond the addresses and the few
instruction words the cheats switch, taken from public community codes. Everything else game-specific is read
from your own copy on your own PC. Do not share builds of the game, or files taken from it.

The code in this repository is MIT-licensed (see `LICENSE`). The engine (`engine/`, from sp00nznet's ps3recomp)
and the port base (`base/`, Erik Martins' infamous2-recomp) keep their own MIT licenses and copyright notices
(`engine/LICENSE`, `base/LICENSE`). The libraries in `engine/third_party/` keep theirs;
`engine/third_party/rl_achievements` is under the Recomp Achievements License 1.0, not MIT. The third-party
libraries the build downloads keep their own licenses.
