# The slim Setup's own tools (v0.6.0), instead of 1.4 GB of build tools: the prebuilt engine (Infamous2Recomp.exe +
# SDL2.dll: no game code in it), the official clang + lld (only what a module build needs), python.org's embeddable
# Python and PyPI's pycryptodome (tools\ps3tool.py is fast with it). About 48 MB, listed with their sizes and SHA-256
# in tools\slim_assets.json (made by tools\make_slim_assets.py) and attached to the GitHub release it names.
# Dot-sourced by tools\setup_build.ps1:
#   Install-SlimTools        each asset downloaded (unless its folder already holds that SHA-256), checked, unpacked
#                            into tools-cache\slim\<engine|llvm|python>; prints Setup's "  progress:" lines
#   Get-SlimPaths            @{ Engine = <exe>; Llvm = <dir>; Python = <exe>; Profdata = <exe> } (or $null each)
# Where from: https://github.com/SamiEnazi/infamous2-port/releases/download/<release>/<name>, or for tests and offline
# installs PORT_SETUP_RELEASES=<base url> (http://127.0.0.1:8765/releases/download), or PORT_SLIM_ASSETS=<folder
# holding the files>. A file is used only when its size and SHA-256 are the manifest's.

$script:SlimRepo = "SamiEnazi/infamous2-port"

function Get-SlimManifest {
    $root = Split-Path -Parent $PSScriptRoot
    $p = Join-Path $root "tools\slim_assets.json"
    if (-not (Test-Path -LiteralPath $p)) { throw "tools\slim_assets.json is missing: download the port again with Setup.exe" }
    Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
}

function Get-SlimDir([string]$Role) {
    $root = Split-Path -Parent $PSScriptRoot
    $sub = @{ engine = "engine"; toolchain = "llvm"; python = "python"; pycrypto = "python" }[$Role]
    Join-Path $root "tools-cache\slim\$sub"
}

function Get-SlimPaths {
    $e = Join-Path (Get-SlimDir engine) "Infamous2Recomp.exe"
    $l = Get-SlimDir toolchain
    $p = Join-Path (Get-SlimDir python) "python.exe"
    @{
        Engine   = $(if (Test-Path -LiteralPath $e) { $e } else { $null })
        Llvm     = $(if (Test-Path -LiteralPath (Join-Path $l "bin\clang.exe")) { $l } else { $null })
        Python   = $(if (Test-Path -LiteralPath $p) { $p } else { $null })
        Profdata = $(if (Test-Path -LiteralPath (Join-Path $l "bin\llvm-profdata.exe")) { Join-Path $l "bin\llvm-profdata.exe" } else { $null })
    }
}

# The mark of a finished unpack: <dir>\.slim-<role> holding the asset's SHA-256. $Same: SHA-256 of other files with
# the very same contents (v0.6.0's compiler was a .7z: an install that unpacked it does not download it again).
function Test-SlimReady([string]$Dir, [string]$Role, [string]$Sha, $Same = @()) {
    $m = Join-Path $Dir ".slim-$Role"
    if (-not (Test-Path -LiteralPath $m)) { return $false }
    $have = (Get-Content -LiteralPath $m -Raw).Trim()
    ($have -eq $Sha) -or (@($Same | ForEach-Object { ([string]$_).ToUpperInvariant() }) -contains $have)
}

