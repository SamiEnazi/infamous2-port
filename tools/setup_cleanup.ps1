# After the build: delete everything only the build needed, keep the game.
#   tools\setup_cleanup.ps1 [-Force]
# Deletes: tools-cache\ (the portable build tools), deps\ (SDL2, Vulkan headers, glslang sources and
#          build), work\venv\, work\port\ (the translated sources and the object files) except the
#          finished game, work\logs\compile*.log, build\ (cached compile settings)
# Keeps:   work\port\build\Infamous2Recomp.exe + SDL2.dll (+ its .pdb, for crash reports),
#          work\EBOOT.elf (the game reads it), work\icon, game\ (your game's files, firmware files, saves),
#          config.local.json, eboot\, the desktop shortcut, and "Optimise for this PC"'s profile
#          (work\pgo-setup\setup-*.profdata, ~30 MB, with the training runs' logs): an update compiles with
#          it instead of training again while it still fits (tools\setup_build.ps1 -Pgo)
# Both ways delete what a training left behind: work\port\build-train, the runs' save copies and counters.
# A later update just downloads the tools again. Never runs in the development folder (it has
# PLAN.md) unless -Force. Prints "freed: <GB>" for Setup.
#   tools\setup_cleanup.ps1 -KeepBuildCache   (what Setup does after a build, unless told otherwise)
# keeps the build tools and the build files for the next update instead: tools-cache\, deps\, work\venv\,
# work\port\ (the translated game and its object files) and build\, about 2.6 GB. The next update then
# downloads no tools, skips every translation step whose inputs did not change and compiles only what
# changed: minutes instead of 20-60. Only logs and leftovers go. Running it without the switch frees them.
param([switch]$Force, [switch]$KeepBuildCache)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ((Test-Path -LiteralPath (Join-Path $root "PLAN.md")) -and -not $Force) { "  not cleaning up: this is the development folder (it has PLAN.md)"; exit 0 }

# Bytes under a folder, without following links (game\disc is a junction to the player's disc).
function Get-TreeSize([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return 0L }
    $total = 0L
    $stack = New-Object System.Collections.Stack
    $stack.Push((Get-Item -LiteralPath $Path -Force))
    while ($stack.Count) {
        $d = $stack.Pop()
        if ($d -is [IO.FileInfo]) { $total += $d.Length; continue }
        try { $items = $d.GetFileSystemInfos() } catch { continue }
        foreach ($e in $items) {
            if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($e -is [IO.DirectoryInfo]) { $stack.Push($e) } else { $total += $e.Length }
        }
    }
    $total
}
# rd /s removes a junction itself, never what it points to.
# A file still in use is a note, never a failure: under "Stop", rd's message on stderr would end the build.
function Remove-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { cmd /c rd /s /q "$Path" 2>&1 | Out-Null } finally { $ErrorActionPreference = $eap }
    if (Test-Path -LiteralPath $Path) { "  could not delete all of $Path (a file in use?)" }
}

$before = Get-TreeSize $root
# "Optimise for this PC" (tools\pgo_train.ps1): the training build and what a stopped training left in its runs
if ($env:PORT_PGO_KEEP_TRAIN -ne "1") { Remove-Tree (Join-Path $root "work\port\build-train") }   # (a test keeps it)
foreach ($r in Get-ChildItem -LiteralPath (Join-Path $root "work\pgo-setup\runs") -Directory -ErrorAction SilentlyContinue) {
    Remove-Tree (Join-Path $r.FullName "gamedata")
    Get-ChildItem -LiteralPath $r.FullName -Filter "*.profraw" -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}
if ($KeepBuildCache) {
    "  cleaning up: logs and leftovers; the build tools and build files stay for quick updates"
    foreach ($d in "tools-cache\downloads", "tools-cache\msvc.unpack", "tools-cache\sdk.unpack", "deps\sdl2-dl", "deps\glslang-build", "work\port-keep") {
        Remove-Tree (Join-Path $root $d)
    }
    foreach ($l in Get-ChildItem -LiteralPath (Join-Path $root "work\logs") -Filter "compile*.log" -ErrorAction SilentlyContinue) {
        try { Remove-Item -LiteralPath $l.FullName -Force -ErrorAction Stop } catch { "  could not delete $($l.Name)" }
    }
    $kept = 0L
    foreach ($d in "tools-cache", "deps", "work\venv", "work\port", "build") { $kept += Get-TreeSize (Join-Path $root $d) }
    foreach ($f in "Infamous2Recomp.exe", "SDL2.dll", "Infamous2Recomp.pdb") {   # the game itself, not a build file
        $p = Join-Path $root "work\port\build\$f"
        if (Test-Path -LiteralPath $p) { $kept -= (Get-Item -LiteralPath $p).Length }
    }
    $after = Get-TreeSize $root
    "  freed: {0:N1} GB; kept for quick updates: {1:N1} GB of build tools and build files (tools\setup_cleanup.ps1 deletes them); the game folder uses {2:N1} GB" -f (($before - $after) / 1GB), ($kept / 1GB), ($after / 1GB)
    exit 0
}
"  cleaning up: the build tools and what only the build needed"
foreach ($d in "tools-cache", "deps", "work\venv", "build") { Remove-Tree (Join-Path $root $d) }
foreach ($l in Get-ChildItem -LiteralPath (Join-Path $root "work\logs") -Filter "compile*.log" -ErrorAction SilentlyContinue) {
    try { Remove-Item -LiteralPath $l.FullName -Force -ErrorAction Stop } catch { "  could not delete $($l.Name)" }
}

# work\port: keep only the finished game
$port = Join-Path $root "work\port"
$keep = @("Infamous2Recomp.exe", "SDL2.dll", "Infamous2Recomp.pdb")
$bld = Join-Path $port "build"
if (Test-Path -LiteralPath (Join-Path $bld "Infamous2Recomp.exe")) {
    $save = Join-Path $root "work\port-keep"
    Remove-Tree $save
    New-Item -ItemType Directory -Force $save | Out-Null
    foreach ($f in $keep) { if (Test-Path -LiteralPath (Join-Path $bld $f)) { Move-Item -LiteralPath (Join-Path $bld $f) $save } }
    Remove-Tree $port
    New-Item -ItemType Directory -Force $bld | Out-Null
    foreach ($f in $keep) { if (Test-Path -LiteralPath (Join-Path $save $f)) { Move-Item -LiteralPath (Join-Path $save $f) $bld } }
    Remove-Tree $save
} else { Remove-Tree $port }

$after = Get-TreeSize $root
"  freed: {0:N1} GB (the game folder now uses {1:N1} GB)" -f (($before - $after) / 1GB), ($after / 1GB)
