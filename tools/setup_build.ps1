# The work behind Setup.exe and Setup.cmd (also usable on its own). v0.6.0, the slim Setup:
#   1. check this PC and the game                                    (tools\setup_common.ps1)
#   2. the port's tools: the prebuilt engine, clang + lld, an embeddable Python (and pycryptodome), about 48 MB from
#      the GitHub release in tools\slim_assets.json, SHA-256 checked, into tools-cache\slim\ (tools\slim_tools.ps1).
#      The 2 GB build tools of v0.5.x (MSVC, the Windows SDK, CMake, Ninja, full LLVM) are deleted when found.
#   3. your game INTO the port's folder (game\disc: a copy of the game folder, the disc image extracted, the PSN
#      package unpacked; tools\game_source.py), so the folder can be moved or copied to another drive or PC and plays;
#      the game's 1.04 update and the PS3 firmware files into game\: the port's own, else RPCS3's copies when it has
#      them, else Sony's own servers, unpacked with tools\ps3tool.py (tools\sony_files.py). The game makes its 2.4 GB
#      HDD install itself at its first start (RPCS3's copy is used when there is one).
#   4. decrypt the 1.04 EBOOT (tools\ps3tool.py, else RPCS3, or your own decryptor: tools\decrypt.ps1)
#                                                                    -> eboot\EBOOT_upd104.elf
#   5. (the build's Python: the embeddable one from step 2)   6. (SDL2, Vulkan, glslang: not needed, the engine is prebuilt)
#   7. the shortcut's icon from the player's own ICON0.PNG           -> work\icon\app.ico
#   8. translate the game and compile it into the game module beside the engine
#                                                                    -> work\port\build\Infamous2Game.dll (+ the engine)
#   9. a desktop shortcut
#  10. with -Cleanup: delete what only this build needed (tools\setup_cleanup.ps1). The port's tools (about 220 MB
#      unpacked) and the build files stay for the next update: an update that changes a few files then downloads only
#      a changed tool and rebuilds only what changed, in minutes. -NoBuildCache (or PORT_BUILD_CACHE=0: Setup.exe
#      --prepare, for RecompLauncher) deletes them too.
#   tools\setup_build.ps1 -Source <game folder | .iso | .pkg> [-DiscKey <file>] [-Rap <file>] [-Rpcs3 <RPCS3 folder>]
#                         [-NoShortcut] [-Cleanup] [-NoBuildCache] [-Pgo] [-KeepGameLink] [-NoAsk]
# -Disc is the old name of -Source (Setup.exe 0.5.x, which runs this script when it updates an install in place, and
# Setup.cmd); -AcceptMsvcLicense is taken and ignored (nothing of Microsoft's is downloaded any more).
# The step numbers stay those of v0.5.x (10, or 12 with -Pgo), so a Setup.exe 0.5.x reads this script's progress.
# -Pgo ("Optimise for this PC", Setup.exe's checkbox): profile-guided optimisation trained on this PC, 12 steps
# (steps 1-7 as above):
#   8. translate the game, then compile a training build (instrumented) -> work\port\build-train
#   9. train it: the game plays by itself in a window for a few minutes, sound off (tools\pgo_train.ps1)
#  10. compile the game with what it learned   11. a desktop shortcut   12. cleaning up
# PGO never makes Setup fail: when the training build, the training or the optimised compile fails, the game is
# compiled the normal way and the line "  pgo: not used: <why>" says why (Setup.exe shows it). Updates: the profile
# is kept (work\pgo-setup\setup-<time>.profdata, never published) and the new version is compiled with it; when hot
# code changed since the training, Setup trains again.
# An install of v0.5.x whose game\disc is a link to the player's game folder: the folder is copied in (Setup asks
# once, shows the size; -NoAsk or PORT_SETUP_NO_ASK=1: copied without asking; -KeepGameLink: the link stays, and the
# port's folder then still needs the player's game folder where it is). The player's folder is never changed.
# Lines for Setup.exe: "== [n/10] <step>", "  progress: <n>% <what>", "== FAILED: <why>", "  needs: disc-key" /
# "  needs: rap <content id>" (Setup asks for that file and runs this again with -DiscKey / -Rap).
param(
    [string]$Source = "",
    [string]$Disc = "",
    [string]$DiscKey = "",
    [string]$Rap = "",
    [string]$Rpcs3 = "",          # optional: RPCS3's update, firmware and license files are used when it has them
    [switch]$NoShortcut,
    [switch]$AcceptMsvcLicense,   # (ignored)
    [switch]$Cleanup,
    [switch]$NoBuildCache,
    [switch]$Pgo,
    [switch]$KeepGameLink,
    [switch]$NoAsk
)
$ErrorActionPreference = "Stop"
# Setup.exe reads these lines through a pipe, in UTF-8 (a console keeps its own code page)
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.Encoding]::UTF8 }
trap { "== FAILED: $_"; exit 1 }   # the reason lands in the log Setup shows
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "setup_common.ps1")
. (Join-Path $PSScriptRoot "slim_tools.ps1")
. (Join-Path $PSScriptRoot "shortcut.ps1")
$sw = [Diagnostics.Stopwatch]::StartNew()
$nSteps = if ($Pgo) { 12 } else { 10 }
function Step([int]$n, [string]$text) { "== [$n/$nSteps] $text   ($([int]$sw.Elapsed.TotalMinutes) min so far)" }
# Native tools (python, robocopy) write progress and warnings to stderr; under "Stop" with the output redirected to
# Setup's log, Windows PowerShell would turn the first such line into a failure. Run them under "Continue", keep
# their lines, judge them by exit code.
function Invoke-Native([scriptblock]$Cmd) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $Cmd 2>&1 | ForEach-Object { "    $_" } } finally { $ErrorActionPreference = $eap }
}
function Copy-Tree([string]$Src, [string]$Dst, [string]$What) {
    Remove-Junction $Dst   # an older Setup linked it
    "  copying $What"
    Invoke-Native { robocopy $Src $Dst /E /NFL /NDL /NJH /NJS /NP } | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copying $Src failed (robocopy $LASTEXITCODE)" }
}
# GitHub #19, #34: a port folder whose name the build tools can't take ("E:\Decomps & Recomps\...", another alphabet,
# a long path) is built through a junction with a plain name (tools\safe_paths.ps1 Get-BuildRoot): this script runs
# again from there, so every path from here on is plain. The files stay where they are; nothing is copied.
if (-not $env:PORT_REAL_ROOT) {
    $buildRoot = Get-BuildRoot $root -Create
    if ($buildRoot -ne $root) {
        "  your port folder: $root"
        "  built through $buildRoot (a link with a plain name: the build tools can't take every folder name)"
        $env:PORT_REAL_ROOT = $root
        try { & (Join-Path $buildRoot "tools\setup_build.ps1") @PSBoundParameters } finally { Remove-Item env:PORT_REAL_ROOT -ErrorAction SilentlyContinue }
        exit $LASTEXITCODE
    }
}
$realRoot = Get-RealRoot
$game = Join-Path $root "game"
$discDir = Join-Path $game "disc"
if (-not $Source) { $Source = $Disc }
$nameOf = { param($p) foreach ($r in $root, $realRoot) { if ($p -and ($p.TrimEnd('\') -eq "$r\game\disc" -or $p.TrimEnd('\') -eq "$r\game\psn")) { return $true } }; $false }
# The port's own game\disc given (Setup.exe 0.5.x updating in place passes it), or nothing (Setup.cmd after an
# install): the copy in it, or the player's folder an old link points to.
$link = Get-Item -LiteralPath $discDir -Force -ErrorAction SilentlyContinue
$isLink = $link -and ($link.Attributes -band [IO.FileAttributes]::ReparsePoint)
$linkTarget = if ($isLink -and @($link.Target)[0]) { ([string]@($link.Target)[0]).TrimEnd('\') } else { $null }
if ($linkTarget) {
    # v0.5.5's frame around a PSN folder (game\psn\PS3_GAME -> the player's title folder)
    $pj = Get-Item -LiteralPath (Join-Path $linkTarget "PS3_GAME") -Force -ErrorAction SilentlyContinue
    if ((& $nameOf $linkTarget) -and $pj -and ($pj.Attributes -band [IO.FileAttributes]::ReparsePoint) -and @($pj.Target)[0]) { $linkTarget = ([string]@($pj.Target)[0]).TrimEnd('\') }
}
if (-not $Source -or (& $nameOf $Source)) { $Source = if ($linkTarget) { $linkTarget } else { $discDir } }
if ((Test-Path -LiteralPath $Source -PathType Container)) { $Source = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\') }
elseif (Test-Path -LiteralPath $Source -PathType Leaf) { $Source = (Resolve-Path -LiteralPath $Source).Path }
if ($Rpcs3) {
    if (Test-Path -LiteralPath (Join-Path $Rpcs3 "rpcs3.exe")) { $Rpcs3 = (Resolve-Path -LiteralPath $Rpcs3).Path.TrimEnd('\') }
    else { "  note: no rpcs3.exe in ${Rpcs3}: RPCS3 is not used"; $Rpcs3 = "" }
}
$src = Get-GameSource $Source $Rpcs3
if ($src.Kind -eq "folder" -and $src.Folder) { $Source = $src.Folder }
if (-not $Rap -and $src.Title -like "NP*") {
    $cid0 = if ($src.ContentId) { $src.ContentId } elseif ($src.Sfo) { [string]$src.Sfo["CONTENT_ID"] } else { "" }
    $near = Find-RapNear $Source $cid0
    if ($near) { $Rap = $near; "  your license: $(Split-Path -Leaf $near), next to your game" }
}

Step 1 "checking"
foreach ($sub in @(@("engine", "CMakeLists.txt"), @("base", "infamous2-lle\CMakeLists.txt"))) {
    if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $root $sub[0]) $sub[1]))) {
        throw "$($sub[0])\$($sub[1]) is missing: download the port again with Setup.exe (or 'git clone')"
    }
    "  $($sub[0]): ok"
}
# A Setup.exe of v0.4.2 or older updates by copying the new version over the old one, never deleting. A file the new
# engine\ or base\ no longer has is deleted here (.port-files, from the public repository). Not in a git clone, never
# through a link.
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
                if ($e -is [IO.DirectoryInfo]) { if ($e.Name -ne "__pycache__") { $stack.Push($e) }; continue }
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
"  your game: $Source ($($src.Kind)$(if ($src.Layout) { ', ' + $src.Layout })$(if ($src.Title) { ', ' + $src.Title }))"
$rows = Get-SetupChecks -Disc $Source -Rpcs3 $Rpcs3 -DiscKey $DiscKey -Rap $Rap
foreach ($r in $rows) { "  {0,-5} {1}: {2}" -f $(if ($r.Ok -eq $true) { "ok" } elseif ($r.Ok -eq $false) { "MISS" } else { "note" }), $r.Name, $r.Detail }
$miss = @($rows | Where-Object { $_.Ok -eq $false })
if ($miss | Where-Object { $_.Name -eq "Disc key" }) { "  needs: disc-key" }
if ($miss | Where-Object { $_.Name -eq "License (.rap)" }) { "  needs: rap $($src.ContentId)" }
if ($miss) { throw "fix the MISS items first: $(($miss | ForEach-Object { $_.Name + ': ' + $_.Detail }) -join '; ')" }
$title = $src.Title
$isPsn = $title -like "NP*"
# config.local.json keeps no path of this PC (the folder may be moved or copied): an "rpcs3" an older Setup wrote goes
$cfgPath = Join-Path $root "config.local.json"
if ((Test-Path -LiteralPath $cfgPath) -and -not (Test-Path -LiteralPath (Join-Path $root "PLAN.md"))) {
    try {
        $cfg = [ordered]@{}
        (Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json).psobject.Properties | ForEach-Object { $cfg[$_.Name] = $_.Value }
        $abs = @($cfg.Keys | Where-Object { $_ -ne "decryptor" -and $cfg[$_] -is [string] -and [IO.Path]::IsPathRooted([string]$cfg[$_]) })
        if ($abs.Count) {
            foreach ($k in $abs) { $cfg.Remove($k) }
            if ($cfg.Count) { [IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json)) } else { Remove-Item -LiteralPath $cfgPath -Force }
            "  config.local.json: no paths of this PC kept ($($abs -join ', '))"
        }
    } catch { "  note: config.local.json could not be read ($_)" }
}
if ($Rpcs3) { $env:PORT_RPCS3 = $Rpcs3 }   # for this build only (tools\rebuild_win.py, tools\decrypt.ps1)

Step 2 "the port's tools: the prebuilt engine, clang + lld and Python (about 48 MB, nothing is installed)"
Remove-OldBuildTools
Install-SlimTools
$slim = Get-SlimPaths
$py = $slim.Python
$crypto = Test-Path -LiteralPath (Join-Path (Get-SlimDir python) "Lib\site-packages\Crypto")
"  engine: $($slim.Engine)"
"  clang + lld: $($slim.Llvm)"
"  Python: $py$(if ($crypto) { ' (with pycryptodome)' } else { ' (no pycryptodome: unpacking Sony''s files takes a few minutes longer)' })"

Step 3 "your game into the port's folder; the game's update and the PS3 firmware files (from RPCS3 when it has them, else from Sony's servers)"
New-Item -ItemType Directory -Force (Join-Path $game "dev_hdd0\game"), (Join-Path $game "gamedata") | Out-Null
$keepMark = Join-Path $root "work\setup-keep-link"
$ps3tool = Join-Path $PSScriptRoot "ps3tool.py"
# tools\ps3tool.py first (a release zip comes without it: from GitHub): game_source.py needs it for a .pkg
$eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
try { & $py -I (Join-Path $PSScriptRoot "sony_files.py") ps3tool --root $root 2>&1 | ForEach-Object { "  $_" } } finally { $ErrorActionPreference = $eap }
# Before anything large is copied: can this ps3tool do what this pick needs (tools\setup_common.ps1 Get-Ps3ToolModes)?
# An encrypted disc image needs its "iso" mode, a PSN copy its license lookup ("psn-rap": tools\game_source.py modes),
# or RPCS3 holding the license. Both are there in the current ps3tool; an older one stops here with what to do. A
# file it can't use is never asked for, and nothing is copied for a game that can't be unlocked afterwards.
if (($src.Kind -eq "iso" -and $src.Encrypted) -or ($isPsn -and -not (Test-Path -LiteralPath (Join-Path $root "eboot\EBOOT_upd104.elf")))) {
    $modes = Get-Ps3ToolModes
    if ($null -eq $modes) { $modes = @() }
    "  this ps3tool offers: $(if ($modes.Count) { $modes -join ', ' } else { '(nothing it could say)' })"
    if ($src.Kind -eq "iso" -and $src.Encrypted) {
        if ($modes -notcontains "iso") { throw $script:IsoCant }
        # (no key given: ps3tool looks beside the image and inside it first; "needs: disc-key" only when it finds none)
    }
    if ($isPsn) {
        $ownTool = $false
        try { . (Join-Path $PSScriptRoot "decrypt.ps1"); $ownTool = [bool](Get-DecryptorTemplate) } catch {}
        $cidP = if ($src.ContentId) { $src.ContentId } elseif ($src.Sfo) { [string]$src.Sfo["CONTENT_ID"] } else { "" }
        $licRp = if ($Rpcs3 -and $cidP) { Find-DigitalLicense $Rpcs3 $cidP } else { $null }
        $licOwn = $cidP -and (Test-Path -LiteralPath (Join-Path $game "gamedata\dev_hdd0\home\00000001\exdata\$cidP.rap"))
        if ($ownTool) { "  your PSN copy: unlocked by your own decryptor (config.local.json)" }
        elseif ($modes -contains "psn-rap") {
            # (a .rap beside the game is found by Find-RapNear above; RPCS3's exdata by ps3tool itself)
            if (-not $Rap -and -not $licOwn -and -not $licRp) { "  needs: rap $cidP"; throw "the PSN version's code is unlocked with your license for it: Setup needs your $cidP.rap file" }
        }
        elseif ($licRp) { "  your PSN copy: unlocked through RPCS3, which holds your license ($(Split-Path -Leaf $licRp)); the ps3tool in this folder is an older one that can't use a license file" }
        else { throw (Get-PsnCant $title) }
    }
}
if ($isLink) {
    # an install of v0.5.x: game\disc is a link to the player's game folder
    $keep = $KeepGameLink -or (Test-Path -LiteralPath $keepMark)
    if (-not $keep) {
        $gb = 0
        try { $gb = [math]::Round(((Get-ChildItem -LiteralPath $Source -Recurse -File -Force -ErrorAction Stop | Measure-Object Length -Sum).Sum) / 1GB, 1) } catch {}
        $yes = $true
        if (-not $NoAsk -and $env:PORT_SETUP_NO_ASK -ne "1" -and [Environment]::UserInteractive) {
            try {
                Add-Type -AssemblyName System.Windows.Forms
                $owner = New-Object System.Windows.Forms.Form
                $owner.TopMost = $true; $owner.ShowInTaskbar = $false; $owner.StartPosition = "CenterScreen"; $owner.Size = New-Object System.Drawing.Size(1, 1)
                $owner.Show(); $owner.Hide()
                $ans = [System.Windows.Forms.MessageBox]::Show($owner,
                    ("From this version on, the port keeps its own copy of your game, so its folder can be moved or copied to another " +
                     "drive or PC and still play.`n`nCopy your game folder ($gb GB) into the port's folder now?`n`n$Source`n`nYour folder is not changed. " +
                     "No: the port keeps using it where it is (and asks no more)."),
                    "inFAMOUS 2 PC port - Setup", [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Question)
                $owner.Dispose()
                $yes = $ans -eq [System.Windows.Forms.DialogResult]::Yes
            } catch { $yes = $true }
        }
        if (-not $yes) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keepMark) | Out-Null
            Set-Content -LiteralPath $keepMark -Value "The player kept game\disc as a link to his game folder ($(Get-Date -Format s))"
            $keep = $true
        }
    }
    if ($keep) { "  your game: game\disc stays a link to $Source (the port's folder needs it there: delete work\setup-keep-link and run Setup to copy it in)" }
    else {
        "  your game: game\disc was a link to $Source; copying it in ($gb GB), your folder stays as it is"
        Remove-Junction $discDir
        $psnFrame = Join-Path $game "psn"
        if (Test-Path -LiteralPath $psnFrame) { Remove-Junction (Join-Path $psnFrame "PS3_GAME"); Remove-Item -LiteralPath $psnFrame -Force -Recurse -ErrorAction SilentlyContinue }
    }
}
if (-not ($isLink -and $keep)) {
    $gsArgs = @("-I", (Join-Path $PSScriptRoot "game_source.py"), "import", $Source, "--game", $game, "--ps3tool", $ps3tool, "--python", $py)
    if ($DiscKey) { $gsArgs += @("--disc-key", $DiscKey) }
    if ($Rap) { $gsArgs += @("--rap", $Rap) }
    $why = $null
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $py @gsArgs 2>&1 | ForEach-Object { $s = "$_"; if ($s -match '^\s*error:\s*(.+)$') { $why = $Matches[1].Trim() }; $s } } finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE) {
        if ($why -match 'disc key') { "  needs: disc-key" }
        throw "your game could not be put into the port's folder: $why"
    }
}
# The update and the firmware files: the port's own copies (an earlier Setup), else RPCS3's, else Sony's servers.
$hdd = Get-Rpcs3Dir $Rpcs3 "dev_hdd0" "game\$title\PARAM.SFO"
$updDst = Join-Path $game "dev_hdd0\game\$title"
$flashDst = Join-Path $game "dev_flash"
function Test-Update([string]$Dir) {
    $sfo = Join-Path $Dir "PARAM.SFO"
    if (-not (Test-Path -LiteralPath $sfo) -or -not (Test-Path -LiteralPath (Join-Path $Dir "USRDIR\EBOOT.BIN"))) { return $false }
    $v = (Read-Sfo $sfo)["APP_VER"]
    $v -eq "01.04" -or ($isPsn -and $v -eq "01.03")
}
function Test-Firmware([string]$Flash) {
    $Flash -and -not (@("liblv2", "libsysmodule", "libsre") | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Flash "sys\external\$_.sprx")) })
}
$sony = @()
$rpUpd = if ($hdd) { Join-Path $hdd "game\$title" } else { $null }
if ($isPsn) {
    # the PSN copy is its own update (01.03): game_source.py made game\dev_hdd0\game\<ID> from it; an old link install:
    # its folder, copied
    if (-not (Test-Update $updDst) -and (Test-Update $Source)) { Copy-Tree $Source $updDst "your PSN copy's own folder as the game's update" }
    if (-not (Test-Update $updDst)) { throw "the PSN copy is not version 01.03 (the port needs the PSN version's last update)" }
    "  the PSN copy's update: game\dev_hdd0\game\$title"
}
elseif (Test-Update $updDst) { "  the 1.04 update: already in game\dev_hdd0\game\$title" }
elseif ($rpUpd -and (Test-Update $rpUpd)) { Copy-Tree $rpUpd $updDst "the 1.04 update from RPCS3 (0.2 GB)" }
else { $sony += "update" }
if (Test-Firmware $flashDst) { "  the PS3 firmware files: already in game\dev_flash" }
elseif ($Rpcs3 -and (Test-Firmware (Get-Rpcs3Dir $Rpcs3 "dev_flash")) -and ((Get-FirmwareVersion (Get-Rpcs3Dir $Rpcs3 "dev_flash")) -in @($null, "4.93"))) {
    # (RPCS3's firmware only when it is the 4.93 the port is made with, or does not say: another version comes from Sony)
    # only the three modules the build translates, as from Sony (the game itself opens nothing else in dev_flash)
    $rf = Get-Rpcs3Dir $Rpcs3 "dev_flash"
    New-Item -ItemType Directory -Force (Join-Path $flashDst "sys\external") | Out-Null
    foreach ($m in "liblv2", "libsysmodule", "libsre") { Copy-Item -LiteralPath (Join-Path $rf "sys\external\$m.sprx") -Destination (Join-Path $flashDst "sys\external\$m.sprx") -Force }
    "  the PS3 firmware modules: copied from RPCS3 ($rf)"
}
else { $sony += "firmware" }
if ($sony.Count) {
    "  from Sony's servers: $($sony -join ' and ') (RPCS3 $(if ($Rpcs3) { 'does not have them' } else { 'is not used' }))"
    $what = if ($sony.Count -eq 2) { "all" } else { $sony[0] }
    $sArgs = @("-I", (Join-Path $PSScriptRoot "sony_files.py"), $what, "--root", $root, "--title", $title)
    # PORT_SONY_DOWNLOADS=<folder>: Sony's files are looked for there first and kept there (tests, offline installs)
    if ($env:PORT_SONY_DOWNLOADS) { $sArgs += @("--downloads", $env:PORT_SONY_DOWNLOADS, "--keep-downloads") }
    $why = $null
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $py @sArgs 2>&1 | ForEach-Object { $s = "$_"; if ($s -match '^\s*error:\s*(.+)$') { $why = $Matches[1].Trim() }; "  $s" } } finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE) {
        throw ("getting the game's $($sony -join ' and ') from Sony's servers failed: $why. Check your internet connection and run " +
               "Setup again: it goes on where it stopped. (Only if Sony's servers can't be reached from your network at all: " +
               "an RPCS3 on this PC that has the game's 1.04 update and firmware 4.93 installed works too, Setup copies them from it.)")
    }
}
# DLC (GitHub #48, #56): the game's unlock files (...\USRDIR\dlc\ulf\unlock_*.edat, tools\port_paths.ps1). Copied
# from RPCS3 when it has them, on a first install and on every later run, wherever the update came from (Sony's
# update has none), and from the drop folder <port>\dlc (the launcher does that too, at each start). Files only
# ever get added: the install's dlc folder is never deleted. Nothing here can make Setup fail.
try {
    $dlcDst = Join-Path $updDst "USRDIR\dlc"
    $rpDlcHdd = if ($Rpcs3) { Get-Rpcs3Dir $Rpcs3 "dev_hdd0" "game\$title\USRDIR\dlc" } else { $null }
    $rpDlc = if ($rpDlcHdd) { Join-Path $rpDlcHdd "game\$title\USRDIR\dlc" } else { $null }
    $nRp = 0; $nDrop = 0
    if ($rpDlc -and @(Get-DlcFiles $rpDlc).Count) { $nRp = Copy-DlcTree $rpDlc $dlcDst }
    $drop = New-DlcDropFolder $root
    $nDrop = Copy-DlcDrop $drop (Join-Path $dlcDst "ulf")
    $dlcHave = @(Get-DlcFiles $dlcDst)
    if ($dlcHave.Count) {
        $from = @($(if ($nRp) { "$nRp copied from RPCS3 ($rpDlc)" }), $(if ($nDrop) { "$nDrop copied from the dlc folder" })) | Where-Object { $_ }
        "  DLC: $($dlcHave.Count) unlock file(s) in game\dev_hdd0\game\$title\USRDIR\dlc$(if ($from) { ' (' + ($from -join ', ') + ')' })"
        $lic = Get-DlcNeedLicense $dlcHave
        if ($lic) { "  note: $lic of the DLC files need their license (.rap): not supported yet (the game will not show that DLC)" }
    }
    else { "  DLC: none found (optional: install it in RPCS3 and run Setup again, or put its unlock files, *.edat, in the port's dlc folder)" }
} catch { "  note: the DLC files could not be copied ($_): the game plays without them" }
# The game copies 2.4 GB to the HDD on its first boot (<title>_INSTALL): RPCS3's copy if there is one, else the
# game does the install itself (about a minute, behind the title screen).
$instDst = Join-Path $game "dev_hdd0\game\${title}_INSTALL"
$inst = if ($hdd) { Join-Path $hdd "game\${title}_INSTALL" } else { $null }
if ($inst -and (Test-Path -LiteralPath $inst) -and -not (Test-Path -LiteralPath $instDst)) { Copy-Tree $inst $instDst "the game's HDD install from RPCS3 (2.4 GB)" }
elseif (-not (Test-Path -LiteralPath $instDst)) { "  the game's HDD install (2.4 GB): the game makes it itself at its first start" }

