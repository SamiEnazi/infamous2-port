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
#  10. with -Cleanup: delete the build tools and everything only the build needed (tools\setup_cleanup.ps1)
#   tools\setup_build.ps1 -Disc <folder holding PS3_GAME> -Rpcs3 <RPCS3 folder> [-NoShortcut]
#                         [-AcceptMsvcLicense] [-Cleanup]
# -AcceptMsvcLicense: Microsoft's MSVC + Windows SDK files are under the Visual Studio Build Tools
# license (https://go.microsoft.com/fwlink/?LinkId=2179911); Setup asks first.
param(
    [Parameter(Mandatory = $true)][string]$Disc,
    [Parameter(Mandatory = $true)][string]$Rpcs3,
    [switch]$NoShortcut,
    [switch]$AcceptMsvcLicense,
    [switch]$Cleanup
)
$ErrorActionPreference = "Stop"
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
# The toolkit and the port base: plain folders when the port came as ZIP files (Setup.exe), or git
# submodules in a git clone. A git clone's submodules are checked out with LF line ends: our patches
# are LF, and Git for Windows' default (core.autocrlf=true) would check them out as CRLF. GitHub's
# ZIP files already have the repos' own LF line ends.
foreach ($sub in @(@("toolkit", "CMakeLists.txt"), @("community\erikomis-infamous2-recomp", "infamous2-lle\CMakeLists.txt"))) {
    $p = Join-Path $root $sub[0]
    $isGit = Test-Path (Join-Path $p ".git")
    if (-not $isGit -and (Test-Path (Join-Path $p $sub[1]))) { "  $($sub[0]): ok"; continue }
    if (-not $isGit) {
        if (-not (Test-Path (Join-Path $root ".git"))) { throw "$($sub[0]) is missing: download the port again with Setup.exe (or 'git clone --recursive')" }
        Invoke-Native { git -C $root submodule update --init -- ($sub[0] -replace '\\', '/') }
        if ($LASTEXITCODE) { throw "git submodule update --init $($sub[0]) failed" }
    }
    if ((Invoke-Native { git -C $p config --get core.autocrlf }) -notmatch 'false') {
        Invoke-Native { git -C $p config core.autocrlf false }
        Invoke-Native { git -C $p rm -rq --cached . } | Out-Null
        Invoke-Native { git -C $p reset -q --hard }
        "  $($sub[0]) checked out with LF line ends"
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
if (-not (Test-Path $discLink)) { New-Item -ItemType Junction -Path $discLink -Target $Disc | Out-Null }
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
    # a venv whose base Python was deleted (an earlier cleanup) does not start: make it again
    Invoke-Native { & $venvPy -c "import sys" } | Out-Null
    if ($LASTEXITCODE) { cmd /c rd /s /q (Join-Path $root "work\venv") | Out-Null }
}
if (-not (Test-Path $venvPy)) {
    Invoke-Native { & $py -m venv (Join-Path $root "work\venv") }
    if ($LASTEXITCODE) { throw "python -m venv failed" }
}
Invoke-Native { & $venvPy -m pip install --disable-pip-version-check -q -r (Join-Path $root "toolkit\requirements.txt") }
if ($LASTEXITCODE) { throw "pip install failed" }

Step 6 "SDL2, Vulkan headers and glslang (download, then build glslang: a few minutes)"
& (Join-Path $PSScriptRoot "fetch_deps.ps1")
if (-not (Test-Path (Join-Path $root "deps\SDL2-2.30.10\cmake"))) { throw "fetching the libraries failed (see above)" }
if (-not (Test-Path (Join-Path $root "deps\glslang-install\lib\cmake\glslang"))) {
    Invoke-Native { cmd /c "`"$(Join-Path $PSScriptRoot 'build_deps.cmd')`"" }
    if ($LASTEXITCODE) { throw "building glslang failed" }
}

Step 7 "the exe's icon (from your copy's ICON0.PNG)"
try { & (Join-Path $PSScriptRoot "make_icon.ps1") -Icon0 (Join-Path $Disc "PS3_GAME\ICON0.PNG") } catch { "  no icon: $_ (the game still builds)" }

Step 8 "translating and compiling the game (the long part: 15-40 minutes)"
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
if ($Cleanup) { & (Join-Path $PSScriptRoot "setup_cleanup.ps1") }
else { "  kept the build tools and files (no -Cleanup): tools\setup_cleanup.ps1 deletes them" }
"== done in $([int]$sw.Elapsed.TotalMinutes) min: play with the desktop shortcut or 'Play inFAMOUS 2.cmd'. RPCS3 is no longer needed."
exit 0
