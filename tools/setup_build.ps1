# The work behind Setup.cmd (also usable on its own):
#   1. check this PC, the tools, the game and RPCS3            (tools\setup_common.ps1)
#   2. link the player's game into game\ (nothing big is copied, except the 2.4 GB HDD install)
#   3. decrypt the 1.04 EBOOT with the player's RPCS3          -> eboot\EBOOT_upd104.elf
#   4. Python tools                                            -> work\venv
#   5. SDL2, Vulkan headers, glslang                           -> deps\
#   6. the exe's icon from the player's own ICON0.PNG          -> work\icon\app.ico
#   7. translate and compile the game                          -> work\port\build\Infamous2Recomp.exe
#   8. a desktop shortcut
#   tools\setup_build.ps1 -Disc <folder holding PS3_GAME> -Rpcs3 <RPCS3 folder> [-NoShortcut]
param(
    [Parameter(Mandatory = $true)][string]$Disc,
    [Parameter(Mandatory = $true)][string]$Rpcs3,
    [switch]$NoShortcut
)
$ErrorActionPreference = "Stop"
trap { "== FAILED: $_"; exit 1 }   # the reason lands in the log Setup shows
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "setup_common.ps1")
$sw = [Diagnostics.Stopwatch]::StartNew()
function Step([int]$n, [string]$text) { "== [$n/8] $text   ($([int]$sw.Elapsed.TotalMinutes) min so far)" }
$Disc = (Resolve-Path $Disc).Path.TrimEnd('\'); $Rpcs3 = (Resolve-Path $Rpcs3).Path.TrimEnd('\')

Step 1 "checking"
# The two submodules (the toolkit, the port base), checked out with LF line ends: our patches
# are LF, and Git for Windows' default (core.autocrlf=true) would check them out as CRLF.
foreach ($sub in "toolkit", "community\erikomis-infamous2-recomp") {
    $p = Join-Path $root $sub
    if (-not (Test-Path (Join-Path $p ".git"))) {
        if (-not (Test-Path (Join-Path $root ".git"))) { throw "$sub is missing: get the source with 'git clone --recursive' (the ZIP download lacks the submodules)" }
        & git -C $root submodule update --init -- ($sub -replace '\\', '/')
        if ($LASTEXITCODE) { throw "git submodule update --init $sub failed" }
    }
    if ((& git -C $p config --get core.autocrlf) -ne "false") {
        & git -C $p config core.autocrlf false
        & git -C $p rm -rq --cached . ; & git -C $p reset -q --hard
        "  $sub checked out with LF line ends"
    }
}
$rows = Get-SetupChecks -Disc $Disc -Rpcs3 $Rpcs3
foreach ($r in $rows) { "  {0,-5} {1}: {2}" -f $(if ($r.Ok -eq $true) { "ok" } elseif ($r.Ok -eq $false) { "MISS" } else { "note" }), $r.Name, $r.Detail }
if ($rows | Where-Object { $_.Ok -eq $false }) { throw "fix the MISS items first (Setup's 'Install missing tools' gets the tools)" }
$title = (Read-Sfo (Join-Path $Disc "PS3_GAME\PARAM.SFO"))["TITLE_ID"]
$cfgPath = Join-Path $root "config.local.json"
$cfg = [ordered]@{}
if (Test-Path $cfgPath) { (Get-Content $cfgPath -Raw | ConvertFrom-Json).psobject.Properties | ForEach-Object { $cfg[$_.Name] = $_.Value } }
$cfg["rpcs3"] = $Rpcs3
[System.IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json))

Step 2 "linking your game into game\"
$game = Join-Path $root "game"
New-Item -ItemType Directory -Force (Join-Path $game "dev_hdd0\game"), (Join-Path $game "gamedata") | Out-Null
function Set-Link([string]$Link, [string]$Target) {
    if (Test-Path $Link) {
        $i = Get-Item $Link -Force
        if ($i.LinkType -ne "Junction") { "  $Link is a real folder: kept as it is"; return }
        if (@($i.Target) -contains $Target) { "  $Link -> $Target"; return }
        cmd /c rmdir "$Link" | Out-Null
    }
    New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
    "  $Link -> $Target"
}
Set-Link (Join-Path $game "disc") $Disc
Set-Link (Join-Path $game "dev_hdd0\game\$title") (Join-Path $Rpcs3 "dev_hdd0\game\$title")
# The game copies 2.4 GB to the HDD on its first boot (<title>_INSTALL). A copy of RPCS3's keeps
# the two apart; without one the port's first boot does the install itself.
$inst = Join-Path $Rpcs3 "dev_hdd0\game\${title}_INSTALL"
$instDst = Join-Path $game "dev_hdd0\game\${title}_INSTALL"
if ((Test-Path $inst) -and -not (Test-Path $instDst)) {
    "  copying the game's HDD install from RPCS3 (2.4 GB)"
    robocopy $inst $instDst /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copying $inst failed (robocopy $LASTEXITCODE)" }
} elseif (-not (Test-Path $instDst)) { "  note: no ${title}_INSTALL in RPCS3 -- the port's first boot installs it (if that fails: start the game once in RPCS3)" }

Step 3 "decrypting the game's 1.04 EBOOT with RPCS3"
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

Step 4 "Python tools (work\venv)"
$venvPy = Join-Path $root "work\venv\Scripts\python.exe"
if (-not (Test-Path $venvPy)) { & python -m venv (Join-Path $root "work\venv"); if ($LASTEXITCODE) { throw "python -m venv failed" } }
& $venvPy -m pip install --disable-pip-version-check -q -r (Join-Path $root "toolkit\requirements.txt")
if ($LASTEXITCODE) { throw "pip install failed" }

Step 5 "SDL2, Vulkan headers and glslang (download, then build glslang: a few minutes)"
& (Join-Path $PSScriptRoot "fetch_deps.ps1")
if (-not (Test-Path (Join-Path $root "deps\glslang-install\lib\cmake\glslang"))) {
    cmd /c "`"$(Join-Path $PSScriptRoot 'build_deps.cmd')`""
    if ($LASTEXITCODE) { throw "building glslang failed" }
}

Step 6 "the exe's icon (from your copy's ICON0.PNG)"
try { & (Join-Path $PSScriptRoot "make_icon.ps1") -Icon0 (Join-Path $Disc "PS3_GAME\ICON0.PNG") } catch { "  no icon: $_ (the game still builds)" }

Step 7 "translating and compiling the game (the long part: 15-40 minutes)"
& $venvPy -I (Join-Path $PSScriptRoot "rebuild_win.py")
if ($LASTEXITCODE) { throw "the build failed: see work\logs\ (python -I tools\build_errors.py work\logs\compile.log)" }
$exe = Join-Path $root "work\port\build\Infamous2Recomp.exe"
if (-not (Test-Path $exe)) { throw "no $exe after the build" }

Step 8 "desktop shortcut"
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
}
"== done in $([int]$sw.Elapsed.TotalMinutes) min: play with the desktop shortcut or 'Play inFAMOUS 2.cmd'"
exit 0