Step 4 "decrypting the game's 1.04 EBOOT (tools\ps3tool.py, else RPCS3, or your own decryptor: tools\decrypt.ps1)"
$eb = Join-Path $root "eboot"
New-Item -ItemType Directory -Force $eb | Out-Null
$elf = Join-Path $eb "EBOOT_upd104.elf"
if (-not (Test-Path -LiteralPath $elf)) {
    . (Join-Path $PSScriptRoot "decrypt.ps1")
    $script:DecryptorTemplate = Get-DecryptorTemplate
    $bin = Join-Path $eb "EBOOT_upd104.BIN"
    Copy-Item -LiteralPath (Join-Path $updDst "USRDIR\EBOOT.BIN") -Destination $bin -Force
    if ($isPsn -and $Rap) {
        # the player's license, kept where a PS3 (and RPCS3) keeps it: copied as a file, never read here
        $cid = (Read-Sfo (Join-Path $updDst "PARAM.SFO"))["CONTENT_ID"]
        $ex = Join-Path $game "gamedata\dev_hdd0\home\00000001\exdata"
        New-Item -ItemType Directory -Force $ex | Out-Null
        Copy-Item -LiteralPath $Rap -Destination (Join-Path $ex "$cid.rap") -Force
        $script:Ps3ToolRap = Join-Path $ex "$cid.rap"   # (the copy: named after the game's content id, as ps3tool looks it up)
    } elseif ($isPsn) {
        $cid = (Read-Sfo (Join-Path $updDst "PARAM.SFO"))["CONTENT_ID"]
        $own = Join-Path $game "gamedata\dev_hdd0\home\00000001\exdata\$cid.rap"
        if (Test-Path -LiteralPath $own) { $script:Ps3ToolRap = $own }
    }
    $firstDefault = @(Get-DefaultDecryptors) | Select-Object -First 1
    $usesRpcs3 = if ($script:DecryptorTemplate) { $script:DecryptorTemplate -match "rpcs3" } else { $null -ne $firstDefault -and $firstDefault -eq "" }
    if ($usesRpcs3 -and (Get-Process rpcs3 -ErrorAction SilentlyContinue)) { throw "close RPCS3 first (it decrypts one file per start)" }
    try { Invoke-Decryptor -InPath $bin -OutPath $elf }
    catch {
        if ($isPsn) {
            # a PSN EBOOT (NPDRM) is unlocked only with the player's own license for it (GitHub #40)
            $cid = (Read-Sfo (Join-Path $updDst "PARAM.SFO"))["CONTENT_ID"]
            if (-not $Rap) { "  needs: rap $cid" }
            throw ("your PSN copy's EBOOT could not be unlocked ($($_.Exception.Message.Trim())). It needs your license for the game ($cid.rap)" +
                   $(if ($Rap) { ": ps3tool could not unlock it with $(Split-Path -Leaf $Rap). Check that this .rap is your license for this game ($cid)" } else { "" }))
        }
        throw "decrypting the game's EBOOT failed: $($_.Exception.Message)"
    }
}
"  $elf"