# One archive unpacked into an empty folder, with nothing that can dead-end (GitHub #62, #70: Windows 10's tar.exe
# can't read LZMA, "LZMA codec is unsupported"; only Windows 11's can):
#   .zip / .whl      .NET's ZipFile (every Windows 10 and 11)
#   .tar.xz          the port's own Python (tarfile + lzma of its standard library: unpacked first, from a .zip),
#                    then a 7-Zip on this PC, then Windows' tar.exe
#   .7z              a 7-Zip on this PC, then Windows' tar.exe (Windows 11)
# PORT_SLIM_NO_TAR=1 leaves tar.exe out, PORT_SLIM_TAR=<program> names another one, PORT_SLIM_NO_7Z=1 leaves 7-Zip out
# (tests: a Windows 10 stand-in).
function Find-SevenZip {
    if ($env:PORT_SLIM_NO_7Z -eq "1") { return $null }
    foreach ($p in @($(if ($env:ProgramFiles) { Join-Path $env:ProgramFiles "7-Zip\7z.exe" }), $(if (${env:ProgramFiles(x86)}) { Join-Path ${env:ProgramFiles(x86)} "7-Zip\7z.exe" }))) {
        if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    }
    $c = Get-Command "7z.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $null
}
function Expand-SlimArchive([string]$File, [string]$Dir, [string]$Python = "") {
    $name = Split-Path -Leaf $File
    if ($name -like "*.zip" -or $name -like "*.whl") { Expand-Zip $File $Dir; return }
    $ways = @()
    if ($name -like "*.tar.xz" -and $Python -and (Test-Path -LiteralPath $Python)) { $ways += "python" }
    if (Find-SevenZip) { $ways += "7z" }
    $tar = if ($env:PORT_SLIM_TAR) { $env:PORT_SLIM_TAR } else { Join-Path $env:WINDIR "System32\tar.exe" }
    if ($env:PORT_SLIM_NO_TAR -ne "1" -and (Test-Path -LiteralPath $tar)) { $ways += "tar" }
    $why = @()
    foreach ($w in $ways) {
        # a failed try leaves nothing behind for the next one
        Get-ChildItem -LiteralPath $Dir -Force -ErrorAction SilentlyContinue | ForEach-Object { Remove-Tree $_.FullName }
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"   # (a native program's stderr line is no error)
        try {
            if ($w -eq "python") {
                $code = "import sys, tarfile; t = tarfile.open(sys.argv[1], 'r:xz'); t.extractall(sys.argv[2], filter='data'); t.close()"
                $o = & $Python -I -c $code $File $Dir 2>&1 | ForEach-Object { "$_" }
            } elseif ($w -eq "7z") {
                $z = Find-SevenZip
                if ($name -like "*.tar.xz") {
                    # two steps: the .xz, then the .tar in it
                    $tmp = "$Dir.tar"
                    New-Item -ItemType Directory -Force $tmp | Out-Null
                    $o = & $z x -y "-o$tmp" $File 2>&1 | ForEach-Object { "$_" }
                    if (-not $LASTEXITCODE) { $inner = Get-ChildItem -LiteralPath $tmp -File | Select-Object -First 1; $o = & $z x -y "-o$Dir" $inner.FullName 2>&1 | ForEach-Object { "$_" } }
                    $rc = $LASTEXITCODE
                    Remove-Tree $tmp
                    $global:LASTEXITCODE = $rc
                } else { $o = & $z x -y "-o$Dir" $File 2>&1 | ForEach-Object { "$_" } }
            } else { $o = & $tar -xf $File -C $Dir 2>&1 | ForEach-Object { "$_" } }
            $rc = $LASTEXITCODE
        } catch { $rc = 1; $o = @("$_") } finally { $ErrorActionPreference = $eap }
        if (-not $rc -and @(Get-ChildItem -LiteralPath $Dir -Force -ErrorAction SilentlyContinue).Count) {
            if ($why.Count) { "  ${name}: unpacked with $w" }
            return
        }
        $why += "$w`: $((@($o) | Where-Object { $_ } | Select-Object -Last 2) -join ' ')"
        "  ${name}: $w could not unpack it ($(($why | Select-Object -Last 1))); trying another way"
    }
    throw ("unpacking $name failed" + $(if ($why.Count) { " (" + ($why -join "; ") + ")" } else { ": nothing on this PC can unpack it" }) +
           ". Press Try again. If it fails again, install 7-Zip (7-zip.org) and press Try again: Setup then unpacks it with 7-Zip")
}

