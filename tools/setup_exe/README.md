# Setup.exe: one Install button

`Setup.exe` is the beginner's way in: one window, one big **Install** button, one progress bar, then
**Play**. It wraps the repo's own PowerShell setup and does not redo it.

What Install does, in order. It asks nothing it can find out itself:

1. **The port's files.** If Setup.exe was downloaded on its own (no `tools\setup_common.ps1` beside it), it
   downloads the port from GitHub as one ZIP file, with no git: the port at the newest commit of `main`,
   with the engine (`engine\`) and the port base (`base\`) in it. (Before v0.5.0 these were two more
   downloads, the `toolkit` and `community/erikomis-infamous2-recomp` submodules at the commits the port
   pinned.)

   **Older Setup.exe files keep working.** A Setup.exe of v0.4.2 or older asks GitHub for those two pins
   before it downloads anything, so the public repository keeps them (`.gitmodules` and two gitlinks, made
   by `tools\publish_export.ps1`: the toolkit at v0.4.2's commit, the port base at 8077cda). Such a Setup.exe
   then downloads the port (with `engine\` and `base\`) plus the two pinned folders, and runs the new
   `tools\setup_build.ps1`, which builds from `engine\` and `base\` only. It copies the new files over the old
   ones without deleting, so `setup_build.ps1` deletes what `engine\` and `base\` hold beyond the version's own
   list (`.port-files`, also made by the export). This Setup.exe ignores the pins, and deletes `toolkit\` and
   `community\erikomis-infamous2-recomp\` when an older Setup.exe or a release zip made them.

   It goes into a folder next to Setup.exe (else `%LOCALAPPDATA%\inFAMOUS2-port`), about 40 MB. Then
   Setup.exe copies itself in.
2. **RPCS3 and the game.** It finds them by itself:
   - RPCS3: the saved choice, a running RPCS3, next to Setup.exe, the user's Desktop, Downloads and
     Documents, Program Files, the top of each drive
   - the game: RPCS3's own game list (`config\games.yml`)

   Only if one of them can't be found does a small *"Where is RPCS3?"* or *"Where is your inFAMOUS 2
   game folder?"* dialog appear.
3. **The checks** of `tools\setup_common.ps1`. A one-line reason appears only for what the player must
   fix: no PS3 firmware or no 1.04 update in RPCS3, RPCS3 still open, or a PC that can't run the port.
4. **The build:** `tools\setup_build.ps1 -AcceptMsvcLicense -Cleanup`. It downloads portable build
   tools into `tools-cache\` with `tools\fetch_tools.ps1` (nothing is installed, no admin rights):
   - CPython 3.12
   - CMake 3.31
   - Ninja 1.11
   - LLVM 22.1.8
   - Microsoft's MSVC 14.38 headers and libraries, and the Windows SDK 10.0.22621, taken from
     Microsoft's own packages the way [portable-msvc](https://gist.github.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977)
     does

   Then it builds the game and deletes the tools and the build's leftovers (`tools\setup_cleanup.ps1`).
   The status line reads "Downloading tools... / Building... 3 of 7 / Cleaning up...".
   With **Optimise for this PC** ticked (the default) it passes `-Pgo`: see below.
5. **Play.**

## Optimise for this PC

A checkbox under Install, on by default and remembered per folder (`work\setup-no-pgo` when it was off;
`--no-pgo` starts with it off). It makes the game faster where the CPU sets the pace, with profile-guided
optimisation (PGO) trained **on the player's own PC**: the game is compiled once with counters, plays by itself
for a few minutes, and is compiled again with what the counters saw. A profile comes from running the game,
so it is never published (`tools\publish_export.ps1` refuses `*.profdata`, `*.profraw` and `work\`): every
player makes their own. On the test PC (i5-13600K) a clean install took 33 minutes with it and 17 without:
the training itself is 6.5 minutes, the rest is a second compile. The player build got 5-9% more fps at the
cathedral spire (a city view the training never visits) on the full CPU, and 6-11% with the game held to 4
cores / 8 threads. `setup_build.ps1 -Pgo` has 12 steps instead of 10:

1. **Step 8:** the game is translated, then compiled as a *training build* (`rebuild_win.py`, `PORT_PGO=gen`,
   into `work\port\build-train`, log `work\logs\compile-build-train.log`).
2. **Step 9, training** (`tools\pgo_train.ps1`). The training build opens in a window and plays by itself:
   the sound is off, a scripted controller plays, the player's own controller, keyboard and mouse do nothing
   in it, and the window title says what is going on. The status line reads "Training the build on your
   PC... 40%"; the line under it says how long it takes and not to touch it. Two runs:
   - **A new game** (always): the title (START until the opening montage starts), the montage and the opening
     cutscenes, the tutorial and the fight on the docks with `tools\fast_combat.steps`' inputs, then one more
     looser round of the same kinds of input. About 6 minutes.
   - **The newest story save**, when there is one: about 100 s of looking around, walking, jumping and
     fighting wherever it is. About 3 minutes.

   Each run plays on its **own copy** of the saves (`work\pgo-setup\runs\<run>\gamedata`, deleted after the
   run) and of the settings file (`%APPDATA%\inFAMOUS2-port\settings.ini`, a portable folder's, or the
   caller's `PS3_OVERLAY_INI`); the player's files are only read. It uses the game's own shader cache, so the
   training also warms it for play.
3. **Step 10:** the runs' counters are merged (`llvm-profdata merge`) into
   `work\pgo-setup\setup-<time>.profdata` (a new name every time: ninja rebuilds nothing when only the content
   of a profile at the same path changes), and the game is compiled with it (`PORT_PGO=use:<profile>`).

**Nothing in the training can miss.** It never looks at the screen: the game is driven from its own log lines
(the menu, the montage, the city-falls cutscene) and its own first world frame (the save). When the city-falls
line never comes, the scripted inputs start anyway 300 s after the montage, and the looser second round
answers a tutorial prompt the first round met too early. The game writes its counters every 40 s and each
complete write is set aside at once, so a run cut short (a crash, the window closed) still counts. Every run
that reached the game is merged, however far it got. The one run that is **not** merged is one that never got
past the title screen: all gameplay code would have zero counts in it, and clang compiles zero-count code as
cold (for size, not inlined), so that game would be *slower* than the normal build.

**PGO never makes Setup fail.** No `llvm-profdata.exe`, the training build does not compile, no run reached
the game, the merge fails, or the optimised compile fails: step 10 compiles the normal way, the log says
`  pgo: not used: <why>`, and the Play screen reads "Ready to play. Optimise for this PC didn't work this time
(<why>), so the game was built the normal way." When it worked: "Ready to play, optimised for this PC."

**Updates** (Install over an install, "Update to the latest version", F2 > Update): the profile stays
(`work\pgo-setup`, about 30 MB, kept by both cleanups) and step 8 compiles the new version with it. Clang checks
every function against the profile: unchanged functions get their counts, a changed one compiles without
them, so a profile is only ever used where it still fits. When `rebuild_win.py` then reports that hot code
changed since the training (its "retrain" hint), or this LLVM can't read the profile, Setup trains again
(steps 8-10; the game just built with the old profile stays if the new training does not work). Otherwise
steps 9 and 10 are skipped and the update stays quick.

Test switches (environment): `PORT_PGO_TRAIN=opening|save|opening,save` picks the runs;
`PORT_PGO_TEST=fail:llvm|fail:trainbuild|fail:compile` fails that stage, `stale` treats a kept profile as out of
date, `stopafter:<s>` closes the game window after `<s>` s of each run (as a player closing it would);
`PORT_PGO_KEEP_TRAIN=1` keeps the training build. `tools\pgo_train.ps1 -StopAfter <s>` does the same on its own.

**Updating:** run the newest `Setup.exe`, either in the game folder or next to it, and press Install
(or "Update to the latest version" under Play). On a folder that already has the port, Install:

1. **Backs up** the saves (`game\gamedata\dev_hdd0\home\00000001\savedata`), `input.toml`,
   `config.local.json` and the game as it is (exe + `SDL2.dll`). They go to
   `%LOCALAPPDATA%\inFAMOUS2-port\save-backups\<time>\`, not `%TEMP%`, and the newest 3 are kept.
2. **Gets the newest code.** It asks GitHub (1 API call) and downloads only if the port commit differs
   from `port-version.txt`.
   - The port's own files are overwritten in place and nothing is deleted. `game\`, `work\`, `play\`
     and the other player and build folders are never written, and nothing is written through a
     link (`game\disc` is a junction to the player's disc).
   - `engine\` and `base\` are replaced whole (a file deleted upstream must not stay behind: the
     engine's CMake builds every file in its folders): set aside first, and never when a link is
     inside them.
   - An install from before v0.5.0: its `toolkit\` and `community\erikomis-infamous2-recomp\`
     (the engine and the port base then) are deleted after the download when an earlier Setup.exe
     made them (their `.zip-source` mark), never with a link inside.
   - A git clone is left to `git pull`.
3. **Builds** with the normal flow.
4. **Restores, built or not.** If the new game was not built, the old exe and `SDL2.dll` go back, so
   the player can always play. The saves are copied back: missing ones are added and older ones
   overwritten, but a save newer than the backup stays.

Pressing Install accepts Microsoft's license for the C++ files; the line under the button says so and
links to it. **Details** (bottom left) shows the log, which is also written to `work\setup.log`. After a
failure, **Copy log** copies it for a bug report.

## Build it

Run `tools\setup_exe\build.cmd` (double-click works). It uses the C# compiler that ships with
Windows (`%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe`, .NET Framework 4.5+) and writes
`Setup.exe` to the repo root, with `tools\setup.ico` as its icon. That compiler is C# 5: no `$""`, no
`=>` members, no `?.` and no `nameof`. The exe carries full version information (the `[assembly: ...]` lines
at the top of `SetupWizard.cs`: keep the version in step with the release) and the manifest
`setup.manifest` (runs as the player, never as admin; Windows 10/11; DPI aware). It is not packed or
obfuscated.

Setup runs the port's scripts with `powershell -NoProfile -ExecutionPolicy RemoteSigned -File <script>`
(no inline `-Command` code): `tools\setup_checks.ps1` for the checks, `tools\setup_build.ps1` for the build
and `tools\play.ps1` for Play. The policy is this process's only, so it also works on a PC left at Windows'
default (Restricted). The files Setup downloads and unpacks itself carry no "downloaded from the internet"
mark. A release ZIP downloaded with a browser and unpacked with Explorer does carry the mark, which
RemoteSigned refuses: Setup then stops at the checks and says how to unblock the ZIP (Properties > Unblock),
exit code 83 under `--prepare`.

`Setup.exe` is not committed: it is attached to a GitHub Release (see the next section).

GitHub Actions builds it the same way: `.github/workflows/setup-exe.yml` runs `build.cmd` on GitHub's own Windows
machine for every change to these files and for every release, and keeps the exe as the run's artifact.

## Releases and signing

Release Setup.exe files are signed by [SignPath Foundation](https://signpath.org) (free for open source; the
README's *Code signing policy*). SignPath signs only the exe the workflow built from the release's tag, and only
after the approver has approved it. Its settings are the repository's `SIGNPATH_*` secret and variables (listed at
the top of the workflow). Without them the workflow only builds, and releases are made as before.

1. Set the version at the top of `SetupWizard.cs` (the three `[assembly: ...Version]` lines) and in
   `setup.manifest`. Commit and push `main`.
2. Publish the release on GitHub (tag `v<version>`) without Setup.exe. The release notes link the README's
   *Code signing policy* (SignPath asks for that term on the download page).
3. The workflow builds Setup.exe from the tag and sends it to SignPath. SignPath e-mails the approver: open the
   request on app.signpath.io and approve it (the workflow waits up to an hour).
4. The workflow checks the signature (Windows must accept it) and attaches the signed Setup.exe to the release.
   If the approval came too late: Actions > Setup.exe > Run workflow, pick the tag.
5. The zip, with that same signed exe:

   ```
   gh release download v<version> -R SamiEnazi/infamous2-port -p Setup.exe -D work\release\signed
   tools\make_release_zip.ps1 -Version <version> -SetupExe work\release\signed\Setup.exe -RequireSigned
   gh release upload v<version> -R SamiEnazi/infamous2-port work\release\inFAMOUS2-port-v<version>-Windows-x64.zip
   ```

To publish everything at once instead: push the tag, create the release as a draft, run the workflow on the tag
(Run workflow, pick the tag), approve, attach the zip, then publish. The workflow then finds the signed
Setup.exe on the release and does not sign it again.

SignPath's `test-signing` policy (the first one SignPath sets up) signs with a certificate Windows does not
trust: the workflow keeps that exe as the run's artifact `Setup.exe-signed` and attaches nothing.

## Test switches

| Switch | What it does |
|---|---|
| `--test-build` | press Install right away |
| `--rpcs3 <folder>`, `--disc <folder>` (or `--game <folder>`) | use these instead of looking |
| `--folder <folder>` | where a downloaded-alone Setup.exe puts the port |
| `--no-shortcut` | pass `-NoShortcut` (no desktop shortcut) |
| `--no-cleanup` | keep the build tools and files afterwards |
| `--only-cache` | the build sees only `tools-cache\`: `PORT_ONLY_CACHE=1` and a PATH of Windows folders only |
| `--no-pgo` | start with "Optimise for this PC" off (no `-Pgo`) |
| `--exit-when-done` | close after the build. The exit code is 0 for built, 1 for stopped and 2 for closed early. |
| `--page done` / `--page ask` | show the Play screen / the "Where is RPCS3?" dialog (for screenshots) |
| `--test-fail-build` | make the build fail on purpose at step 2 (no license flag), to check that the old game and the saves come back |
| `PORT_SETUP_API`, `PORT_SETUP_CODELOAD` (environment) | a stand-in for `https://api.github.com` and `https://codeload.github.com` (e.g. `http://127.0.0.1:8765`), to test the download and the update with a local copy of a commit |

## RecompLauncher: `--prepare` and `--play`

[RecompLauncher](https://recomplauncher.com) installs the port from the release's
`inFAMOUS2-port-v<version>-Windows-x64.zip` (made by `tools\make_release_zip.ps1`: the port at the release's
commit, with `engine\` and `base\`, and `Setup.exe`, `port-version.txt` and `recomplauncher.json` at the top). The
repo's `recomplauncher.json` tells it to use Setup.exe in two ways, both without a window. Without these
switches Setup.exe is exactly as before.

- `Setup.exe --prepare --disc <game folder> [--rpcs3 <folder>]` (`--game` works as well as `--disc`) is the launcher's setup step, after the player
  picks their game folder. It does what Install does after the download:
  - finds RPCS3 the same way
  - runs the same checks
  - backs up an existing game and its saves
  - runs `tools\setup_build.ps1 -AcceptMsvcLicense -Cleanup -NoShortcut`. The launcher's setup screen says that
    going ahead accepts Microsoft's license. There is no "Optimise for this PC" unless `--pgo` is given: its
    training opens the game's window for a few minutes, which the launcher's setup step does not announce, and
    each launcher install is a new folder, so there is never an earlier training to reuse. With `--pgo` the build
    has 12 steps; the progress lines and exit codes are the same (a training that does not work never fails it).

  It prints `progress <0..1> <text>` lines, writes the whole log to `play\setup.log`, and after a build writes
  `work\launcher-built.txt` (a copy of `port-version.txt`).
- `Setup.exe --play [play.ps1's options]` is the launcher's Play button. It runs `tools\play.ps1` without a
  console window, and adds `-Fullscreen` when `RECOMPLAUNCHER_MODE` is `bigpicture`. play.ps1's lines go to
  `play\launcher.log`, followed by the last 150 lines of the session's `stderr.log`. It ends with the game's own
  exit code (0 for a normal quit). If the port was updated after the game was built, the log says so.
- `--dry-run` with either one does everything up to the build or the start, then stops. Nothing is built or
  started. Setup.exe has no console, so pipe it to see the lines: `Setup.exe --prepare --dry-run --disc <folder> | more`.

The updater (F2 > Update in the game, `tools\update_check.ps1`, which builds its own Setup.exe from
`tools\setup_exe` into `work\update\setup` with the same compiler as build.cmd):

- `Setup.exe --update --folder <port folder> [--to v0.5.1] [--disc <game folder>] [--rpcs3 <folder>]` opens the
  window and does at once what "Update to the latest version" does: saves backed up, the newest port over the
  folder, the build, the saves reapplied; the last line says "Updated to v0.5.1", then Play. One Setup per
  port folder (the lock is the folder's, wherever this Setup.exe is).
- `--update --no-window [--dry-run] [--pgo]`: the same without a window (tests, scripts), its log in `play\update.log`,
  `--prepare`'s progress lines and exit codes; `--dry-run` downloads and checks, then stops before the build.
  The game folder is `--disc`, else the `game\disc` link.
- An update keeps the time of every port, engine and port-base file that did not change, and gives a changed
  one the current time (a ZIP's times are the commit's), so the build files Setup keeps rebuild only what changed.

Both put themselves in a job object that Windows closes with them. When the launcher stops Setup.exe, the
build or the game stops too. Exit codes (`recomplauncher.json`'s `exit_codes` has the player's message for each):

| Code | Meaning |
|---|---|
| 69 | Setup is already running in this folder |
| 70 | the port's files are not next to Setup.exe |
| 71 | no `PS3_GAME\PARAM.SFO` in the game folder |
| 72 | not a supported copy of inFAMOUS 2 |
| 73 | RPCS3 not found |
| 74 | no PS3 firmware in RPCS3 |
| 75 | no 1.04 update in RPCS3 (or, for the digital version, no game) |
| 76 | RPCS3 is open |
| 77 | this PC can't run the port (64-bit Windows, AVX2, Vulkan) |
| 78 | a drive that can't hold the `game\disc` link |
| 79 | the checks or the build failed (`play\setup.log`) |
| 80 | `--play`: the game is not built yet |
| 82 | `--play`: play.ps1 could not run |
| 83 | Windows marked the port's scripts as downloaded from the internet (a ZIP unpacked without Unblock) |

## Known limits

- Until the releases are signed (see *Releases and signing*), Windows SmartScreen says "Windows protected your
  PC" on the first start. Click **More info**, then **Run anyway**. A few machine-learning virus scanners on
  VirusTotal flagged v0.4.1's unsigned exe (false positives).
- The port's folder must be on a local NTFS (or ReFS) drive, because `game\disc` is a junction to the
  player's game folder. FAT32/exFAT drives (USB sticks, many external disks) and network drives can't hold
  one; Windows then fails with only "Incorrect function" (GitHub issue #2). So a downloaded-alone
  Setup.exe on such a drive installs into `%LOCALAPPDATA%\inFAMOUS2-port` instead and says so in the log.
  The checks (`Get-SetupChecks`) stop with a plain reason when the folder was chosen by hand (`--folder`,
  or Setup.cmd in such a folder). A game folder on a network drive is refused the same way.
- The first build downloads about 1.3 GB of tools. They and the build files stay (about 2.6 GB) so an update
  builds only what changed; `tools\setup_cleanup.ps1` deletes them. `--prepare` (RecompLauncher) still deletes them.
- During "Optimise for this PC" the game window opens by itself for about 6 minutes (9 with a story save).
  Closing it ends that run early: what was counted is used when the run got past the title, else the game is
  built the normal way. An install from v0.5.1 or older kept an LLVM without `llvm-profdata.exe`: the first
  update with the box ticked downloads LLVM's archive once more (862 MB) and takes just that file from it.
- GitHub's API answers 60 questions an hour per network without a sign-in. Setup asks 3 per download.
- Only one Setup runs per folder at a time.
