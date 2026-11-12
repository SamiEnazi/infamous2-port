# The work behind Setup.exe and Setup.cmd (also usable on its own):
#   1. check this PC, the game and RPCS3                             (tools\setup_common.ps1)
#   2. the build tools, downloaded into tools-cache\ (no installs)    (tools\fetch_tools.ps1)
#   3. your game: the disc folder linked into game\disc; the 1.04 update, the firmware files and
#      the game's HDD install copied into game\ (so RPCS3 is not needed after Setup)
#   4. decrypt the 1.04 EBOOT with RPCS3                             -> eboot\EBOOT_upd104.elf
#   5. Python tools                                                  -> work\venv
#   6. SDL2, Vulkan headers, glslang                                 -> deps\
#   7. the exe's icon from the player's own ICON0.PNG                -> work\icon\app.ico
#   8. translate and compile the game                                -> work\port\build\Infamous2Recomp.exe
#   9. a desktop shortcut
#  10. with -Cleanup: delete what only this build needed (tools\setup_cleanup.ps1). The build tools and the
#      build files stay for the next update (about 2.6 GB): an update that changes a few files then downloads
#      no tools and rebuilds only what changed, in minutes. -NoBuildCache (or PORT_BUILD_CACHE=0: Setup.exe
#      --prepare, for RecompLauncher) deletes them too, as before.
#   tools\setup_build.ps1 -Disc <folder holding PS3_GAME> -Rpcs3 <RPCS3 folder> [-NoShortcut]
#                         [-AcceptMsvcLicense] [-Cleanup] [-NoBuildCache]
# -AcceptMsvcLicense: Microsoft's MSVC + Windows SDK files are under the Visual Studio Build Tools
# license (https://go.microsoft.com/fwlink/?LinkId=2179911); Setup asks first.
param(
    [Parameter(Mandatory = $true)][string]$Disc,
    [Parameter(Mandatory = $true)][string]$Rpcs3,
    [switch]$NoShortcut,
    [switch]$AcceptMsvcLicense,
    [switch]$Cleanup,
    [switch]$NoBuildCache
)
$ErrorActionPreference = "Stop"
# Setup.exe reads these lines through a pipe, in UTF-8 (a console keeps its own code page)
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.Encoding]::UTF8 }
trap { "== FAILED: $_"; exit 1 }   # the reason lands in the log Setup shows
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "setup_common.ps1")
$sw = [Diagnostics.Stopwatch]::StartNew()
function Step([int]$n, [string]$text) { "== [$n/10] $text   ($([int]$sw.Elapsed.TotalMinutes) min so far)" }
# Native tools (git, pip, cmd, python, robocopy) write progress and warnings to stderr; under
# "Stop" with the output redirected to Setup's log, Windows PowerShell would turn the first such
# line into a failure. Run them under "Continue", keep their lines, judge them by exit code.
function Invoke-Native([scriptblock]$Cmd) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $Cmd 2>&1 | ForEach-Object { "    $_" } } finally { $ErrorActionPreference = $eap }
}
function Copy-Tree([string]$Src, [string]$Dst, [string]$What) {
    if (Test-Path $Dst) {
        $i = Get-Item $Dst -Force
        if ($i.LinkType -eq "Junction") { cmd /c rmdir "$Dst" | Out-Null }   # an older Setup linked it
    }
    "  copying $What"
    Invoke-Native { robocopy $Src $Dst /E /NFL /NDL /NJH /NJS /NP } | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copying $Src failed (robocopy $LASTEXITCODE)" }
}
$Disc = (Resolve-Path $Disc).Path.TrimEnd('\'); $Rpcs3 = (Resolve-Path $Rpcs3).Path.TrimEnd('\')
# The disc folder given as a junction (e.g. this port's own game\disc from an earlier Setup): use
# the folder it points to, or step 3 would replace the link with a link to itself.
$di = Get-Item -LiteralPath $Disc -Force
if ($di.LinkType -eq "Junction" -and @($di.Target)[0]) { $Disc = ([string]@($di.Target)[0]).TrimEnd('\') }

Step 1 "checking"
# The engine (engine\) and the port base (base\) are folders of this repository since v0.5.0 (no
# submodules any more). A git clone has them with LF line ends too (.gitattributes: the patches in
# patches\port are LF, as base\'s files are).
foreach ($sub in @(@("engine", "CMakeLists.txt"), @("base", "infamous2-lle\CMakeLists.txt"))) {
    if (-not (Test-Path (Join-Path (Join-Path $root $sub[0]) $sub[1]))) {
        throw "$($sub[0])\$($sub[1]) is missing: download the port again with Setup.exe (or 'git clone')"
    }
    "  $($sub[0]): ok"
}
# A Setup.exe of v0.4.2 or older updates by copying the new version over the old one, never deleting, and
# it still downloads toolkit\ and community\ (the engine and the port base before v0.5.0: nothing builds them
# now). A file the new engine\ or base\ no longer has would still be built (the engine's CMake builds every
# file it finds), so what they hold beyond the version's own list (.port-files, from the public repository)
# is deleted here. Not in a git clone (git keeps them), never through a link.
$portFiles = Join-Path $root ".port-files"
if ((Test-Path -LiteralPath $portFiles) -and -not (Test-Path -LiteralPath (Join-Path $root ".git"))) {
    $want = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($l in [IO.File]::ReadAllLines($portFiles)) { if ($l.Trim()) { [void]$want.Add($l.Trim().Replace('/', '\')) } }
    $stale = New-Object System.Collections.Generic.List[string]
    foreach ($top in "engine", "base") {
        $stack = New-Object System.Collections.Stack
        $stack.Push((Get-Item -LiteralPath (Join-Path $root $top) -Force))
        while ($stack.Count) {
            $d = $stack.Pop()
            foreach ($e in $d.GetFileSystemInfos()) {
                if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
                if ($e -is [IO.DirectoryInfo]) { if ($e.Name -ne "__pycache__") { $stack.Push($e) }; continue }   # (Python's own cache)
                $rel = $e.FullName.Substring($root.Length + 1)
                if (-not $want.Contains($rel)) { $stale.Add($e.FullName) }
            }
        }
    }
    if ($want.Count -lt 100) { "  note: .port-files lists only $($want.Count) files: nothing deleted" }
    elseif ($stale.Count) {
        foreach ($f in $stale) { Remove-Item -LiteralPath $f -Force }
        "  engine\ and base\: $($stale.Count) file(s) from an earlier version deleted (this version has no such files)"
    }
}
$rows = Get-SetupChecks -Disc $Disc -Rpcs3 $Rpcs3
foreach ($r in $rows) { "  {0,-5} {1}: {2}" -f $(if ($r.Ok -eq $true) { "ok" } elseif ($r.Ok -eq $false) { "MISS" } else { "note" }), $r.Name, $r.Detail }
if ($rows | Where-Object { $_.Ok -eq $false }) { throw "fix the MISS items first" }
$title = (Read-Sfo (Join-Path $Disc "PS3_GAME\PARAM.SFO"))["TITLE_ID"]
$cfgPath = Join-Path $root "config.local.json"
$cfg = [ordered]@{}
if (Test-Path $cfgPath) { (Get-Content $cfgPath -Raw | ConvertFrom-Json).psobject.Properties | ForEach-Object { $cfg[$_.Name] = $_.Value } }
$cfg["rpcs3"] = $Rpcs3
[System.IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json))

Step 2 "the build tools (a temporary download into tools-cache\, nothing is installed)"
& (Join-Path $PSScriptRoot "fetch_tools.ps1") -AcceptMsvcLicense:$AcceptMsvcLicense
$py = Get-PortTool python
if (-not $py) { throw "no Python after getting the build tools" }

Step 3 "your game: the disc linked, the update, firmware files and HDD install copied into game\"
$game = Join-Path $root "game"
New-Item -ItemType Directory -Force (Join-Path $game "dev_hdd0\game"), (Join-Path $game "gamedata") | Out-Null
$discLink = Join-Path $game "disc"
if (Test-Path $discLink) {
    $i = Get-Item $discLink -Force
    if ($i.LinkType -eq "Junction" -and -not (@($i.Target) -contains $Disc)) { cmd /c rmdir "$discLink" | Out-Null }
}
if (-not (Test-Path $discLink)) {
    # GitHub issue #2: on FAT32/exFAT or a network drive Windows says only "Incorrect function"
    try { New-Item -ItemType Junction -Path $discLink -Target $Disc -ErrorAction Stop | Out-Null }
    catch {
        throw ("could not link your game folder into the port ($($_.Exception.Message)). The port's folder must be on a " +
               "local NTFS drive and your game folder on a drive in this PC: put Setup.exe in a folder on your internal drive, " +
               "for example C:\Games\inFAMOUS2, and run it again")
    }
}
"  $discLink -> $Disc (keep your disc folder where it is)"
Copy-Tree (Join-Path $Rpcs3 "dev_hdd0\game\$title") (Join-Path $game "dev_hdd0\game\$title") "the 1.04 update (0.2 GB)"
Copy-Tree (Join-Path $Rpcs3 "dev_flash") (Join-Path $game "dev_flash") "the PS3 firmware files the game reads (0.2 GB)"
# The game copies 2.4 GB to the HDD on its first boot (<title>_INSTALL): RPCS3's copy if there is
# one, else the port's first boot does the install itself.
$inst = Join-Path $Rpcs3 "dev_hdd0\game\${title}_INSTALL"
$instDst = Join-Path $game "dev_hdd0\game\${title}_INSTALL"
if ((Test-Path $inst) -and -not (Test-Path $instDst)) { Copy-Tree $inst $instDst "the game's HDD install (2.4 GB)" }
elseif (-not (Test-Path $instDst)) { "  note: no ${title}_INSTALL in RPCS3 -- the port's first boot installs it (if that fails: start the game once in RPCS3)" }

Step 4 "decrypting the game's 1.04 EBOOT with RPCS3"
$eb = Join-Path $root "eboot"
New-Item -ItemType Directory -Force $eb | Out-Null
$elf = Join-Path $eb "EBOOT_upd104.elf"
if (-not (Test-Path $elf)) {
    if (Get-Process rpcs3 -ErrorAction SilentlyContinue) { throw "close RPCS3 first (it decrypts one file per start)" }
    $bin = Join-Path $eb "EBOOT_upd104.BIN"
    Copy-Item (Join-Path $Rpcs3 "dev_hdd0\game\$title\USRDIR\EBOOT.BIN") $bin -Force
    Start-Process -FilePath (Join-Path $Rpcs3 "rpcs3.exe") -ArgumentList "--decrypt", "`"$bin`"" -Wait -WindowStyle Hidden
    if (-not (Test-Path $elf)) { throw "RPCS3 did not write $elf (is the firmware installed in RPCS3?)" }
}
"  $elf"

Step 5 "Python tools (work\venv)"
$venvPy = Join-Path $root "work\venv\Scripts\python.exe"
if (Test-Path $venvPy) {
    # a venv whose base Python was deleted (an earlier cleanup) does not start, and one a kept build made with
    # another Python version has its packages in the wrong place: make it again
    $pyMm = "$(Invoke-Native { & $py -c "import sys; print('%d.%d' % sys.version_info[:2])" } | Select-Object -Last 1)".Trim()
    $cfgLine = Select-String -LiteralPath (Join-Path $root "work\venv\pyvenv.cfg") -Pattern '^\s*version\s*=\s*(\d+\.\d+)' -ErrorAction SilentlyContinue |
               Select-Object -First 1
    $venvMm = if ($cfgLine) { $cfgLine.Matches[0].Groups[1].Value } else { "" }
    Invoke-Native { & $venvPy -c "import sys" } | Out-Null
    if ($LASTEXITCODE -or ($pyMm -match '^\d+\.\d+$' -and $venvMm -and $venvMm -ne $pyMm)) { cmd /c rd /s /q (Join-Path $root "work\venv") | Out-Null }
}
if (-not (Test-Path $venvPy)) {
    Invoke-Native { & $py -m venv (Join-Path $root "work\venv") }
    if ($LASTEXITCODE) { throw "python -m venv failed" }
}
Invoke-Native { & $venvPy -m pip install --disable-pip-version-check -q -r (Join-Path $root "engine\requirements.txt") }
if ($LASTEXITCODE) { throw "pip install failed" }

Step 6 "SDL2, Vulkan headers and glslang (download, then build glslang: a few minutes)"
& (Join-Path $PSScriptRoot "fetch_deps.ps1")
if (-not (Test-Path (Join-Path $root "deps\SDL2-2.30.10\cmake"))) { throw "fetching the libraries failed (see above)" }
# built again when its sources are another version than the kept build (deps\glslang-install\.source)
$gSrc = Join-Path $root "deps\glslang-src\.source"
$gMark = Join-Path $root "deps\glslang-install\.source"
$gWant = if (Test-Path $gSrc) { (Get-Content $gSrc -Raw).Trim() } else { "" }
$gHave = if (Test-Path $gMark) { (Get-Content $gMark -Raw).Trim() } else { "" }
if (-not (Test-Path (Join-Path $root "deps\glslang-install\lib\cmake\glslang")) -or ($gWant -and $gHave -and $gWant -ne $gHave)) {
    Invoke-Native { cmd /c "`"$(Join-Path $PSScriptRoot 'build_deps.cmd')`"" }
    if ($LASTEXITCODE) { throw "building glslang failed" }
    if ($gWant) { Set-Content -Path $gMark -Value $gWant }
}

Step 7 "the exe's icon (from your copy's ICON0.PNG)"
try { & (Join-Path $PSScriptRoot "make_icon.ps1") -Icon0 (Join-Path $Disc "PS3_GAME\ICON0.PNG") } catch { "  no icon: $_ (the game still builds)" }

Step 8 "translating and compiling the game (the long part: 15-40 minutes)"
# With the build files of an earlier build (an update): a translation step whose inputs did not change is
# skipped, and only what changed is compiled again (tools\rebuild_win.py, PORT_LIFT_REUSE)
$env:PORT_LIFT_REUSE = "1"
Invoke-Native { & $venvPy -I (Join-Path $PSScriptRoot "rebuild_win.py") }
if ($LASTEXITCODE) { throw "the build failed: see work\logs\ (python -I tools\build_errors.py work\logs\compile.log)" }
$exe = Join-Path $root "work\port\build\Infamous2Recomp.exe"
if (-not (Test-Path $exe)) { throw "no $exe after the build" }

Step 9 "desktop shortcut"
if (-not $NoShortcut) {
    $ws = New-Object -ComObject WScript.Shell
    $lnkPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "inFAMOUS 2 (PC port).lnk"
    $lnk = $ws.CreateShortcut($lnkPath)
    $lnk.TargetPath = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
    $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'play.ps1')`""
    $lnk.WorkingDirectory = $root
    $lnk.IconLocation = "$exe,0"
    $lnk.WindowStyle = 7
    $lnk.Description = "inFAMOUS 2, natively on Windows (your own copy, recompiled)"
    $lnk.Save()
    "  $lnkPath"
} else { "  skipped (-NoShortcut)" }

Step 10 "cleaning up"
if ($Cleanup) { & (Join-Path $PSScriptRoot "setup_cleanup.ps1") -KeepBuildCache:(-not $NoBuildCache -and $env:PORT_BUILD_CACHE -ne "0") }
else { "  kept the build tools and files (no -Cleanup): tools\setup_cleanup.ps1 deletes them" }
"== done in $([int]$sw.Elapsed.TotalMinutes) min: play with the desktop shortcut or 'Play inFAMOUS 2.cmd'. RPCS3 is not needed to play (an update reads the game's update and firmware from it again: keep it)."
exit 0
