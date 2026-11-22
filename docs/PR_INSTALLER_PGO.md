# Linux: a double-click installer and "Optimise for this PC" (PGO)

DRAFT section for the pull request description (agents inst and inst2, 2026-11-21; uni, 2026-11-22). Numbers: clean
installs on a Steam Deck (OLED, SteamOS 3.9.5), see "Measured" below. v2 (inst2): the game used where it is (no 14 GB
copy), Optimise trains the engine too, the new game played the way the tutorial asks, and Add to Steam. v3 (uni): the
same installer on every distribution with nothing to install (a downloaded build environment), a double-click program
for GNOME, and the Deck's job-thread count only on the Deck. v3 was installed by the player on a Steam Deck and an
Ubuntu 22.04 laptop (2026-11-22). v4: Add to Steam keeps shortcuts.vdf's permissions (see "Add to Steam"), a
**Play inFAMOUS 2** program, and no .desktop or .cmd files in the Linux folder.

## What a Linux / Steam Deck player gets

Unpack the Linux zip anywhere and double-click the program **Install inFAMOUS 2** (Desktop Mode on the Deck, or any
desktop; GNOME, when its file manager will not start it: right-click,
**Run as a Program**, or `bash tools/install_gui.sh`). The installer uses zenity or kdialog; without either it opens a
terminal and asks there:

1. **Start screen**: what the install does (time, disk space, what is downloaded) and two checkboxes, both ticked:
   **Optimise for this PC (PGO)** (as in Setup.exe) and **Add to Steam** (shown when Steam is installed; then a box
   for the name it gets in the library).
2. **Your game folder**: the one RPCS3's game list has (or an earlier setup used) is offered ("Use it" / "Choose
   another"); otherwise a folder picker. Picking `PS3_GAME` itself, or the folder above the game, works too. Then
   **how to use it**: from where it is (the default, no copy) or copied into the port's folder (below).
3. **Progress window**: "Step n of 8 (10 with PGO): <setup's own words>", the current part's detail (the build's
   "progress:" lines, the training's note "The game now opens by itself for about 9 minutes ... please don't touch it")
   and the minutes so far. Cancel asks first, then stops setup and the training game; what is done stays.
