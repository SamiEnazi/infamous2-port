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

## Known limits

- The exe is not signed, so Windows SmartScreen says "Windows protected your PC" on the first start.
  Click **More info**, then **Run anyway**.
- The first build downloads about 1.3 GB of tools, and an update downloads them again.
- GitHub's API answers 60 questions an hour per network without a sign-in. Setup asks 3 per download.
- Only one Setup runs per folder at a time.
