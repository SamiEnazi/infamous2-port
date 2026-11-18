# Where this PC keeps what the game needs (the PowerShell side of tools\port_paths.py).
#   . "$PSScriptRoot\port_paths.ps1";  Get-PortPath rpcs3
# Order: environment variable (PORT_RPCS3 ...), then config.local.json in the project
# folder (written by Setup.cmd, not in git).
. (Join-Path $PSScriptRoot "safe_paths.ps1")   # Get-BuildRoot, New-Junction, Invoke-Cmd (any folder name)
function Get-PortPath([string]$Key, [switch]$Optional) {
    $envName = @{ rpcs3 = "PORT_RPCS3"; vcvars = "PORT_VCVARS"; ninja = "PORT_NINJA"; llvm = "PORT_LLVM" }[$Key]
    $v = if ($envName) { [Environment]::GetEnvironmentVariable($envName) } else { $null }
    if (-not $v) {
        $cfg = Join-Path (Split-Path -Parent $PSScriptRoot) "config.local.json"
        if (Test-Path -LiteralPath $cfg) { $v = (Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).$Key }
    }
    if (-not $v -and -not $Optional) {
        throw "no '$Key' path set: run Setup.cmd (or set $envName, or `"$Key`" in config.local.json)"
    }
    $v
}

# --- where RPCS3 keeps dev_hdd0 and dev_flash (GitHub #50) -----------------------------------------
# RPCS3 reads config\vfs.yml: "/dev_hdd0/: <path>" and "/dev_flash/: <path>", each possibly on another
# drive, with "$(EmulatorDir)" standing for RPCS3's data folder (the folder of rpcs3.exe when portable,
# else %APPDATA%\rpcs3). An empty value means the default (<data folder>\dev_hdd0). Players who moved
# their game data to another drive have a vfs.yml like that; looking only next to rpcs3.exe missed it.
function Get-Rpcs3DataDirs([string]$Rpcs3) {
    $c = New-Object System.Collections.ArrayList
    foreach ($d in @($Rpcs3, $(if ($env:APPDATA) { Join-Path $env:APPDATA "rpcs3" }), $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "rpcs3" }))) {
        if ($d -and (Test-Path -LiteralPath $d -PathType Container) -and ($c -notcontains $d)) { [void]$c.Add($d) }
    }
    , $c.ToArray()
}

# The vfs.yml entry for "dev_hdd0" / "dev_flash" of one data folder as a full path, or $null (none / empty).
function Get-Rpcs3VfsPath([string]$DataDir, [string]$Name) {
    $yml = Join-Path $DataDir "config\vfs.yml"
    if (-not (Test-Path -LiteralPath $yml -PathType Leaf)) { return $null }
    foreach ($line in Get-Content -LiteralPath $yml -ErrorAction SilentlyContinue) {
        if ($line -notmatch '^\s*"?/?(\w+)/?"?\s*:\s*(.*?)\s*$' -or $Matches[1] -ne $Name) { continue }
        $v = $Matches[2].Trim().Trim('"', "'")
        if (-not $v) { return $null }
        $emu = $DataDir.TrimEnd('\', '/') + "\"
        if ($v.StartsWith('$(EmulatorDir)', [StringComparison]::OrdinalIgnoreCase)) { $v = $emu + $v.Substring(14).TrimStart('/', '\') }
        $v = $v.Replace('/', '\')
        if (-not [IO.Path]::IsPathRooted($v)) { $v = $emu + $v }
        return $v.TrimEnd('\')
    }
    $null
}

# RPCS3's dev_hdd0 / dev_flash. $Probe is a path inside it that proves it is the right one (the game's
# update folder, the firmware's liblv2.sprx): of the places RPCS3 may keep it (vfs.yml, the default
# beside rpcs3.exe or in %APPDATA%\rpcs3) the first holding it; none holds it = the first place.
function Find-Rpcs3Dir([string]$Rpcs3, [string]$Name, [string]$Probe) {
    $cands = New-Object System.Collections.ArrayList
    foreach ($d in (Get-Rpcs3DataDirs $Rpcs3)) {
        $v = Get-Rpcs3VfsPath $d $Name
        if ($v -and ($cands -notcontains $v)) { [void]$cands.Add($v) }
        $def = [IO.Path]::Combine($d, $Name)
        if ($cands -notcontains $def) { [void]$cands.Add($def) }
    }
    if ($cands.Count -eq 0) { return (Join-Path $Rpcs3 $Name) }
    if ($Probe) { foreach ($c in $cands) { if (Test-Path -LiteralPath ([IO.Path]::Combine($c, $Probe))) { return $c } } }
    $cands[0]
}

# The PS3 firmware files the game reads while it runs (fonts, system data): Setup's copy in
# game\dev_flash (then RPCS3 is no longer needed), else RPCS3's own dev_flash.
function Get-DevFlash {
    $own = Join-Path (Split-Path -Parent $PSScriptRoot) "game\dev_flash"
    if (Test-Path -LiteralPath (Join-Path $own "sys")) { return $own }
    $rp = Get-PortPath rpcs3 -Optional
    if (-not $rp) { return $own }   # (v0.6.0: Setup always puts them there; no RPCS3 path is kept)
    Find-Rpcs3Dir $rp "dev_flash" "sys\external\liblv2.sprx"
}

# --- DLC (GitHub #48, #56) -------------------------------------------------------------------------
# inFAMOUS 2's DLC is a set of small unlock files (26 for the EU disc) that the game opens by fixed, lower-case
# names in the folder its update lives in:  /dev_hdd0/game/<TITLE ID>/USRDIR/dlc/ulf/unlock_*.edat
# The skins and powers themselves are in the game's archives already. The port only COPIES these files into the
# install (never a link: the install stays portable, and the engine keeps a cache file beside each one):
#   Setup       from RPCS3's dev_hdd0\game\<TITLE ID>\USRDIR\dlc, on a first install and on every later run
#   the launcher  from the drop folder <install>\dlc (any *.edat, in it or in subfolders), before each start
# Nothing here reads more than the file's first 12 bytes (which kind of file it is).

# The *.edat files under $Dir (any depth); never the engine's own "<name>.edat.dec" cache files. Wrap in @().
function Get-DlcFiles([string]$Dir) {
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
    Get-ChildItem -LiteralPath $Dir -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '\.edat$' }
}

# The license kind in an unlock file's header: 3 = free (works as it is), 1 or 2 = bound to a license (.rap) the
# port can't use yet, 0 = not such a file.
function Get-DlcLicense([string]$File) {
    $h = New-Object byte[] 12; $n = 0
    try { $s = [IO.File]::OpenRead($File); try { $n = $s.Read($h, 0, 12) } finally { $s.Close() } } catch { return 0 }
    if ($n -lt 12 -or $h[0] -ne 0x4E -or $h[1] -ne 0x50 -or $h[2] -ne 0x44 -or $h[3] -ne 0) { return 0 }
    [int]$h[11]
}
# How many of these files need a license (.rap)
function Get-DlcNeedLicense($Files) { @($Files | Where-Object { $_ -and ((Get-DlcLicense $_.FullName) -in 1, 2) }).Count }

# One file, only when $Dst is missing or different: $true when it was copied. A replaced file's cache goes too.
function Copy-DlcFile([string]$Src, [string]$Dst) {
    if (Test-Path -LiteralPath $Dst -PathType Leaf) {
        $a = [IO.File]::ReadAllBytes($Src); $b = [IO.File]::ReadAllBytes($Dst)
        if ($a.Length -eq $b.Length -and [Convert]::ToBase64String($a) -ceq [Convert]::ToBase64String($b)) { return $false }
        Remove-Item -LiteralPath $Dst -Force   # (not written through: a PSN install's file may be a hard link)
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $Dst) | Out-Null
    Copy-Item -LiteralPath $Src -Destination $Dst -Force
    (Get-Item -LiteralPath $Dst -Force).Attributes = [IO.FileAttributes]::Normal
    Remove-Item -LiteralPath "$Dst.dec" -Force -ErrorAction SilentlyContinue
    $true
}

# A "dlc" folder (RPCS3's or a PS3's ...\USRDIR\dlc) into the install's, the same layout, lower-case names (the game
# opens lower-case). Returns how many files were copied; nothing in $To is ever deleted.
function Copy-DlcTree([string]$From, [string]$To) {
    $n = 0
    $base = (Resolve-Path -LiteralPath $From).Path.TrimEnd('\')
    foreach ($f in @(Get-DlcFiles $base)) {
        $rel = $f.FullName.Substring($base.Length + 1).ToLowerInvariant()
        if (Copy-DlcFile $f.FullName (Join-Path $To $rel)) { $n++ }
    }
    $n
}

# The drop folder's *.edat files (any depth) into ...\USRDIR\dlc\ulf, lower-case names. Returns how many were copied.
function Copy-DlcDrop([string]$Drop, [string]$Ulf) {
    $n = 0
    foreach ($f in @(Get-DlcFiles $Drop)) { if (Copy-DlcFile $f.FullName (Join-Path $Ulf $f.Name.ToLowerInvariant())) { $n++ } }
    $n
}

# The drop folder <install>\dlc with its README.txt (made by Setup and by the launcher; never emptied)
function New-DlcDropFolder([string]$Root) {
    $d = Join-Path $Root "dlc"
    if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Force $d | Out-Null }
    $note = Join-Path $d "README.txt"; $text = Join-Path $PSScriptRoot "dlc_readme.txt"
    if (-not (Test-Path -LiteralPath $note) -and (Test-Path -LiteralPath $text)) { Copy-Item -LiteralPath $text -Destination $note }
    $d
}

# The install's game folders in game\dev_hdd0\game that hold the game's update (BCES01143, NPUA80638 ...): where
# the game looks for its DLC. Normally one.
function Get-DlcTitleDirs([string]$Game) {
    Get-ChildItem -LiteralPath (Join-Path $Game "dev_hdd0\game") -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "*_INSTALL" -and (Test-Path -LiteralPath (Join-Path $_.FullName "USRDIR\EBOOT.BIN")) }
}
