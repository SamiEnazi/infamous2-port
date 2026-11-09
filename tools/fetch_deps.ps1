# Download the third-party pieces the build needs into deps\ (tools\build_deps.cmd then builds glslang).
#   deps\Vulkan-Headers   KhronosGroup/Vulkan-Headers v1.3.296         (Apache-2.0 / MIT)
#   deps\glslang-src      KhronosGroup/glslang 15.1.0                  (BSD-3 and others)
#   deps\SDL2-2.30.10     libsdl-org SDL2-devel-2.30.10-VC.zip         (zlib)
#   deps\sdl2-inc\SDL2    SDL2's headers again, under SDL2\ (the toolkit includes <SDL2/SDL.h>)
# Only what Windows has: GitHub's ZIP archives of the two tags (no git), .NET for the download and
# the unpacking. "Already there" = the folder's .source file names the same tag (or an older
# Setup's git checkout).
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "setup_common.ps1")      # Save-Url
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root "deps"
New-Item -ItemType Directory -Force $deps | Out-Null

# A GitHub repo at a tag, as a ZIP, unpacked into $Dir without the archive's top folder.
function Get-RepoZip([string]$Repo, [string]$Tag, [string]$Dir) {
    $marker = Join-Path $Dir ".source"
    if ((Test-Path (Join-Path $Dir ".git")) -or ((Test-Path $marker) -and (Get-Content $marker -Raw).Trim() -eq "$Repo $Tag")) {
        "  $(Split-Path $Dir -Leaf): already there"; return
    }
    $zip = Join-Path $deps "$(Split-Path $Dir -Leaf)-$Tag.zip"
    Save-Url "https://codeload.github.com/$Repo/zip/refs/tags/$Tag" $zip "$Repo $Tag"
    $tmp = "$Dir.unpack"
    foreach ($d in $tmp, $Dir) { if (Test-Path $d) { cmd /c rd /s /q "$d" | Out-Null } }
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $tmp)
    $top = Get-ChildItem $tmp -Directory | Select-Object -First 1
    Move-Item $top.FullName $Dir
    cmd /c rd /s /q "$tmp" | Out-Null
    Remove-Item $zip
    Set-Content -Path $marker -Value "$Repo $Tag"
    "  $(Split-Path $Dir -Leaf): $Repo $Tag"
}
Get-RepoZip "KhronosGroup/Vulkan-Headers" "v1.3.296" (Join-Path $deps "Vulkan-Headers")
Get-RepoZip "KhronosGroup/glslang" "15.1.0" (Join-Path $deps "glslang-src")

$sdl = Join-Path $deps "SDL2-2.30.10"
if (-not (Test-Path (Join-Path $sdl "cmake"))) {
    $dl = Join-Path $deps "sdl2-dl"
    New-Item -ItemType Directory -Force $dl | Out-Null
    $zip = Join-Path $dl "SDL2-devel-2.30.10-VC.zip"
    if (-not (Test-Path $zip)) {
        Save-Url "https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-devel-2.30.10-VC.zip" $zip `
            "SDL2 2.30.10" "A9ADAB9A570A72D2E4F1565DCF0275B7D9EF7B487E05BA0CF7A7A0F35C79C5E7"
    }
    $h = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($h -ne "A9ADAB9A570A72D2E4F1565DCF0275B7D9EF7B487E05BA0CF7A7A0F35C79C5E7") {
        Remove-Item $zip; throw "SDL2 download has the wrong SHA-256 ($h): deleted, run again"
    }
    if (Test-Path $sdl) { cmd /c rd /s /q "$sdl" | Out-Null }      # a half-unpacked earlier try
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $deps)
} else { "  SDL2-2.30.10: already there" }
$inc = Join-Path $deps "sdl2-inc\SDL2"
if (-not (Test-Path (Join-Path $inc "SDL.h"))) {
    New-Item -ItemType Directory -Force $inc | Out-Null
    Copy-Item (Join-Path $sdl "include\*") $inc
}
"deps ready"
