# The exe's icon from the player's own copy: PS3_GAME\ICON0.PNG (every PS3 game has one,
# 320x176) fitted into a square, as a multi-size .ico. No Sony art is kept in this repo:
# the build picks up work\icon\app.ico when it exists (tools\rebuild_win.py, step "port").
#   tools\make_icon.ps1 [-Icon0 <ICON0.PNG>]     default: game\disc\PS3_GAME\ICON0.PNG
param([string]$Icon0 = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Icon0) { $Icon0 = Join-Path $root "game\disc\PS3_GAME\ICON0.PNG" }
if (-not (Test-Path $Icon0)) { throw "no ICON0.PNG at $Icon0" }
Add-Type -AssemblyName System.Drawing
$outDir = Join-Path $root "work\icon"
New-Item -ItemType Directory -Force $outDir | Out-Null

$src = [System.Drawing.Image]::FromFile((Resolve-Path $Icon0).Path)
try {
    $pngs = @()
    foreach ($size in 256, 64, 48, 32, 16) {
        $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $scale = [Math]::Min($size / $src.Width, $size / $src.Height)
        $w = [int][Math]::Round($src.Width * $scale); $h = [int][Math]::Round($src.Height * $scale)
        $g.DrawImage($src, [int](($size - $w) / 2), [int](($size - $h) / 2), $w, $h)
        $g.Dispose()
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        if ($size -eq 256) { $bmp.Save((Join-Path $outDir "app.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
        $bmp.Dispose()
        $pngs += , @($size, $ms.ToArray())
    }
} finally { $src.Dispose() }

# .ico with PNG-compressed entries (Windows Vista and later read them)
$ico = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ico
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $s = $p[0]; $data = $p[1]
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$data.Length); $bw.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($p in $pngs) { $bw.Write([byte[]]$p[1]) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $outDir "app.ico"), $ico.ToArray())
"icon: $(Join-Path $outDir 'app.ico')"
