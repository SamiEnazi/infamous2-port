# After the build: delete everything only the build needed, keep the game.
#   tools\setup_cleanup.ps1 [-Force]
# Deletes: tools-cache\ (the portable build tools), deps\ (SDL2, Vulkan headers, glslang sources and
#          build), work\venv\, work\port\ (the translated sources and the object files) except the
#          finished game, work\logs\compile*.log, build\ (cached compile settings)
# Keeps:   work\port\build\Infamous2Recomp.exe + SDL2.dll (+ its .pdb, for crash reports),
#          work\EBOOT.elf (the game reads it), work\icon, game\ (your game's files, firmware files, saves),
#          config.local.json, eboot\, the desktop shortcut
# A later update just downloads the tools again. Never runs in the development folder (it has
# PLAN.md) unless -Force. Prints "freed: <GB>" for Setup.
param([switch]$Force)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ((Test-Path (Join-Path $root "PLAN.md")) -and -not $Force) { "  not cleaning up: this is the development folder (it has PLAN.md)"; exit 0 }

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
"  cleaning up: the build tools and what only the build needed"
foreach ($d in "tools-cache", "deps", "work\venv", "build") { Remove-Tree (Join-Path $root $d) }
foreach ($l in Get-ChildItem (Join-Path $root "work\logs") -Filter "compile*.log" -ErrorAction SilentlyContinue) {
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
    foreach ($f in $keep) { if (Test-Path (Join-Path $bld $f)) { Move-Item (Join-Path $bld $f) $save } }
    Remove-Tree $port
    New-Item -ItemType Directory -Force $bld | Out-Null
    foreach ($f in $keep) { if (Test-Path (Join-Path $save $f)) { Move-Item (Join-Path $save $f) $bld } }
    Remove-Tree $save
} else { Remove-Tree $port }

$after = Get-TreeSize $root
"  freed: {0:N1} GB (the game folder now uses {1:N1} GB)" -f (($before - $after) / 1GB), ($after / 1GB)