4. **End**: "Ready to play, optimised for this PC." (or "Optimise for this PC didn't work this time (<why>), so the game
   was built the normal way.") with **Play now** / **Close**, and a **Play inFAMOUS 2** launcher in the same folder
   (it starts `tools/play.sh`; `%k` keeps it working when the folder is moved). On failure: the reason, the failed
   checks of step 1, and the log's path.
5. **Started again** once built: **Play** / **Rebuild the normal way** / **Optimise for this PC** (or **Train again**
   when it is optimised already) / **Add to Steam**; a newer version in the folder is pointed out.

Nothing outside the folder changes except the menu entry `tools/setup.sh` always made (and not even that when it
belongs to another folder's install: a second copy no longer takes over the first one's entry) and, only when Add to
Steam is ticked, one new entry in Steam's shortcut list.
Screenshots of every window: (to attach; taken on the Deck: work/pr/installer-shots/ in the dev tree).

In a terminal nothing changes for existing users: `./setup.sh` as before, plus `./setup.sh --pgo` and
`./setup.sh --link-game`.

## v3: the same on every distribution, nothing to install

Before v3 step 1 checked the distribution's packages (a C/C++ toolchain, python3 with venv, the Vulkan loader, SDL2)
and told the player to install what was missing; SteamOS, which has no headers and a read-only system, got its own
glibc / gcc packages downloaded. Now `tools/setup.sh` downloads the whole build environment into `tools-cache/`, the
same files on every distribution, each pinned to an exact version, address and SHA-256 (`tools/buildenv.sh`):

| what | from | size | SHA-256 |
|---|---|---|---|
| Python 3.14.10 (python-build-standalone 20261026, `x86_64-unknown-linux-gnu-install_only_stripped`) | GitHub (astral-sh) | 37 MB (109 MB unpacked) | `4d3c16c5...412aed` |
| sysroot: `libc6`, `libc6-dev` 2.35-0ubuntu3 (Ubuntu 22.04's glibc) | archive.ubuntu.com, else Launchpad | 3.2 + 2.1 MB | `ea9a27e0...6b6db2`, `cc37cab5...370c56` |
| sysroot: `linux-libc-dev` 5.15.0-25.25 (kernel headers) | same | 1.3 MB | `1b2a9302...5336d8` |
| sysroot: `libgcc-12-dev`, `libstdc++-12-dev`, `libgcc-s1` 12.3.0-1ubuntu1~22.04.3 (GCC 12.3) | same | 2.6 + 2.2 + 0.05 MB | `a9933d99...eddca0e`, `250f538c...6ed1e`, `d383e642...4589a` |
| SDL 2.32.10 (`libsdl2-2.0-0` 2.32.10+dfsg-4+steamrt1.1+srt1, Valve's Steam Runtime 1 build) | repo.steampowered.com | 0.6 MB | `102f431d...e91052` |
| LLVM 22.1.8, CMake 3.31.3, Ninja 1.11.1, Vulkan headers, glslang 15.1.0, SDL2 2.30.10 headers | GitHub (as before; CMake and Ninja now always, never the system's) | | as before |

- **The sysroot** (`tools-cache/sysroot`, 40 MB unpacked by `tools/buildenv.py deb`: no dpkg or ar needed; absolute
  links made relative so nothing points at the system; no docs, locales, sanitizer libraries or static libc) is the
  compiler's whole world: `tools-cache/llvm/bin/clang.cfg` = `--sysroot=<CFGDIR>/../../sysroot -static-libgcc`,
  `clang++.cfg` adds `-static-libstdc++` (clang does not warn about config-file options a compile does not use). The
  headers, crt files, `libc_nonshared.a`, the kernel headers, libgcc and libstdc++ all come from there, nothing from
  `/usr/include`, and the game links against glibc 2.35's own `libc.so.6`, so it asks for no newer symbol versions.
  The engine then needs `libSDL2-2.0.so.0 libm.so.6 libc.so.6 ld-linux-x86-64.so.2` and nothing newer than
  GLIBC_2.35 (`tools/buildenv.py elf` checks it after the build and in the compiler self-test); the game module links
  nothing at all (as before). Runtime needs: glibc 2.35 or newer and the GPU's Vulkan driver.
- **SDL2** for the game is Valve's own Steam Runtime build (2.32.10), which loads X11, PulseAudio / PipeWire / ALSA,
  udev and D-Bus by itself when they are there (it has no Wayland backend: `tools/play.sh` already starts the game
  through XWayland on a Wayland desktop). The engine links against it and finds it beside itself
  (`work/port/build/libSDL2-2.0.so.0`, RUNPATH `$ORIGIN`, copied with the engine into training builds). Valve's pool
  keeps only its newest build: when the pinned file is gone, setup uses the system's `libSDL2-2.0.so.0` as before and
  says so. Another SDL2 for one session: `SDL_DYNAMIC_API=/usr/lib/libSDL2-2.0.so.0 tools/play.sh`.
- **Python**: `tools-cache/python` runs step 1's game checks, `work/venv` (made again when an older setup made it
  with the system's python3), every tool setup runs, the installer's Steam helper and play.sh's session summary.
- **Step 1** checks only what cannot be downloaded: x86-64, AVX2, glibc 2.35 or newer, a Vulkan driver (the loader
  and an ICD file), free disk space (MISS under 12 GB), curl / tar / gzip; plain words, no package names.
- **Fallback, so the Deck cannot regress:** when the sysroot cannot be downloaded or unpacked, or its compiler
  self-test fails (a C and a C++ program compile, link, run, and the C++ one needs no `libstdc++.so.6`), setup writes
  the old `.cfg` (the system's own GCC folder, else SteamOS's gcc package into `tools-cache/sysroot-gcc`; SteamOS's
  glibc / linux-api-headers headers into `tools-cache/sysroot-fix`) and tests that. The C++ library is then linked
  the old way (dynamically).
- **A changed compiler set** (the first build after this update, or the fallback) rebuilds what was compiled with the
  other one: glslang (`deps/glslang-install-linux/.toolchain`) and the engine's CMake build (`work/setup-toolchain`;
  the game module's objects stay: the module links nothing but itself).
- **No xz or unzip** on the system: LLVM's `.tar.xz` and Ninja's zip are unpacked by the port's Python.

**The launchers** (`tools/launcher/launcher.c`, ~230 lines, compiled twice): GNOME's file manager runs programs on a
double-click but not `.desktop` files kept in a folder, so the zip's top folder has two programs, **Install inFAMOUS
2** and **Play inFAMOUS 2** (`-DPLAY`). Play starts `bash tools/play.sh` with its arguments when
`work/port/build/Infamous2Recomp` is there, else the installer; play.sh shows its errors in a message box when it has
no terminal (the game folder gone, files missing or encrypted, no AVX2, not built). It has no C library at all (Linux system calls, `-nostdlib -static`: 3 KB with lld, 9 KB with GNU ld), so the
same file runs on any x86-64 Linux. It starts `tools/install_gui.sh` from its own folder; without zenity / kdialog it
opens a terminal (konsole, ptyxis, kgx, gnome-terminal, xfce4-terminal, mate-terminal, tilix, x-terminal-emulator,
alacritty, kitty, foot, xterm; install_gui.sh's own list is the same). It is not committed as a binary:
`tools/make_release_zip_linux.py` compiles it from the commit's source (`--llvm`, else `PORT_LLVM`, else
tools-cache/llvm, else clang on PATH; lld, else the GNU linker), checks it is a static executable, prints its SHA-256,
and adds it (mode 755) with **How to install on Linux.txt**.

v4: no `.desktop` files in the folder any more. Dolphin showed "Install inFAMOUS 2" twice (the program and the
entry), and the installer's "Play inFAMOUS 2.desktop" did nothing on GNOME. The programs work everywhere, and the
installer removes the old entries (only ours: the name and the `sh %k` line). The Windows starters at the top
(`Setup.cmd`, `Play inFAMOUS 2.cmd`) are left out of the Linux zip, and the zip script refuses any other
`.desktop` / `.cmd` / `.bat` / `.exe` at the top. Kept: setup's menu entry
(`~/.local/share/applications/infamous2-port.desktop`). It isn't in the folder, it shows the game in the application
menu, and Steam's "Add a Non-Steam Game" lists it.

**Job threads: the Deck only.** `PS3_NATIVE_JOBS_THREADS=4` is set by `tools/play.sh` (and the trainer) only on a
Steam Deck (board vendor Valve: LCD and OLED); every other PC gets the engine's own rule (physical cores - 2, at most
8). Every other Deck tuning in play.sh stays on for every Linux PC.

## Use the game where it is (no 14 GB copy)

Since v0.6.0 setup copies the game folder into the port's folder (`game/disc`), so the port's folder can be moved or
copied anywhere and still plays. On a Steam Deck the game is usually on the SD card (EmuDeck), which stays in, and the
copy costs 14 GB of internal storage and 4 minutes. Nothing needs the game files to be local: the game only reads them
(`/dev_bdvd`, `/app_home`), and everything that is written or decrypted is in the port's folder in both ways: the
decrypted EBOOT (`eboot/`), the firmware (`work/fw`, `game/dev_flash`), the 1.04 update (`game/dev_hdd0/game/<ID>`),
the game's own 2.4 GB HDD data (`game/dev_hdd0/game/<ID>_INSTALL`), the saves (`game/gamedata`). (The Linux setup takes
a decrypted folder only, so no decryption output lives in the game folder either.)

- `tools/setup.sh --link-game`: `game/disc` is a link to the game folder (as before v0.6.0), nothing is copied.
  `--copy-game` = the copy (still the terminal's default). The choice is remembered (`work/setup-game`), so an update
  keeps it. Switching from a copy to the link deletes the copy only when it is the same game (identical `PARAM.SFO`)
  and every file in it is also in the game folder (an interrupted copy counts); anything else stops setup with a
  message and the copy stays.
- **The installer asks**, after the game folder: **Use the game from its folder (saves 14 GB; keep the SD card in)**
  (the default) or **Copy the game into this folder (14 GB more; then the port needs nothing outside it)**. The copy
  line says so when there is not enough free space. The default is the link because on a Deck the card stays in and
  internal space is what runs out; the copy only buys moving the folder or taking the card out.
- **A missing or moved folder** (`tools/play.sh`): the card is not in (its `/run/media/<user>/<card>` is not mounted):
  "Your game folder is on a drive that is not there now ... put it in, then start the game again", as a message box
  when there is no terminal (the Play launcher, the menu entry, Steam). Moved: #13's search near the port's folder,
  then the same game (the title the port was built for) in RPCS3's game list or any `Emulation/roms/ps3` folder (home
  or any card): linked again. Gone: "not where it was ... run Install inFAMOUS 2 again and choose it". The installer,
  started again with the card out: "Your game folder is not there" with Try again / Choose the game folder / Cancel.

## Add to Steam

A second box on the installer's first screen, **Add to Steam** (shown and ticked when Steam is installed), then a box
with the name the game gets in the Steam library ("inFAMOUS 2 (PC port)", editable). It adds one non-Steam game that
starts the folder's `tools/play.sh` (Game Mode works: the Deck branch of play.sh starts it fullscreen), with artwork
made from the player's own `PS3_GAME/ICON0.PNG` and `PIC1.PNG` (ffmpeg: grid 600x900 and 920x430, hero 1920x620, a
256x256 icon). `tools/steam_shortcut.py` does the file work:

- it only ADDS an entry: Steam's binary `shortcuts.vdf` is read and must write back byte for byte, or nothing is
  written; the new entry is appended and the old file must be the start of the new one; a copy is kept as
  `shortcuts.vdf.bak-<time>`; a picture that exists is never overwritten;
- a name already in the library: the name box comes back with "<name> (2)" and says why; a shortcut that already
  starts this folder: "nothing is added";
- Steam rewrites the file when it exits, so the entry is added only while Steam is closed: the installer asks "Close
  Steam now" (`steam -shutdown`) / "I closed Steam myself" / "Not now", and says to start Steam again;
- several Steam accounts: it asks (the last logged-in first).

Tested only on a copy of a Steam folder (`PORT_STEAM_ROOT`, a stand-in for the steam program): the real library was
only read.

**Fixed after v0.6.3-deck3 (2026-11-22): keep the file's permissions.** On the player's Deck, Add to Steam emptied the
whole non-Steam list a few seconds after it wrote. `steam_shortcut.py` wrote a new file and swapped it in, so
Steam's `rwxr-xr-x` became `rw-r--r--`. NonSteamLaunchers' **NSL Game Scanner** (a user service,
`~/.config/systemd/user/NSLGameScanner.py`, run every ~10 s) checks `os.access(shortcuts.vdf, X_OK)`. If the file
isn't executable, it writes an empty `{'shortcuts': {}}` and then re-adds only its own launchers. 73 entries became 5:
Ubisoft Connect, EA App, Repair EA App, NonSteamLaunchers and AC IV. The copy test never had the scanner, so it didn't
show this.

- Recovered from the tool's own `shortcuts.vdf.bak-20261122-162844` with Steam closed. The scanner then read the file
  and wrote it back unchanged. Proton mappings (config.vdf) and collections were untouched.
- The fix: the new file gets the old file's mode (`rwxr-xr-x` for a new file), and a warning is printed if it can't.
- Checked on a copy: the mode is kept, all 73 entries plus the new one; NSL's own `vdf.binary_loads` reads the result.
- On the real Deck with the scanner running: added, the scanner rewrote the file, and all 74 were kept.

## Optimise for this PC on Linux: the Windows rules, ported

`tools/setup.sh --pgo` (10 steps instead of 8) and `tools/pgo_train.py` are `setup_build.ps1 -Pgo` and
`pgo_train.ps1` on Linux:

- **Step 7**: translate, then compile a training build: the **game module** (`PORT_PGO=gen`, `work/port/build-train`)
  **and the engine** (`PORT_ENGINE_PGO=gen`, `work/port/build-train-eng`), both instrumented. On Windows the engine
  comes prebuilt with the release, so only the module is trained there; on Linux the engine is compiled on the
  player's PC anyway, so it is trained in the same runs. patches/port 0014: each binary has its own profile runtime
  and both read `LLVM_PROFILE_FILE`; with `PS3_PGO_ENGINE_FILE=<path>` the engine writes its own counters there and the
  `PS3_PGO_DUMP_AT` thread writes both (only in a `PORT_ENGINE_PGO=gen` build; without the variable nothing changes).
  If the engine's training build does not compile, the module is trained alone (the engine then compiles normally).
- **Step 8, training** (`tools/pgo_train.py`): the training build plays by itself in a window, sound off
  (`PS3_AUDIO_VOLUME=0`), the player's keyboard, mouse and controllers off (`PS3_KBM=0 PS3_PAD_SDL=0`), the window
  title says what is going on. The same two runs and the same engine mechanisms as Windows:
  - **a new game** (the default, and on Linux the only run by default: the same for every player and every region):
    START every 1.5 s from the menu's log line (`load_menu_catalog`) until the opening montage starts
    (`intro_montage`); from 17.9 s after the city-falls cutscene's subtitle opens (`h_jbi_cityfalls`), the tutorial
    is played the way it asks, section by section (see "The new game" below).
  - **the newest story save** (`PORT_PGO_TRAIN=opening,save`; Windows' default): the game loads it by itself; the
    save pad starts at the engine's first world frame 40 s or more after boot (`PS3RECOMP_ENG_WORLD_MARK(_AFTER)`),
    ~110 s of looking, walking, jumping, aiming and firing.
  - **benchmark scenes** (optional: `--runs street,plaza`, only with training saves in `tools/pgo_scenes/<run>/gamedata`,
    which are NOT shipped): the busy street and the plaza, each a recorded walk (`tools/pgo_scenes/<run>.pad`,
    `PS3_PAD_REPLAY` on the frame-stepped clock) from the save's first world frame. A save is for one title ID; another
    version of the game does not list it, so the run is skipped with a note.
  - Nothing looks at the screen; everything is the game's own log lines and flag files. Each run plays on its own copy
    of the saves (`work/pgo-setup/runs/<run>/gamedata`) and of the settings file (the folder's `settings.ini`, else
    the one `play.sh` would copy in once, else none); the shader cache is the one the game uses (warmed for play).
    The environment is `tools/play.sh`'s (kept in step, as `pgo_train.ps1` keeps `play.ps1`'s).
  - The counters are written every 40 s (`PS3_PGO_DUMP_AT`, patches/port 0007 + 0013: the engine asks the module to
    write them) and each complete write is set aside at once; the run ends with SIGTERM (SDL turns it into the
    window's quit, the module's atexit writer runs), SIGKILL only after 20 s.
  - **The merge rule**: every run that reached the game is merged, however far it got; a run that never left the title
    (new game: no montage / city-falls line, or under 150 s after the montage before the story; save: no world frame,
    or under 30 s in the world) is **not** merged: its profile would mark all gameplay code cold.
- **Step 9**: `llvm-profdata merge` (from the port's own LLVM, `tools-cache/llvm/bin`) into
  `work/pgo-setup/setup-<time>.profdata` (the module) and `engine-<time>.profdata` (the engine; the same runs, the same
  merge rule), a new name each time, then both are compiled with them (`PORT_PGO=use:`, `PORT_ENGINE_PGO=use:`). An
  engine profile that does not work is only left out: the engine then compiles normally and the module keeps its
  profile (`  pgo: the engine is compiled the normal way (its training: <why>)`).
- **PGO never makes setup fail.** No `llvm-profdata`, the training build does not compile, no run reached the game,
  the merge fails, or the optimised compile fails: the game is compiled the normal way, and setup says
  `  pgo: not used: <why>` (the installer shows the why). Updates keep the profile and compile with it; when the build
  reports hot code changed since the training (the "retrain" hint), it trains again. The choice is remembered
  (`work/setup-pgo`: `./setup.sh` passes `--pgo` again on an update); `--no-pgo` forgets it. Test switches as on
  Windows: `PORT_PGO_TEST=fail:llvm|fail:trainbuild|fail:enginetrain|fail:compile|stale|stopafter:<s>`,
  `PORT_PGO_TRAIN`, `PORT_PGO_KEEP_TRAIN=1`; `PORT_PGO_ENGINE=0` trains the module only.
- **The profile never leaves the PC**: it lives in `work/` (never in git); `tools/make_release_zip_linux.py` now
  refuses `*.profdata`, `*.profraw` and `work/` the way it refuses game files.

## The new game: played the way the tutorial asks

The training's new game used fast_combat.steps' fixed presses (from Windows' `pgo_train.ps1`). Watched on the Deck, it
looked random: the camera pushed during a cutscene, a walk during the "look left" prompt, 42 s of aiming into the sky
and 6 bolts at nothing, 30 s walking into a railing, a bolt at the gangway's chain; it never reached the gate. Linux's
trainer now plays it section by section (`Player` and `OPENING_SECTIONS` in `tools/pgo_train.py`), on `PAD_FILE`
(closed loop: a hold is the same short command written every 0.1 s, so it lasts its seconds at any frame rate):

1. **The look tutorial** ("Use the right stick to look up / left / right": the game stops at each until it is done):
   from 3 s after Zeke's cutscene opens (its subtitle file, `h_jbi_bus`), three calm rounds of look up, back down,
   left, back right, each pair as long as each other, so the camera ends where the game put it: along the pier.
2. **"Head towards the disturbance"**: run down the pier toward the EMPIRE CITY arch, a step to the left off the
   railing the run may end against, then on until the gate stops Cole (the camera turns in behind him).
3. **"Blast open the gate"** ("Hold L1 to aim", "Press R1 to fire lightning bolts"): the gate opens when a bolt hits
   its chain lock between the two halves, and Cole stops a little to one side: a step back, aim, and look along the
   gate for the lock, a bolt at the middle and then at small camera turns outward (left, right, further left, ...).
4. Through the gate, past the people running away, down the pier as it collapses (a scripted sequence), until Cole
   climbs the post at its end.
5. **"Shoot at the source of disturbance"** from the post: aim and fire five bolts. (The Beast rises: a ~50 s
   cutscene.)
6. **"Take down the Beast"**: aim and fire, strafe, jump, three times.

Watched on the Deck (iteration 4, the sweep from the middle to the left): the prompts answered in turn, the run
straight down the pier to the arch, the gate blown open at the third bolt, the run through the collapsing pier, the
climb, the shots at the source, the Beast's cutscene, and the fight; the whole new game 422 s, about 7 minutes. The
final version searches outward from the middle, so fewer bolts follow once the gate is open; watched again (iteration
5): the gate open after a few bolts, the same run, climb, shots and fight; 412 s.

Nothing looks at the screen: the game's log has the two cutscene marks (`h_jbi_cityfalls`, `h_jbi_bus`) and nothing
after them up to the gate, so the later sections are timed from the end of the one before, with moves that are right
at any frame rate. Region: the trainer depends on no title ID; its marks are file names inside the game's data
(`load_menu_catalog`, `intro_montage`, `h_jbi_cityfalls`, `h_jbi_bus`), the same on every disc, with `PS3_LANG=en`
pinning the language (and so the movies and timings); the 1.04 EBOOT is the same code on the tested discs. Windows'
`pgo_train.ps1` keeps its fixed presses (not changed here).

## SteamOS 3.9: the build works on a stock Deck again

(v3: this is now the fallback; the sysroot above is the normal way on SteamOS too.)

On SteamOS 3.9.5 (this Deck, build 20261023.100) `./setup.sh` stopped at step 1: `MISS C++ headers and libraries`.
SteamOS 3.9's image has no `/usr/include` at all and keeps only `gcc-libs` (libstdc++.so.6, libgcc_s.so.1): no C++
headers, no `crtbeginS.o`, no `libgcc.a`. #81's fix covered the C headers only. `tools/libc_headers.sh` now does the
same for the C++ side: SteamOS 3.9's own `gcc` package (the same version as its `gcc-libs`, from Valve's package
mirror, SHA-256 checked, 61 MB), only its C++ headers and GCC's start files and static helpers, into
`tools-cache/sysroot-gcc`, used as clang's GCC installation (`--gcc-install-dir=<CFGDIR>/...` in the `.cfg`). The
game still links against and runs with the system's own libstdc++. The step-2 compiler self-test passes, and the whole
install below ran on it.

## Measured v2 (Steam Deck OLED, SteamOS 3.9.5, agent inst2)

A clean install from the release zip with the v2 installer, double-clicked, Optimise ticked, "Use the game from its
folder" (the SD card), and for the measurement below all four kinds of training run in one go
(`PORT_PGO_TRAIN=opening,save,street,plaza`, the counters kept), the Deck preset settings + the FPS overlay:

| phase | wall time |
|---|---|
| 2 build tools | 125 s |
| 3 your game: linked (was 250 s with the 14 GB copy) + Sony's update and firmware (433 MB) | 23 s |
| 4-6 decrypt, glslang | 24 s |
| 7a translating the game (ppu 644 s, spu 11 s, jobs 60 s) | 715 s |
| 7b training build: engine (instrumented) 43 s + module (instrumented) 547 s | 590 s |
| 8 training, four runs: new game 386 s, save 153 s, street 147 s, plaza 161 s | 850 s |
| 9 optimised compile: engine 37 s + module 513 s | 550 s |
| **whole install** | **47.9 min** |

With the default training (the new game only, ~400 s) that is about **40 minutes**; the normal way (no Optimise) about
24 minutes (the translation, the engine and a normal module, 469 s). Training the engine as well costs about 1.3
minutes (its instrumented and its optimised compile; the normal build compiles it once too); the instrumented engine
makes the training game slower but not longer (its sections are timed). Disk: 9.1 GB after the install with the game
used from the card (tools-cache 4.9 GB, the game's HDD data 2.4 GB; here the two training builds were kept for the
measurement, ~2 GB, normally deleted) instead of 23 GB with the copy.

**Which training** (each against the same build without PGO; engine AND module trained in every variant; Deck preset,
GPU 1100 MHz, frame-stepped guest clock, FPS overlay on; rotated rounds, every build once per scene per round, the
order rotated and reversed; abseq's rules on the pairs with the no-PGO run of the same round):

| training (engine + module trained in each) | quick street, guest 4-20 s | plaza, guest 16-100 s | training time |
|---|---|---|---|
| none (the normal build) | 37.06 fps (main 23.62 ms, RSX 22.24 ms) | 40.17 fps (main 23.11, RSX 22.77) | |
| **(a') the new game only** (the default) | 40.28 fps, **+8.71 % (SE 0.49, 6 pairs)** (main 21.64, RSX 19.37) | 43.10 fps, **+7.31 % (SE 0.95, 6)** (main 21.48, RSX 20.24) | one run, 399 s |
| (b) our benchmark scenes: the street and the plaza (training saves) | 40.37 fps, +8.94 % (SE 0.62, 6) | 43.15 fps, +7.40 % (SE 0.57, 5) | two runs, 308 s |
| (c) all four: the new game (the old fixed script), the newest save, street, plaza | 40.03 fps, +8.02 % (SE 0.57, 6) | 43.13 fps, +7.33 % (SE 0.54, 5) | four runs, 850 s |
| (b) against (a') | +0.21 % (SE 0.49): no difference shown | +0.64 % (SE 0.97): no difference shown | |

All accepted by abseq's rules against no PGO (ACCEPT at 14-18 SE on the street, 8-14 SE at the plaza); between the
variants nothing reaches 2 SE. Two plaza runs went another way (draws 6385 and 6978 against a median of 8281: the
camera or the route differed; b round 2, c round 5) and are left out with their pairs, by the rule set before the
batch (draws more than 10 % off the scene's median). The (a') run is the tutorial as played before the gate sweep
(it stood at the closed gate; see "The new game"), the (c) new game the old fixed presses.

So training the engine as well is what counts: the player-trained module alone gave +3.05 % on the street and +0.56 %
at the plaza (v1, below); with the engine, +8.7 % and +7.3 %. The RSX (graphics) thread, engine code, is 2.5-2.9 ms a
frame faster, the main thread 1.6-2.0 ms. And the new game alone does as well as training on the very scenes measured:
(b) is +0.2 % / +0.6 % over (a') with SEs of 0.5 / 1.0, so (a') leaves at most about 1 % (street) to 2.5 % (plaza) on
the table (the 95 % bounds), and needs no save, which is why it is the default: every player gets the same training,
whatever their copy's region. A training save is for one title ID (the street save is BCUS98125's; an EU game does not
list it and the run is skipped).

## Measured v1 (Steam Deck OLED, SteamOS 3.9.5, a clean install from the release zip)

A clean install from the release zip, double-clicked as a player would, Optimise ticked, the game folder on the SD card
(found in RPCS3's list), the player's four saves copied into the new folder first (so the save run had a save), and the
training run with the Deck's default settings (what a fresh install picks) and the FPS overlay on.

| phase | wall time |
|---|---|
| 1 checks | 0 s |
| 2 build tools (LLVM 22.1.8 1.9 GB download + unpack, SteamOS 3.9 C/C++ headers 73 MB, CMake, Ninja, venv) | 122 s |
| 3 game copied from the SD card (14 GB) + Sony's update and firmware (433 MB in 29 s) | 250 s |
| 4-6 decrypt, glslang | 24 s |
| 7a translating the game (ppu 692 s, spu 11 s, jobs 66 s) | 773 s |
| 7b training build: engine 41 s + instrumented module 675 s | 718 s |
| 8 training: new game 384 s (menu 18 s, montage 30 s, city-falls 188 s; the first start's HDD install ran in the menus) + newest save 153 s (world 40 s) | 538 s |
| merge (llvm-profdata, 2 runs of 16 MB each -> 17 MB) | ~1 s |
| 9 optimised compile (module, "every compiled function matched its profile") | 604 s |
| **whole install with Optimise** | **50.5 min** |
| the normal build for comparison (the installer's "Rebuild the normal way": translate 727 s + module 469 s) | 20 min; a clean install the normal way: about 28 min |

So Optimise for this PC costs about 22 minutes on a Deck (the instrumented compile, 9 minutes of training, and an
optimised compile ~2 minutes slower than a normal one). Disk: the folder is 23 GB after the install (the game 17 GB
with its HDD data, tools-cache 4.9 GB, work 0.8 GB, deps 0.1 GB); the training build (~2 GB) is deleted after use.

**Effect** (the player-trained build against the same build without PGO: same engine binary, only the game module
differs; Deck preset settings, GPU clock 1100 MHz, frame-stepped guest clock, FPS overlay on, rotated pairs with
the stopping rules of `abseq`):

| scene | fps no PGO -> PGO | difference | main thread ms/frame | RSX thread ms/frame |
|---|---|---|---|---|
| busy street, guest 4-20 s | 36.79 -> 37.91 | **+3.05 % (SE 0.87, 4 pairs, accepted)** | 23.45 -> 22.32 | 21.72 -> 21.91 |
| plaza, guest 16-100 s | 40.14 -> 40.35 | +0.56 % (SE 0.62, 8 pairs, no effect shown) | 22.75 -> 22.32 | 22.71 -> 22.77 |

The street is set by the game's main thread (the lifted game code: the module, which the training optimises): it gets
1.1 ms a frame faster. The plaza is set by the RSX (graphics) thread, which is engine code and not trained (as on
Windows, where only the module is trained and the engine comes prebuilt with the release): no change. The ~15 % of our own Deck builds
came from profiles of BOTH the engine and the module trained on benchmark routes; a Linux engine profile is the next
step for the plaza-type scenes (it needs the dump thread to write both binaries' counters to separate files).

## Checked on the Deck

v2 (agent inst2):

- A clean install from the release zip with the new installer (`kioclient exec`, the dialogs answered by a test double,
  no input sent): Optimise + "Use the game from its folder": `game/disc` -> the SD card folder, step 3 in seconds,
  the engine and the module trained in the same runs (each run's engine counters 1.2 MB, written every 40 s next to
  the module's), both profiles used (the engine's build: 2 functions "changed since the profile" = `main` and the dump
  thread's lambda, which differ in the instrumented build only; counts 55 and 5).
- The no-copy paths: setup's copy -> link -> remembered link -> copy, the refusals (another `PARAM.SFO`, a file the
  game folder lacks) and an interrupted copy; play.sh with the card "out" (a link into an unmounted
  `/run/media/deck/<card>`: the message box), a renamed folder on the card and a deleted folder in /home (re-linked to
  the same game from RPCS3's list), and a folder that is gone with no copy anywhere (the message box).
- Add to Steam on a copy of a Steam folder (the real library only read): the guard saw the player's own
  "inFAMOUS 2 (PC port)" entry by name and by program; the copy got one more entry, the old file a byte-for-byte
  prefix of the new one, the backup, the artwork; the installer's flow with a stand-in for `steam -shutdown`.

v1 (agent inst):

- The whole flow above, launched the way Dolphin does (`kioclient exec`), with the player's saves copied in: 50.5 min,
  both training runs used, "every compiled function matched its profile".
- **Rebuild the normal way** from the installer's menu: 20 min, the other install's menu entry untouched.
- **Fallback**: a training closed after 25 s (`PORT_PGO_TEST=stopafter:25`, never past the title): not merged,
  "pgo: not used: the training run gave nothing usable: it did not get past the title screen", the game compiled the
  normal way; its module is byte-identical to the normal build's.
- **Update path**: the installer's **Optimise for this PC** again with the kept profile: "training: not needed (the training of
  2026-11-21 still fits this version)", 20 min, "every compiled function matched its profile"; the module is
  byte-identical to the first optimised build's.
- **Play inFAMOUS 2** (the launcher the installer leaves) started the optimised game through `tools/play.sh` (the
  newest save, the FPS overlay), and it closed cleanly.
- The trainer's game driving with a non-instrumented build (both runs reach their events and close on time; no
  counters, so "why: no readable counters" and exit 1).
- kdialog's progress window through qdbus6 (value, label, cancel, close); zenity 3.44 and kdialog 26.04 on SteamOS.

## Files

v3 (uni):

- `tools/buildenv.sh` (new): the pins and `be_python`, `be_sysroot`, `be_cfg`, `be_sdl2`, `be_vulkan`.
- `tools/buildenv.py` (new): `deb` (unpacks .deb packages into a sysroot), `untar` (no xz), `elf` (what a binary needs).
- `tools/setup.sh`: step 1's checks, Python first; step 2 the sysroot with the old way as the fallback, CMake / Ninja
  always the port's, the venv on the port's Python; step 6 Valve's SDL2, glslang rebuilt on a new compiler set; step 7
  the engine reconfigured on a new compiler set, and what the game program needs.
- `tools/libc_headers.sh`: the fallback now; Python from `PORT_PY`; plain advice (no packages).
- `tools/rebuild_win.py`: RUNPATH `$ORIGIN` for the Linux engine, `PORT_SDL2_RUNTIME` copied beside it (and into
  training builds).
- `tools/launcher/install_launcher.c` (new; v4: `launcher.c`, also Play), `tools/make_release_zip_linux.py` (compiles
  it into the zip, adds "How to install on Linux.txt"; v4: both programs, no Windows starters).
- `tools/install_gui.sh`: setup's Python for Add to Steam (downloaded first on a system with no python3), more terminals.
- `tools/play.sh`, `tools/pgo_train.py`: 4 job threads on a Deck only; play.sh's summary with setup's Python.
- `README.md`: the Linux install says it is the same everywhere with nothing to install, and how to start it on GNOME.

v2:

- `tools/setup.sh`: `--link-game` / `--copy-game` (step 3), the engine trained and used with the module (step 7-9),
  the default run list (the new game; `tools/pgo_scenes/runs.txt` can name others).
- `setup.sh`: a remembered link whose folder is gone is said before it asks; `--link-game` / `--copy-game` in the help.
- `tools/install_gui.sh`: the game folder choice (link / copy), the missing-folder dialog, Add to Steam (checkbox, name
  box, Steam closed for the write, a menu item later).
- `tools/steam_shortcut.py` (new): users / check / add for Steam's binary `shortcuts.vdf`, artwork with ffmpeg.
- `tools/play.sh`: the missing / moved game folder (the card not in, re-link by title), message boxes without a terminal.
- `tools/pgo_train.py`: `--engine-profdata` (the engine's counters), the `Player` and `OPENING_SECTIONS` (the tutorial as
  asked), the optional scene runs (`--runs street,plaza`, `--scene-dir`), no close at a counters write, test switches
  `PORT_PGO_KEEP_RAW` / `PORT_PGO_KEEP_RUN`.
- `patches/port/0014-pgo-engine-and-module-counters.patch` (new): `PS3_PGO_ENGINE_FILE`.
- `tools/pgo_scenes/` (new): the two recorded walks and a README; no saves.
- `README.md`: "Install on Linux and Steam Deck" right after the Windows steps; the Linux section points to it.

v1:

- `Install inFAMOUS 2.desktop` (new, executable): runs `tools/install_gui.sh` through `%k`.
- `tools/install_gui.sh` (new): the installer (zenity, kdialog, terminal).
- `tools/pgo_train.py` (new): the Linux trainer.
- `tools/setup.sh`: `--pgo` / `--no-pgo` (steps 7-9 with PGO), SteamOS 3.9's C++ headers.
- `tools/libc_headers.sh`: `lh_gcc_*` (SteamOS 3.9's gcc package).
- `setup.sh`: `--pgo` remembered and passed on, `--prepare --pgo` no longer ignored, `--find-disc`,
  `PORT_SETUP_NO_BOX=1`.
- `tools/make_release_zip_linux.py`: refuses PGO profiles.
- `README.md`: the Linux section.
- Windows: nothing changed.