function Get-SlimAsset($A, [string]$Out) {
    $sha = ([string]$A.sha256).ToUpperInvariant()
    if ((Test-Path -LiteralPath $Out) -and (Get-Item -LiteralPath $Out).Length -eq [long]$A.size -and
        (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash -eq $sha) { return }
    if ($env:PORT_SLIM_ASSETS) {
        $local = Join-Path $env:PORT_SLIM_ASSETS $A.name
        if (Test-Path -LiteralPath $local) {
            "  $($A.name): from $env:PORT_SLIM_ASSETS"
            Copy-Item -LiteralPath $local -Destination $Out -Force
            if ((Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash -eq $sha) { return }
            Remove-Item -LiteralPath $Out -Force
            "  $($A.name) in $env:PORT_SLIM_ASSETS has the wrong SHA-256: downloading it instead"
        }
    }
    $rel = if ($A.release) { [string]$A.release } else { [string](Get-SlimManifest).release }
    $base = if ($env:PORT_SETUP_RELEASES) { $env:PORT_SETUP_RELEASES.TrimEnd('/') } else { "https://github.com/$script:SlimRepo/releases/download" }
    Save-Url "$base/$rel/$($A.name)" $Out $A.name $sha ([long]$A.size)
}

function Expand-Zip([string]$Zip, [string]$Dir) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Dir)
}