Step 5 "the build's Python"
"  ready: $py (the embeddable Python, from step 2)"
Step 6 "SDL2, Vulkan headers and glslang"
"  not needed: the engine comes prebuilt (tools-cache\slim\engine)"

Step 7 "the shortcut's icon (from your copy's ICON0.PNG)"
$icon0 = Join-Path (Get-GameRoot $discDir) "ICON0.PNG"
try { & (Join-Path $PSScriptRoot "make_icon.ps1") -Icon0 $icon0 } catch { "  no icon: $_ (the game still builds)" }

# The player's build: the lift and the game module with the slim tools only (no MSVC, CMake or Ninja); the engine is
# the release's (copied beside the module). With the build files of an earlier build (an update): a translation step
# whose inputs did not change is skipped, and only what changed is compiled again (tools\rebuild_win.py PORT_LIFT_REUSE).
$env:PORT_LIFT_REUSE = "1"
$env:PORT_ENGINE_EXE = $slim.Engine
$env:PORT_LLVM = $slim.Llvm
$env:PORT_PYTHON = $py
foreach ($k in "PORT_TK", "PORT_DIR", "PORT_BUILD", "PORT_ENGINE_BUILD", "PORT_ENGINE_PGO", "PORT_PGO") { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
$playerSteps = "inputs,port,firmware,ppu,spu,jobs,compile"
$exe = Join-Path $root "work\port\build\Infamous2Recomp.exe"
$dll = Join-Path $root "work\port\build\Infamous2Game.dll"
$buildFailed = "the build failed: see work\logs\ (python -I tools\build_errors.py work\logs\module.log)"
$rebuild = Join-Path $PSScriptRoot "rebuild_win.py"
if (-not $Pgo) {
    Step 8 "translating the game and compiling it (the long part: about 10 minutes)"
    "  compile log: work\logs\module.log"
    Invoke-Native { & $py -I $rebuild --only $playerSteps }
    if ($LASTEXITCODE) { throw $buildFailed }
} else {
    # rebuild_win.py with PORT_PGO / PORT_BUILD set for this one call; its lines go on to Setup and into $script:rbOut
    function Invoke-Rebuild([string[]]$RbArgs, [string]$PgoMode, [string]$BuildDir) {
        $keep = @{ PORT_PGO = $env:PORT_PGO; PORT_BUILD = $env:PORT_BUILD }
        $script:rbOut = @(); $script:rbRc = 1
        try {
            if ($PgoMode) { $env:PORT_PGO = $PgoMode } else { Remove-Item env:PORT_PGO -ErrorAction SilentlyContinue }
            if ($BuildDir) { $env:PORT_BUILD = $BuildDir } else { Remove-Item env:PORT_BUILD -ErrorAction SilentlyContinue }
            Invoke-Native { & $py -I $rebuild @RbArgs } | ForEach-Object { $script:rbOut += $_; $_ }
            $script:rbRc = $LASTEXITCODE
        } finally {
            foreach ($k in @($keep.Keys)) {
                if ($null -ne $keep[$k]) { Set-Item "env:$k" $keep[$k] } else { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
            }
        }
    }
    $llvmProf = $slim.Profdata
    function Test-Profile([string]$Path) {   # this LLVM reads it
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        try { & $llvmProf show $Path 2>&1 | Out-Null; $ok = ($LASTEXITCODE -eq 0) } catch { $ok = $false } finally { $ErrorActionPreference = $eap }
        $ok
    }
    $train = Join-Path $root "work\port\build-train"
    $trainExe = Join-Path $train "Infamous2Recomp.exe"
    $pgoDir = Join-Path $root "work\pgo-setup"
    # tests (tools\setup_exe\README.md): PORT_PGO_TEST=fail:llvm | fail:trainbuild | fail:compile | stale | stopafter:<s>,
    # PORT_PGO_KEEP_TRAIN=1 keeps the training build; PORT_PGO_TRAIN=opening|save|opening,save picks the runs
    $test = [string]$env:PORT_PGO_TEST
    $why = $null; $used = $null; $pd = $null; $fallback = $false
    if ($test -eq "fail:llvm") { $llvmProf = $null }
    if (-not $llvmProf) { $why = "the port's clang has no llvm-profdata.exe" }
    $kept = $null
    if ($llvmProf) {
        $kept = Get-ChildItem -LiteralPath $pgoDir -Filter "setup-*.profdata" -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
        if ($kept -and -not (Test-Profile $kept.FullName)) {
            "  the last training's profile ($($kept.Name)) cannot be read by this LLVM: training again"
            Remove-Item -LiteralPath $kept.FullName -Force -ErrorAction SilentlyContinue; $kept = $null
        }
    }
    if ($kept) { Step 8 "translating the game, then compiling it with what the last training on this PC learned (about 10 minutes)" }
    elseif ($why) { Step 8 "translating the game (about 7 minutes)" }
    else { Step 8 "translating the game, then compiling a training build (about 10 minutes)" }
    Invoke-Native { & $py -I $rebuild --only "inputs,port,firmware,ppu,spu,jobs" }
    if ($LASTEXITCODE) { throw $buildFailed }
    if ($kept) {
        "  compile log: work\logs\module.log"
        Invoke-Rebuild @("--only", "compile") ("use:" + $kept.FullName) ""
        if ($script:rbRc -eq 0 -and (Test-Path -LiteralPath $dll)) {
            $fallback = $true
            $stale = @($script:rbOut | Where-Object { $_ -match 'PGO WARNING|-- retrain when these are hot' })
            if (-not $stale.Count -and $test -ne "stale") { $used = "the training of $($kept.LastWriteTime.ToString('yyyy-MM-dd')) still fits this version" }
            else { "  hot code changed since the last training: training again (the game just built stays if that does not work)" }
        } else { "  compiling with the last training failed: training again" }
    }
    if (-not $used -and -not $why) {
        Remove-Item -LiteralPath (Join-Path $train "Infamous2Game.dll") -ErrorAction SilentlyContinue   # never train an older one
        "  compile log: work\logs\module-build-train.log"
        Invoke-Rebuild @("--only", "compile") "gen" $train
        if ($script:rbRc -or -not (Test-Path -LiteralPath (Join-Path $train "Infamous2Game.dll")) -or -not (Test-Path -LiteralPath $trainExe) -or $test -eq "fail:trainbuild") {
            $why = "the training build did not compile (work\logs\module-build-train.log)"
        }
    }
    Step 9 $(if ($used) { "training: not needed ($used)" } elseif ($why) { "training: skipped" }
             else { "training the build on your PC (the game plays by itself in a window: please don't touch it)" })
    if (-not $used -and -not $why) {
        New-Item -ItemType Directory -Force $pgoDir | Out-Null
        # a new name every time: a profile at the same path whose content changed would not rebuild anything
        $pd = Join-Path $pgoDir ("setup-{0:yyyyMMdd-HHmmss}.profdata" -f (Get-Date))
        $runs = if ($env:PORT_PGO_TRAIN) { $env:PORT_PGO_TRAIN } else { "opening,save" }
        $stopAfter = if ($test -match '^stopafter:(\d+)$') { [int]$Matches[1] } else { 0 }
        $psExe = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
        $tOut = @()
        Invoke-Native { & $psExe -NoProfile -ExecutionPolicy RemoteSigned -File (Join-Path $PSScriptRoot "pgo_train.ps1") -Exe $trainExe -Profdata $pd -LlvmProfdata $llvmProf -Runs $runs -StopAfter $stopAfter } |
            ForEach-Object { $tOut += $_; $_ }
        if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $pd)) {
            $w = @($tOut | ForEach-Object { if ("$_" -match '^\s*why:\s*(.+)$') { $Matches[1].Trim() } }) | Select-Object -Last 1
            $why = "the training run gave nothing usable: $(if ($w) { $w } else { 'see the lines above' })"
            Remove-Item -LiteralPath $pd -ErrorAction SilentlyContinue; $pd = $null
        }
    } elseif (-not $used) { "  skipped: $why" }
    if ($env:PORT_PGO_KEEP_TRAIN -ne "1") { try { Remove-Tree $train } catch { "  note: could not delete $train ($_)" } }   # the training build is never the game
    if ($used) { Step 10 "compiling the game, optimised for this PC: done in step 8" }
    else {
        Step 10 $(if ($pd) { "compiling the game, optimised for this PC" } else { "compiling the game" })
        $built = $false
        if ($pd) {
            "  compile log: work\logs\module.log"
            Invoke-Rebuild @("--only", "compile") ("use:" + $pd) ""
            if ($script:rbRc -or $test -eq "fail:compile") { $why = "the optimised compile failed (work\logs\module.log)" }
            else { $built = $true; $used = "trained on this PC now" }
        }
        if (-not $built) {
            if ($fallback -and -not $pd) { $used = "the last training, partly out of date: $why" }
            else {
                "  note: Optimise for this PC did not work this time ($why): compiling the game the normal way"
                "  compile log: work\logs\module.log"
                Invoke-Rebuild @("--only", "compile") "" ""
                if ($script:rbRc) { throw $buildFailed }
            }
        }
    }
    if ($used) {
        "  pgo: used ($used)"
        if ($pd -and $used -like "trained*") {
            Get-ChildItem -LiteralPath $pgoDir -Filter "setup-*.profdata" -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -ne $pd } | Remove-Item -Force -ErrorAction SilentlyContinue
        }
    } else { "  pgo: not used: $why" }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "no $exe after the build" }
if (-not (Test-Path -LiteralPath $dll)) { throw "no $dll after the build" }

Step ($nSteps - 1) "desktop shortcut"
if (-not $NoShortcut) {
    # tools\play.ps1 makes it again at each start when the port's folder was moved or copied (work\shortcut.txt)
    $lnkPath = New-PortShortcut $realRoot
    "  $lnkPath"
} else { "  skipped (-NoShortcut)" }

Step $nSteps "cleaning up"
if ($Cleanup) { & (Join-Path $PSScriptRoot "setup_cleanup.ps1") -KeepBuildCache:(-not $NoBuildCache -and $env:PORT_BUILD_CACHE -ne "0") }
else { "  kept the build tools and files (no -Cleanup): tools\setup_cleanup.ps1 deletes them" }
"== done in $([int]$sw.Elapsed.TotalMinutes) min: play with the desktop shortcut or 'Play inFAMOUS 2.cmd'. Everything the game needs is in this folder (copy or move it anywhere)."
exit 0
