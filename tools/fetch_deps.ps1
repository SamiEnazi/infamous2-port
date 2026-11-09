# Download the third-party pieces the build needs into deps\ (tools\build_deps.cmd then builds glslang).
#   deps\Vulkan-Headers   KhronosGroup/Vulkan-Headers v1.3.296         (Apache-2.0 / MIT)
#   deps\glslang-src      KhronosGroup/glslang 15.1.0                  (BSD-3 and others)
#   deps\SDL2-2.30.10     libsdl-org SDL2-devel-2.30.10-VC.zip         (zlib)
#   deps\sdl2-inc\SDL2    SDL2's headers again, under SDL2\ (the toolkit includes <SDL2/SDL.h>)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root "deps"
New-Item -ItemType Directory -Force $deps | Out-Null

function Get-Repo([string]$Url, [string]$Tag, [string]$Dir) {
    if (Test-Path (Join-Path $Dir ".git")) { "  $(Split-Path $Dir -Leaf): already there"; return }
    "  cloning $Url ($Tag)"
    & git -c advice.detachedHead=false clone --quiet --depth 1 --branch $Tag $Url $Dir
    if ($LASTEXITCODE) { throw "git clone $Url failed" }
}
Get-Repo "https://github.com/KhronosGroup/Vulkan-Headers.git" "v1.3.296" (Join-Path $deps "Vulkan-Headers")
Get-Repo "https://github.com/KhronosGroup/glslang.git" "15.1.0" (Join-Path $deps "glslang-src")

$sdl = Join-Path $deps "SDL2-2.30.10"
if (-not (Test-Path (Join-Path $sdl "cmake"))) {
    $dl = Join-Path $deps "sdl2-dl"
    New-Item -ItemType Directory -Force $dl | Out-Null
    $zip = Join-Path $dl "SDL2-devel-2.30.10-VC.zip"
    if (-not (Test-Path $zip)) {
        "  downloading SDL2 2.30.10"
        Invoke-WebRequest -UseBasicParsing -OutFile $zip `
            "https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-devel-2.30.10-VC.zip"
    }
    $h = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($h -ne "A9ADAB9A570A72D2E4F1565DCF0275B7D9EF7B487E05BA0CF7A7A0F35C79C5E7") {
        Remove-Item $zip; throw "SDL2 download has the wrong SHA-256 ($h): deleted, run again"
    }
    Expand-Archive $zip -DestinationPath $deps -Force
} else { "  SDL2-2.30.10: already there" }
$inc = Join-Path $deps "sdl2-inc\SDL2"
if (-not (Test-Path (Join-Path $inc "SDL.h"))) {
    New-Item -ItemType Directory -Force $inc | Out-Null
    Copy-Item (Join-Path $sdl "include\*") $inc
}
"deps ready"