function Install-SlimTools {
    $man = Get-SlimManifest
    $root = Split-Path -Parent $PSScriptRoot
    $dl = Join-Path $root "tools-cache\slim\dl"
    New-Item -ItemType Directory -Force $dl | Out-Null
    $assets = @($man.assets)
    $want = @($assets | Where-Object { -not (Test-SlimReady (Get-SlimDir $_.role) $_.role ([string]$_.sha256).ToUpperInvariant() $_.same_as) })
    $total = ($want | Measure-Object -Property size -Sum).Sum
    if ($want.Count) { "  the port's tools: {0} file(s), {1:N1} MB from the port's GitHub releases ({2})" -f $want.Count, ($total / 1MB), ((@($want | ForEach-Object { if ($_.release) { [string]$_.release } else { [string]$man.release } }) | Sort-Object -Unique) -join ", ") }
    $doneBytes = 0L
    # Python first: it unpacks the compiler (.tar.xz), and pycryptodome goes into its folder; the engine last (the
    # build copies it beside the game)
    $order = @("python", "pycrypto", "toolchain", "engine")
    foreach ($role in $order) {
        foreach ($a in @($assets | Where-Object { $_.role -eq $role })) {
            $sha = ([string]$a.sha256).ToUpperInvariant()
            $dir = Get-SlimDir $role
            if (Test-SlimReady $dir $role $sha $a.same_as) { "  $($a.name): ready ($($a.role))"; continue }
            $file = Join-Path $dl $a.name
            try {
                Get-SlimAsset $a $file | ForEach-Object {
                    # Save-Url's own percentage is of this file: Setup's bar gets the share of all of them
                    if ("$_" -match '^\s*progress:\s*(\d+)%') {
                        $p = [int]$Matches[1]
                        "  progress: {0}% downloading the port's tools ({1})" -f [int][Math]::Min(99, 100 * ($doneBytes + $a.size * $p / 100) / [Math]::Max(1, $total)), $a.name
                    } else { $_ }
                }
            } catch {
                if ($a.optional) { "  note: $($a.name) could not be downloaded ($($_.Exception.Message)): ps3tool works without it, only slower"; continue }
                throw
            }
            $doneBytes += [long]$a.size
            "  {0}: {1:N1} MB, SHA-256 checked" -f $a.name, ($a.size / 1MB)
            if ($role -eq "pycrypto") {
                $site = Join-Path $dir "Lib\site-packages"
                New-Item -ItemType Directory -Force $site | Out-Null
                foreach ($old in Get-ChildItem -LiteralPath $site -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "Crypto" -or $_.Name -like "pycryptodome-*" }) { Remove-Tree $old.FullName }
                Expand-Zip $file $site
            } else {
                $new = "$dir.new"
                if (Test-Path -LiteralPath $new) { Remove-Tree $new }
                New-Item -ItemType Directory -Force $new | Out-Null
                Expand-SlimArchive $file $new (Join-Path (Get-SlimDir python) "python.exe")
                if ($role -eq "python") {
                    # with python3XX._pth the lifter's scripts can't import their sibling modules (no script folder on sys.path)
                    Get-ChildItem -LiteralPath $new -Filter "python*._pth" -File | Remove-Item -Force
                }
                if (Test-Path -LiteralPath $dir) { Remove-Tree $dir }
                for ($i = 0; ; $i++) {   # a virus scanner reading a just-unpacked file holds the folder for a moment
                    try { [IO.Directory]::Move($new, $dir); break } catch { if ($i -ge 60) { throw }; Start-Sleep -Milliseconds 500 }
                }
            }
            Set-Content -LiteralPath (Join-Path $dir ".slim-$role") -Value $sha -Encoding ascii
            Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $dl -Recurse -Force -ErrorAction SilentlyContinue
    $p = Get-SlimPaths
    if (-not $p.Engine) { throw "no engine after getting the port's tools (tools-cache\slim\engine\Infamous2Recomp.exe)" }
    if (-not $p.Llvm) { throw "no clang after getting the port's tools (tools-cache\slim\llvm\bin\clang.exe)" }
    if (-not $p.Python) { throw "no Python after getting the port's tools (tools-cache\slim\python\python.exe)" }
    "  progress: 100% the port's tools are ready"
}

# Before v0.6.0 Setup downloaded full build tools into tools-cache\ (Python, CMake, Ninja, LLVM, MSVC, the Windows SDK:
# about 2 GB) and built SDL2/glslang into deps\. The prebuilt engine needs none of it: deleted, with work\venv and the
# engine's old CMake build files in work\port\build (the lifted game in work\port stays: an update reuses it).
function Remove-OldBuildTools {
    $root = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $root "PLAN.md")) { return }   # never in the development folder
    $freed = 0L
    $gone = @()
    function Size([string]$p) { try { (Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum } catch { 0 } }
    $tc = Join-Path $root "tools-cache"
    foreach ($d in Get-ChildItem -LiteralPath $tc -Force -ErrorAction SilentlyContinue) {
        if ($d.Name -eq "slim") { continue }
        $freed += [long](Size $d.FullName); $gone += "tools-cache\$($d.Name)"
        Remove-Tree $d.FullName
    }
    foreach ($rel in "deps", "work\venv", "build") {
        $p = Join-Path $root $rel
        if (Test-Path -LiteralPath $p) { $freed += [long](Size $p); $gone += $rel; Remove-Tree $p }
    }
    $b = Join-Path $root "work\port\build"
    # the single exe's debug file (about 100 MB): the prebuilt engine has none, so one there is the old exe's
    $pdb = Join-Path $b "Infamous2Recomp.pdb"
    if ((Test-Path -LiteralPath $pdb) -and -not (Test-Path -LiteralPath (Join-Path $root "tools-cache\slim\engine\Infamous2Recomp.pdb"))) {
        $freed += (Get-Item -LiteralPath $pdb).Length; Remove-Item -LiteralPath $pdb -Force -ErrorAction SilentlyContinue; $gone += "the old exe's debug file"
    }
    foreach ($n in "CMakeFiles", "CMakeCache.txt", "build.ninja", ".ninja_deps", ".ninja_log", "cmake_install.cmake", "rules.ninja", "Infamous2Recomp.lib", "Infamous2Recomp.exp", "Infamous2Recomp.ilk") {
        $p = Join-Path $b $n
        if (Test-Path -LiteralPath $p) { $freed += [long](Size $p); if ((Get-Item -LiteralPath $p -Force) -is [IO.DirectoryInfo]) { Remove-Tree $p } else { Remove-Item -LiteralPath $p -Force }; if ($gone -notcontains "the engine's old build files") { $gone += "the engine's old build files" } }
    }
    if ($gone.Count) { "  the old build tools are not needed any more (the engine comes prebuilt): deleted {0} ({1:N1} GB)" -f ($gone -join ", "), ($freed / 1GB) }
}
