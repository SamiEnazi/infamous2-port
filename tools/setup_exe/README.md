# Setup.exe: one Install button

`Setup.exe` is the beginner's way in: one window, one big **Install** button, one progress bar, then
**Play**. It wraps the repo's own PowerShell setup and does not redo it.

What Install does, in order. It asks nothing it can find out itself:

1. **The port's files.** If Setup.exe was downloaded on its own (no `tools\setup_common.ps1` beside it), it
   downloads the port from GitHub as ZIP files, with no git:
   - the port at the newest commit of `main`
   - the toolkit and the port base at the commits the port pins (GitHub's API names them)

   They go into a folder next to Setup.exe (else `%LOCALAPPDATA%\inFAMOUS2-port`), about 40 MB. Then
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
5. **Play.**

**Updating:** run the newest `Setup.exe`, either in the game folder or next to it, and press Install
(or "Update to the latest version" under Play). On a folder that already has the port, Install:

1. **Backs up** the saves (`game\gamedata\dev_hdd0\home\00000001\savedata`), `input.toml`,
   `config.local.json` and the game as it is (exe + `SDL2.dll`). They go to
   `%LOCALAPPDATA%\inFAMOUS2-port\save-backups\<time>\`, not `%TEMP%`, and the newest 3 are kept.
2. **Gets the newest code.** It asks GitHub (3 API calls) and downloads only if the port commit or a pin
   differs from `port-version.txt`.
   - The port's own files are overwritten in place and nothing is deleted. `game\`, `work\`, `play\`
     and the other player and build folders are never written, and nothing is written through a
     link (`game\disc` is a junction to the player's disc).
   - `toolkit\` and `community\erikomis-infamous2-recomp\` are replaced whole: set aside first, and
     never when a link is inside them.
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
`=>` members, no `?.` and no `nameof`.

`Setup.exe` is not committed: it is attached to a GitHub Release.

## Test switches

| Switch | What it does |
|---|---|
| `--test-build` | press Install right away |
| `--rpcs3 <folder>`, `--disc <folder>` | use these instead of looking |
| `--folder <folder>` | where a downloaded-alone Setup.exe puts the port |
| `--no-shortcut` | pass `-NoShortcut` (no desktop shortcut) |
| `--no-cleanup` | keep the build tools and files afterwards |
| `--only-cache` | the build sees only `tools-cache\`: `PORT_ONLY_CACHE=1` and a PATH of Windows folders only |
| `--exit-when-done` | close after the build. The exit code is 0 for built, 1 for stopped and 2 for closed early. |
| `--page done` / `--page ask` | show the Play screen / the "Where is RPCS3?" dialog (for screenshots) |
| `--test-fail-build` | make the build fail on purpose at step 2 (no license flag), to check that the old game and the saves come back |

## RecompLauncher: `--prepare` and `--play`

[RecompLauncher](https://recomplauncher.com) installs the port from the release's
`inFAMOUS2-port-v<version>-Windows-x64.zip` (made by `tools\make_release_zip.ps1`: the port, the toolkit and the
port base at the release's commits, `Setup.exe`, `port-version.txt` and `recomplauncher.json` at the top). The
repo's `recomplauncher.json` tells it to use Setup.exe in two ways, both without a window. Without these
switches Setup.exe is exactly as before.

- `Setup.exe --prepare --disc <game folder> [--rpcs3 <folder>]` is the launcher's setup step, after the player
  picks their game folder. It does what Install does after the download:
  - finds RPCS3 the same way
  - runs the same checks
  - backs up an existing game and its saves
  - runs `tools\setup_build.ps1 -AcceptMsvcLicense -Cleanup -NoShortcut`. The launcher's setup screen says that
    going ahead accepts Microsoft's license. There is no "Optimise for this PC": its training run opens the
    game's window, which a setup step must not.

  It prints `progress <0..1> <text>` lines, writes the whole log to `play\setup.log`, and after a build writes
  `work\launcher-built.txt` (a copy of `port-version.txt`).
- `Setup.exe --play [play.ps1's options]` is the launcher's Play button. It runs `tools\play.ps1` without a
  console window, and adds `-Fullscreen` when `RECOMPLAUNCHER_MODE` is `bigpicture`. play.ps1's lines go to
  `play\launcher.log`, followed by the last 150 lines of the session's `stderr.log`. It ends with the game's own
  exit code (0 for a normal quit). If the port was updated after the game was built, the log says so.
- `--dry-run` with either one does everything up to the build or the start, then stops. Nothing is built or
  started. Setup.exe has no console, so pipe it to see the lines: `Setup.exe --prepare --dry-run --disc <folder> | more`.

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

## Known limits

- The exe is not signed, so Windows SmartScreen says "Windows protected your PC" on the first start.
  Click **More info**, then **Run anyway**.
- The port's folder must be on a local NTFS (or ReFS) drive, because `game\disc` is a junction to the
  player's game folder. FAT32/exFAT drives (USB sticks, many external disks) and network drives can't hold
  one; Windows then fails with only "Incorrect function" (GitHub issue #2). So a downloaded-alone
  Setup.exe on such a drive installs into `%LOCALAPPDATA%\inFAMOUS2-port` instead and says so in the log.
  The checks (`Get-SetupChecks`) stop with a plain reason when the folder was chosen by hand (`--folder`,
  or Setup.cmd in such a folder). A game folder on a network drive is refused the same way.
- The first build downloads about 1.3 GB of tools, and an update downloads them again.
- GitHub's API answers 60 questions an hour per network without a sign-in. Setup asks 3 per download.
- Only one Setup runs per folder at a time.
