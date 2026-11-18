# After the build: delete what only the build needed, keep the game.
#   tools\setup_cleanup.ps1 [-Force]
# Deletes: tools-cache\ (the port's tools: tools-cache\slim, and any build tools of v0.5.x), deps\, work\venv\,
#          work\port\ (the translated sources and the object files) except the finished game, the build logs,
#          build\, and what a download or unpack left behind
# Keeps:   work\port\build\Infamous2Recomp.exe + SDL2.dll (the engine) + Infamous2Game.dll (the game module) (+ their
#          .pdb, for crash reports), work\EBOOT.elf (the game reads it), work\icon, game\ (your game's files, firmware
#          files, saves), eboot\, the desktop shortcut, and "Optimise for this PC"'s profile (work\pgo-setup\
#          setup-*.profdata, ~30 MB): an update compiles with it instead of training again while it still fits
# Both ways delete what a training left behind: work\port\build-train, the runs' save copies and counters.
# A later update then downloads the port's tools again (about 48 MB) and translates the game again. Never runs in
# the development folder (it has PLAN.md) unless -Force. Prints "freed: <GB>" for Setup.
#   tools\setup_cleanup.ps1 -KeepBuildCache   (what Setup does after a build, unless told otherwise)
# keeps the port's tools and the build files for the next update instead: tools-cache\slim (about 220 MB) and
# work\port\ (the translated game and its object files, about 1.5 GB). The next update then downloads only a tool
# that changed, skips every translation step whose inputs did not change and compiles only what changed: minutes.
param([switch]$Force, [switch]$KeepBuildCache)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ((Test-Path -LiteralPath (Join-Path $root "PLAN.md")) -and -not $Force) { "  not cleaning up: this is the development folder (it has PLAN.md)"; exit 0 }

. (Join-Path $PSScriptRoot "safe_paths.ps1")   # Invoke-Cmd
# Bytes under a folder, without following links (an old install's game\disc may be a junction to the player's game).
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
    try { Invoke-Cmd 'rd /s /q "%INF2_P1%"' @($Path) | Out-Null } finally { $ErrorActionPreference = $eap }
    if (Test-Path -LiteralPath $Path) { "  could not delete all of $Path (a file in use?)" }
}

$before = Get-TreeSize $root
if ($env:PORT_PGO_KEEP_TRAIN -ne "1") { Remove-Tree (Join-Path $root "work\port\build-train") }   # (a test keeps it)
foreach ($r in Get-ChildItem -LiteralPath (Join-Path $root "work\pgo-setup\runs") -Directory -ErrorAction SilentlyContinue) {
    Remove-Tree (Join-Path $r.FullName "gamedata")
    Get-ChildItem -LiteralPath $r.FullName -Filter "*.profraw" -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}
# leftovers of a download or an unpack (a stopped Setup)
foreach ($d in "tools-cache\slim\dl", "tools-cache\downloads", "work\sony-unpack", "game\disc.unpack", "game\pkg.unpack", "work\port-keep") {
    Remove-Tree (Join-Path $root $d)
}
if (-not $env:PORT_SONY_DOWNLOADS) { Remove-Tree (Join-Path $root "work\sony-downloads") }
foreach ($l in Get-ChildItem -LiteralPath (Join-Path $root "work\logs") -Filter "compile*.log" -ErrorAction SilentlyContinue) {
    try { Remove-Item -LiteralPath $l.FullName -Force -ErrorAction Stop } catch { "  could not delete $($l.Name)" }
}
$game = @("Infamous2Recomp.exe", "SDL2.dll", "Infamous2Game.dll", "Infamous2Recomp.pdb", "Infamous2Game.pdb")
if ($KeepBuildCache) {
    "  cleaning up: logs and leftovers; the port's tools and the build files stay for quick updates"
    $kept = 0L
    foreach ($d in "tools-cache", "work\port") { $kept += Get-TreeSize (Join-Path $root $d) }
    foreach ($f in $game) {   # the game itself, not a build file
        $p = Join-Path $root "work\port\build\$f"
        if (Test-Path -LiteralPath $p) { $kept -= (Get-Item -LiteralPath $p).Length }
    }
    $after = Get-TreeSize $root
    "  freed: {0:N1} GB; kept for quick updates: {1:N1} GB of tools and build files (tools\setup_cleanup.ps1 deletes them); the game folder uses {2:N1} GB" -f (($before - $after) / 1GB), ($kept / 1GB), ($after / 1GB)
    exit 0
}
"  cleaning up: the port's tools and what only the build needed"
foreach ($d in "tools-cache", "deps", "work\venv", "build") { Remove-Tree (Join-Path $root $d) }

# work\port: keep only the finished game (the engine and the game module)
$port = Join-Path $root "work\port"
$bld = Join-Path $port "build"
if (Test-Path -LiteralPath (Join-Path $bld "Infamous2Recomp.exe")) {
    $save = Join-Path $root "work\port-keep"
    Remove-Tree $save
    New-Item -ItemType Directory -Force $save | Out-Null
    foreach ($f in $game) { if (Test-Path -LiteralPath (Join-Path $bld $f)) { Move-Item -LiteralPath (Join-Path $bld $f) $save } }
    Remove-Tree $port
    New-Item -ItemType Directory -Force $bld | Out-Null
    foreach ($f in $game) { if (Test-Path -LiteralPath (Join-Path $save $f)) { Move-Item -LiteralPath (Join-Path $save $f) $bld } }
    Remove-Tree $save
} else { Remove-Tree $port }

$after = Get-TreeSize $root
"  freed: {0:N1} GB (the game folder now uses {1:N1} GB)" -f (($before - $after) / 1GB), ($after / 1GB)
