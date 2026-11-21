# Linux: a double-click installer and "Optimise for this PC" (PGO)

DRAFT section for the pull request description (agent inst, 2026-11-21). Numbers: a clean install on a Steam Deck
(LCD, SteamOS 3.9.5), see "Measured" below.

## What a Linux / Steam Deck player gets

Unpack the Linux zip anywhere and double-click **Install inFAMOUS 2** (Desktop Mode on the Deck, or any desktop with
zenity or kdialog; without either it opens a terminal and asks there):

1. **Start screen**: what the install does (time, disk space, what is downloaded) and one checkbox, **Optimise for this
   PC (PGO)**, ticked as in Setup.exe.
2. **Your game folder**: the one RPCS3's game list has (or an earlier setup used) is offered ("Use it" / "Choose
   another"); otherwise a folder picker. Picking `PS3_GAME` itself, or the folder above the game, works too.
3. **Progress window**: "Step n of 8 (10 with PGO): <setup's own words>", the current part's detail (the build's
   "progress:" lines, the training's note "The game now opens by itself for about 9 minutes ... please don't touch it")
   and the minutes so far. Cancel asks first, then stops setup and the training game; what is done stays.
4. **End**: "Ready to play, optimised for this PC." (or "Optimise for this PC didn't work this time (<why>), so the game
   was built the normal way.") with **Play now** / **Close**, and a **Play inFAMOUS 2** launcher in the same folder
   (it starts `tools/play.sh`; `%k` keeps it working when the folder is moved). On failure: the reason, the failed
   checks of step 1, and the log's path.
5. **Started again** once built: **Play** / **Rebuild the normal way** / **Optimise for this PC** (or **Train again**
   when it is optimised already); a newer version in the folder is pointed out.

No Steam shortcut is made. Nothing outside the folder changes except the menu entry `tools/setup.sh` always made, and
not even that when it belongs to another folder's install (a second copy no longer takes over the first one's entry).
Screenshots of every window: (to attach; taken on the Deck: work/pr/installer-shots/ in the dev tree).

In a terminal nothing changes for existing users: `./setup.sh` as before, plus `./setup.sh --pgo`.

## Optimise for this PC on Linux: the Windows rules, ported

`tools/setup.sh --pgo` (10 steps instead of 8) and `tools/pgo_train.py` are `setup_build.ps1 -Pgo` and
`pgo_train.ps1` on Linux:

- **Step 7**: translate, then compile a training build: `PORT_PGO=gen` into `work/port/build-train`. As on Windows only
  the **game module** is instrumented; the engine is compiled once, normally, into `work/port/build` and copied beside
  it. (Linux compiles its engine locally, so engine PGO is possible later; it needs both binaries' counters written by
  the dump thread, which today writes the module's only when the engine is not instrumented.)
- **Step 8, training** (`tools/pgo_train.py`): the training build plays by itself in a window, sound off
  (`PS3_AUDIO_VOLUME=0`), the player's keyboard, mouse and controllers off (`PS3_KBM=0 PS3_PAD_SDL=0`), the window
  title says what is going on. The same two runs and the same engine mechanisms as Windows:
  - **a new game** (always): START every 1.5 s from the menu's log line (`load_menu_catalog`) until the opening montage
    starts (`intro_montage`); the scripted pad (`PAD_SCRIPT`, `tools/fast_combat.steps`' 21 inputs + the looser second
    round, the same string as `pgo_train.ps1`) starts 17.9 s after the city-falls cutscene's subtitle opens
    (`h_jbi_cityfalls`), through the engine's `PAD_SCRIPT_WAIT` anchor file; without that line, 300 s after the montage.
  - **the newest story save** (when a save folder has a `STATE`): the game loads it by itself; the save pad starts at
    the engine's first world frame 40 s or more after boot (`PS3RECOMP_ENG_WORLD_MARK(_AFTER)`), ~110 s of looking,
    walking, jumping, aiming and firing.
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
  `work/pgo-setup/setup-<time>.profdata` (a new name each time), then the module is compiled with `PORT_PGO=use:<it>`.
- **PGO never makes setup fail.** No `llvm-profdata`, the training build does not compile, no run reached the game,
  the merge fails, or the optimised compile fails: the game is compiled the normal way, and setup says
  `  pgo: not used: <why>` (the installer shows the why). Updates keep the profile and compile with it; when the build
  reports hot code changed since the training (the "retrain" hint), it trains again. The choice is remembered
  (`work/setup-pgo`: `./setup.sh` passes `--pgo` again on an update); `--no-pgo` forgets it. Test switches as on
  Windows: `PORT_PGO_TEST=fail:llvm|fail:trainbuild|fail:compile|stale|stopafter:<s>`, `PORT_PGO_TRAIN`,
  `PORT_PGO_KEEP_TRAIN=1`.
- **The profile never leaves the PC**: it lives in `work/` (never in git); `tools/make_release_zip_linux.py` now
  refuses `*.profdata`, `*.profraw` and `work/` the way it refuses game files.

## SteamOS 3.9: the build works on a stock Deck again

On SteamOS 3.9.5 (this Deck, build 20261023.100) `./setup.sh` stopped at step 1: `MISS C++ headers and libraries`.
SteamOS 3.9's image has no `/usr/include` at all and keeps only `gcc-libs` (libstdc++.so.6, libgcc_s.so.1): no C++
headers, no `crtbeginS.o`, no `libgcc.a`. #81's fix covered the C headers only. `tools/libc_headers.sh` now does the
same for the C++ side: SteamOS 3.9's own `gcc` package (the same version as its `gcc-libs`, from Valve's package
mirror, SHA-256 checked, 61 MB), only its C++ headers and GCC's start files and static helpers, into
`tools-cache/sysroot-gcc`, used as clang's GCC installation (`--gcc-install-dir=<CFGDIR>/...` in the `.cfg`). The
game still links against and runs with the system's own libstdc++. The step-2 compiler self-test passes, and the whole
install below ran on it.

## Measured (Steam Deck LCD, SteamOS 3.9.5, a clean install from the release zip)

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
