# Shared by Setup (tools\setup.ps1, Setup.exe) and the scripts it runs: reading a PARAM.SFO, the
# list of checks (this PC, the port's tools, the player's game, its update and firmware files), downloads, tool paths.
. (Join-Path $PSScriptRoot "safe_paths.ps1")   # Invoke-Cmd, New-Junction, Remove-Tree, Get-BuildRoot (any folder name)

. (Join-Path $PSScriptRoot "port_paths.ps1")   # Find-Rpcs3Dir (RPCS3's vfs.yml, GitHub #50)

# PARAM.SFO -> hashtable (strings and integers)
function Read-Sfo([string]$Path) {
    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 20 -or $b[0] -ne 0 -or $b[1] -ne 0x50 -or $b[2] -ne 0x53 -or $b[3] -ne 0x46) { throw "$Path is not a PARAM.SFO" }
    $keys = [BitConverter]::ToUInt32($b, 8); $data = [BitConverter]::ToUInt32($b, 12); $n = [BitConverter]::ToUInt32($b, 16)
    $out = @{}
    for ($i = 0; $i -lt $n; $i++) {
        $e = 20 + 16 * $i
        $ko = [BitConverter]::ToUInt16($b, $e); $fmt = [BitConverter]::ToUInt16($b, $e + 2)
        $len = [BitConverter]::ToUInt32($b, $e + 4); $off = [BitConverter]::ToUInt32($b, $e + 12)
        $k = $keys + $ko; $end = $k; while ($b[$end] -ne 0) { $end++ }
        $name = [System.Text.Encoding]::ASCII.GetString($b, $k, $end - $k)
        if ($fmt -eq 0x0404) { $out[$name] = [BitConverter]::ToUInt32($b, $data + $off) }
        else { $out[$name] = [System.Text.Encoding]::UTF8.GetString($b, $data + $off, $len).TrimEnd([char]0) }
    }
    $out
}

if (-not ("PortSetup.Cpu" -as [type])) {
    Add-Type -Namespace PortSetup -Name Cpu -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint f);'
}

# The game's title IDs whose 1.04 code is the one this port translates (same PPU hash).
$script:KnownTitles = @{
    "BCES01143" = "Europe disc"; "BCES01144" = "Europe disc"; "BCES01229" = "Turkey disc";
    "BCUS98125" = "USA disc"; "NPUA80638" = "USA PSN (digital)"; "NPEA00318" = "Europe PSN (digital)"
}
# Players have built and played these (README: EU disc, US disc, Turkey disc, the PSN version 01.03); the others
# have the same code but nobody has reported on them yet.
$script:TestedTitles = @("BCES01143", "BCES01229", "BCUS98125", "NPUA80638")
# The discs whose update packages tools\sony_files.py knows (it downloads them from Sony's servers when RPCS3 has none).
$script:SonyUpdates = @("BCES01143", "BCES01144", "BCES01229", "BCUS98125")

# A build tool's path, the way tools\port_paths.py finds it for Setup: environment variable
# (PORT_PYTHON ...), then config.local.json, then tools-cache\ (fetch_tools.ps1). PORT_ONLY_CACHE=1:
# only the environment variable and tools-cache. Setup never uses tools found on PATH: it downloads
# its own, so every player builds with the same versions. $null when not there (yet).
function Get-PortTool([string]$Key) {
    $root = Split-Path -Parent $PSScriptRoot
    $v = [Environment]::GetEnvironmentVariable("PORT_" + $Key.ToUpper())
    if (-not $v -and $env:PORT_ONLY_CACHE -ne "1") {
        $cfg = Join-Path $root "config.local.json"
        if (Test-Path -LiteralPath $cfg) { try { $v = (Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).$Key } catch {} }
    }
    if (-not $v) {
        $cache = @{ python = "python\python.exe"; cmake = "cmake\bin\cmake.exe"; ninja = "ninja\ninja.exe"; llvm = "llvm"; msvc = "msvc"; sdk = "sdk" }[$Key]
        if ($cache) {
            $dir = Join-Path (Join-Path $root "tools-cache") ($cache -split '\\')[0]
            if (Test-Path -LiteralPath (Join-Path $dir ".ready")) { $v = Join-Path (Join-Path $root "tools-cache") $cache }
        }
    }
    $v
}

# Download a file: progress lines ("  progress: 45% <what> (12 of 27 MB)"), retried once, checked
# against a SHA-256 when one is given. Only what Windows has (.NET's HttpWebRequest).
function Save-Url([string]$Url, [string]$Out, [string]$What, [string]$Sha256 = "", [long]$Size = 0) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    for ($try = 1; $try -le 2; $try++) {
        try {
            $req = [Net.HttpWebRequest]::Create($Url)
            $req.UserAgent = "inFAMOUS2-port-setup"; $req.Timeout = 60000; $req.ReadWriteTimeout = 120000
            $resp = $req.GetResponse()
            $total = if ($resp.ContentLength -gt 0) { $resp.ContentLength } else { $Size }
            $in = $resp.GetResponseStream(); $fs = [IO.File]::Create("$Out.part")
            try {
                $buf = New-Object byte[] 1048576; $done = 0L; $last = Get-Date
                while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                    $fs.Write($buf, 0, $n); $done += $n
                    if (((Get-Date) - $last).TotalSeconds -ge 1) {
                        $last = Get-Date
                        if ($total -gt 0) { "  progress: {0}% {1} ({2:N0} of {3:N0} MB)" -f [int][Math]::Min(99, 100 * $done / $total), $What, ($done / 1MB), ($total / 1MB) }
                        else { "  {0}: {1:N0} MB" -f $What, ($done / 1MB) }
                    }
                }
            } finally { $fs.Close(); $in.Close(); $resp.Close() }
            if ($Sha256) {
                $h = (Get-FileHash -LiteralPath "$Out.part" -Algorithm SHA256).Hash
                if ($h -ne $Sha256) { Remove-Item -LiteralPath "$Out.part"; throw "$What has the wrong SHA-256 ($h)" }
            }
            Move-Item -Force -LiteralPath "$Out.part" -Destination $Out
            return
        } catch {
            if ($try -eq 2) { throw "downloading $What failed: $($_.Exception.Message). Check the internet connection and try again." }
            "  ${What}: $($_.Exception.Message) -- trying once more"
            Start-Sleep -Seconds 3
        }
    }
}

# $null when the drive of $Path can hold a junction (local NTFS or ReFS), else what the drive is:
# "an exFAT drive", "a FAT32 drive", "a network drive", ... (GitHub issue #2).
function Get-DriveProblem([string]$Path) {
    try {
        $full = [System.IO.Path]::GetFullPath($Path)
        $rootPath = [System.IO.Path]::GetPathRoot($full)
        if (-not $rootPath -or $rootPath.StartsWith("\\")) { return "a network drive" }
        $di = New-Object System.IO.DriveInfo $rootPath
        if ($di.DriveType -eq [System.IO.DriveType]::Network) { return "a network drive" }
        if (-not $di.IsReady) { return "a drive that is not ready" }
        if ($di.DriveFormat -in @("NTFS", "ReFS")) { return $null }
        $fmt = $di.DriveFormat
        $an = if ($fmt -match '^[AEIOU]') { "an" } else { "a" }
        return "$an $fmt drive"
    } catch { return "a drive Setup could not read" }
}

# The port's folder as the player sees it: setup_build.ps1 may run through a junction with a plain name
# (tools\safe_paths.ps1 Get-BuildRoot) and then says where the real folder is (its drive's free space, its file system).
function Get-RealRoot {
    if ($env:PORT_REAL_ROOT) { return $env:PORT_REAL_ROOT }
    Split-Path -Parent $PSScriptRoot
}

# The game's two layouts (GitHub #40):
#   a disc        <folder>\PS3_GAME\PARAM.SFO, PS3_GAME\USRDIR\...           (BCES01143, BCUS98125, ...)
#   a digital     <TITLE ID>\PARAM.SFO, <TITLE ID>\USRDIR\EBOOT.BIN           (NPUA80638, NPEA00318: the PSN version,
#   (PSN) copy    in RPCS3's dev_hdd0\game\<ID>, or wherever the player copied it; no PS3_GAME folder)
# Get-GameRoot: the folder that plays PS3_GAME's part (PS3_GAME itself, or the digital title folder), or $null.
function Get-GameRoot([string]$Disc) {
    if (-not $Disc) { return $null }
    try {
        $pg = Join-Path $Disc "PS3_GAME"
        if (Test-Path -LiteralPath (Join-Path $pg "PARAM.SFO") -PathType Leaf) { return $pg }
        # (a PSN title only: a disc game's update folder in RPCS3 has PARAM.SFO and an EBOOT too, but not the game)
        if ((Test-Path -LiteralPath (Join-Path $Disc "PARAM.SFO") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $Disc "USRDIR\EBOOT.BIN") -PathType Leaf) -and
            [string](Read-Sfo (Join-Path $Disc "PARAM.SFO"))["TITLE_ID"] -like "NP*") { return $Disc }
    } catch {}
    $null
}
function Test-DigitalFolder([string]$Disc) {
    $g = Get-GameRoot $Disc
    $g -and -not (Test-Path -LiteralPath (Join-Path $Disc "PS3_GAME\PARAM.SFO"))
}

# What the player chose, made into a game folder: the disc folder, the PS3_GAME folder itself, a digital title
# folder, the folder above either (e.g. RPCS3's dev_hdd0\game, or a folder holding NPUA80638), or RPCS3's own
# folder (its dev_hdd0\game\<ID>). The chosen path itself when none of those fit (the checks then say why).
function Resolve-GameFolder([string]$Path, [string]$Rpcs3 = "") {
    if (-not $Path) { return $Path }
    $p = $Path.TrimEnd('\')
    if ((Split-Path -Leaf $p) -eq "PS3_GAME" -and (Test-Path -LiteralPath (Join-Path $p "PARAM.SFO"))) { return (Split-Path -Parent $p) }
    if (Get-GameRoot $p) { return $p }
    $ids = @($script:KnownTitles.Keys | Sort-Object)
    foreach ($id in $ids) { $c = Join-Path $p $id; if (Get-GameRoot $c) { return $c } }
    $hdd = Get-Rpcs3Dir $p "dev_hdd0"
    if ($hdd) { foreach ($id in $ids) { $c = Join-Path $hdd "game\$id"; if (Get-GameRoot $c) { return $c } } }
    try {
        foreach ($d in Get-ChildItem -LiteralPath $p -Directory -ErrorAction Stop) { if (Get-GameRoot $d.FullName) { return $d.FullName } }
    } catch {}
    $p
}

# One of RPCS3's PS3 folders (dev_hdd0, dev_flash): where its config\vfs.yml puts it (RPCS3 lets the player move
# them, e.g. "/dev_hdd0/: D:/PS3/dev_hdd0/"), else <RPCS3>\<name>. $null when $Rpcs3 is not RPCS3's folder.
function Get-Rpcs3Dir([string]$Rpcs3, [string]$Name, [string]$Probe) {
    if (-not $Rpcs3 -or -not (Test-Path -LiteralPath (Join-Path $Rpcs3 "rpcs3.exe"))) { return $null }
    # (GitHub #50, #46) each RPCS3 data folder's config\vfs.yml, the default beside rpcs3.exe, %APPDATA%\rpcs3: the
    # first that holds $Probe (tools\port_paths.ps1 Find-Rpcs3Dir); else the $(EmulatorDir) reading below
    if (-not $Probe) { $Probe = if ($Name -eq "dev_flash") { "sys\external\liblv2.sprx" } else { "game" } }
    $found = Find-Rpcs3Dir $Rpcs3 $Name $Probe
    if ($found -and (Test-Path -LiteralPath ([IO.Path]::Combine($found, $Probe)))) { return $found }
    $dir = Join-Path $Rpcs3 $Name
    foreach ($y in (Join-Path $Rpcs3 "config\vfs.yml"), (Join-Path $Rpcs3 "vfs.yml")) {
        if (-not (Test-Path -LiteralPath $y)) { continue }
        try {
            $emu = ""; $val = $null
            foreach ($l in [IO.File]::ReadAllLines($y)) {
                if ($l -match '^\s*\$\(EmulatorDir\)\s*:\s*"?([^"]*)"?\s*$') { $emu = $Matches[1].Trim() }
                if ($l -match ('^\s*"?/' + [regex]::Escape($Name) + '/?"?\s*:\s*"?([^"]*)"?\s*$')) { $val = $Matches[1].Trim() }
            }
            if ($val) {
                $base = if ($emu) { $emu } else { $Rpcs3 + "\" }
                $v = $val.Replace('$(EmulatorDir)', $base).Replace('/', '\').TrimEnd('\')
                if (-not [IO.Path]::IsPathRooted($v)) { $v = Join-Path $Rpcs3 $v }
                if (Test-Path -LiteralPath $v) { $dir = $v }
            }
        } catch {}
        break
    }
    $dir
}

# RPCS3's folder when the game folder is inside one (...\dev_hdd0\game\<ID>), else $null: the RPCS3 that holds a
# digital game is the one with its license.
function Get-Rpcs3OfGame([string]$Disc) {
    try {
        $d = [IO.Path]::GetFullPath($Disc).TrimEnd('\')
        $g = Split-Path -Parent $d; $h = Split-Path -Parent $g; $r = Split-Path -Parent $h
        if ((Split-Path -Leaf $g) -eq "game" -and (Split-Path -Leaf $h) -eq "dev_hdd0" -and (Test-Path -LiteralPath (Join-Path $r "rpcs3.exe"))) { return $r }
    } catch {}
    $null
}

# What tools\ps3tool.py offers (pkg, pup, decrypt, iso; an option as <mode>--<option>, e.g. iso--dkey; "psn-rap" when
# it unlocks a PSN copy with the player's .rap, which it looks up itself: PS3_RAP_DIR), read from its own --help by
# tools\game_source.py modes. $null when it can't be asked yet (a first install: the Python that
# runs it comes with the port's tools). Setup asks before it asks the player for a disc key or a license: a file
# this ps3tool can't use is never asked for.
function Get-Ps3ToolModes {
    $r = Split-Path -Parent $PSScriptRoot
    $tool = Join-Path $r "tools\ps3tool.py"
    $py = $null
    foreach ($p in (Join-Path $r "tools-cache\slim\python\python.exe"), (Join-Path $r "work\venv\Scripts\python.exe"), (Join-Path $r "tools-cache\python\python.exe")) {
        if (Test-Path -LiteralPath $p) { $py = $p; break }
    }
    if (-not $py -or -not (Test-Path -LiteralPath $tool)) { return $null }
    $o = @()
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { $o = @(& $py -I (Join-Path $PSScriptRoot "game_source.py") modes --ps3tool $tool --python $py 2>&1 | ForEach-Object { "$_" }) } catch { return $null } finally { $ErrorActionPreference = $eap }
    foreach ($l in $o) { if ($l -match '^modes:\s*(.*)$') { return , @($Matches[1].Trim() -split '\s+' | Where-Object { $_ }) } }
    $null
}
# What Setup says when this version can't do it (the same words as tools\game_source.py's ISO_CANT)
# (only with an older tools\ps3tool.py, one without the iso mode: the current one does both)
$script:IsoCant = "this disc image is encrypted (a raw copy of the disc), and the ps3tool in this folder is an older one that can't decrypt disc images. Run Setup again to get the newest port, or choose your game's decrypted folder (the one that holds PS3_GAME) or a decrypted .iso instead"
function Get-PsnCant([string]$Title) {
    "the ps3tool in this folder is an older one that can't unlock the digital (PSN) version with a license (.rap) file. Run Setup again to get the newest port. (With that older tool only RPCS3 holding the game and your license can unlock it: choose RPCS3's dev_hdd0\game\$Title folder then.)"
}
# A disc key ps3tool finds by itself: <name>.dkey / .key / .ird beside the image. Only the file's name is looked at.
function Find-DiscKeyNear([string]$Iso) {
    if (-not $Iso -or -not (Test-Path -LiteralPath $Iso -PathType Leaf)) { return $null }
    $d = Split-Path -Parent $Iso; $n = [IO.Path]::GetFileNameWithoutExtension($Iso)
    foreach ($ext in ".dkey", ".key", ".ird") { $f = Join-Path $d ($n + $ext); if (Test-Path -LiteralPath $f -PathType Leaf) { return $f } }
    $null
}

# The firmware RPCS3 has: "4.93" from dev_flash\vsh\etc\version.txt ("release:04.9300:"), or $null.
function Get-FirmwareVersion([string]$DevFlash) {
    try {
        foreach ($l in [IO.File]::ReadAllLines((Join-Path $DevFlash "vsh\etc\version.txt"))) {
            if ($l -match '^release:(\d+)\.(\d\d)') { return "{0}.{1}" -f [int]$Matches[1], $Matches[2] }
        }
    } catch {}
    $null
}

# A digital copy's license in RPCS3 (GitHub #40): RPCS3 unlocks the PSN game's EBOOT with the player's own
# license, a <CONTENT_ID>.rap (File > Install Packages/Raps/Edats) or a .rif with act.dat in
# dev_hdd0\home\<user>\exdata. Only the file NAMES are looked at, never what is in them. The license file's
# path, or $null.
function Find-DigitalLicense([string]$Rpcs3, [string]$ContentId) {
    $hdd = Get-Rpcs3Dir $Rpcs3 "dev_hdd0"
    if (-not $hdd -or -not $ContentId) { return $null }
    foreach ($u in Get-ChildItem -LiteralPath (Join-Path $hdd "home") -Directory -ErrorAction SilentlyContinue) {
        foreach ($ext in ".rap", ".rif") {
            $f = Join-Path $u.FullName ("exdata\" + $ContentId + $ext)
            if (Test-Path -LiteralPath $f -PathType Leaf) { return $f }
        }
    }
    $null
}

# A digital (PSN) copy used to need a PS3_GAME folder made by hand (README before v0.5.6): PARAM.SFO,
# ICON0.PNG and USRDIR copied from RPCS3's dev_hdd0\game\<ID>. The game files of RPCS3's USRDIR
# (archives, movies, images: what the game ships, not what it writes there itself) missing from that
# folder, as USRDIR-relative paths; none = the copy is whole.
function Get-DigitalCopyGaps([string]$Disc, [string]$GameDir) {
    $src = Join-Path $GameDir "USRDIR"; $dst = Join-Path $Disc "PS3_GAME\USRDIR"
    if (-not (Test-Path -LiteralPath $src)) { return }
    $ship = @(".psarc", ".psarc_s", ".bik", ".png")
    foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File -ErrorAction SilentlyContinue) {
        if ($ship -notcontains $f.Extension.ToLowerInvariant()) { continue }
        $rel = $f.FullName.Substring($src.Length).TrimStart('\')
        if (-not (Test-Path -LiteralPath (Join-Path $dst $rel))) { $rel }
    }
}

# The game archives every copy has, each starting with "PSAR" (GitHub #9/#13, 2026-11-13: a copy whose disc files
# were still encrypted, and one without core/music.psarc, both ended in a black screen after the intro). Disc copies
# also carry the two HDD-install archives. Returns a plain-language problem, or $null.
function Get-GameFileProblems([string]$Disc, [string]$Title) {
    $gr = Get-GameRoot $Disc
    if (-not $gr) { $gr = Join-Path $Disc "PS3_GAME" }
    $dir = Join-Path $gr "USRDIR\cache\psarc"
    $where = if ($gr -eq $Disc) { "USRDIR\cache\psarc" } else { "PS3_GAME\USRDIR\cache\psarc" }
    $need = @("core.psarc", "music.psarc")
    $opt = @("install1\infamous1.psarc_s", "install2\infamous2.psarc_s")
    if ($Title -like "BC*") { $need += $opt; $opt = @() }
    $missing = @(); $bad = @()
    foreach ($f in $need + $opt) {
        $p = Join-Path $dir $f
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { if ($need -contains $f) { $missing += $f }; continue }
        $h = New-Object byte[] 4
        try { $s = [IO.File]::OpenRead($p); try { [void]$s.Read($h, 0, 4) } finally { $s.Close() } } catch { $bad += $f; continue }
        if ([Text.Encoding]::ASCII.GetString($h) -ne "PSAR") { $bad += $f }
    }
    if ($missing) { return "your game folder is missing some of the game's files ($($missing -join ', ') in $where). Copy the complete game folder again, then run Setup again" }
    if ($bad) { return "your game's files can't be read ($($bad -join ', ') doesn't start like a game archive): the copy is still encrypted or damaged. The port needs a decrypted copy of the game folder, the same kind RPCS3 uses for a disc game" }
    $null
}

# A digital game's own folder (PARAM.SFO + USRDIR\EBOOT.BIN): the folder chosen when it is one; else, for a
# PS3_GAME folder (the way of v0.5.5 and older: made by hand from RPCS3's copy, or RPCS3's own folder renamed,
# GitHub #40), RPCS3's dev_hdd0\game\<ID> when it is there, else that PS3_GAME folder when it has the EBOOT.
function Get-DigitalGameDir([string]$Disc, [string]$Rpcs3, [string]$Title) {
    if (Test-DigitalFolder $Disc) { return $Disc }
    $hdd = Get-Rpcs3Dir $Rpcs3 "dev_hdd0"
    if ($hdd -and $Title) {
        $d = Join-Path $hdd "game\$Title"
        if (Test-Path -LiteralPath (Join-Path $d "PARAM.SFO")) { return $d }
    }
    $gr = Get-GameRoot $Disc
    if ($gr -and (Test-Path -LiteralPath (Join-Path $gr "USRDIR\EBOOT.BIN"))) { return $gr }
    $null
}

# --- Setup's one pick (v0.6.0): a game folder, a disc image (.iso) or a PSN package (.pkg) -----------------------
# What PowerShell can tell before Python is there: the kind, the title and its PARAM.SFO (an image: read with a
# small ISO 9660 reader, the same as tools\game_source.py's), whether an image's files are still encrypted.
function Read-IsoBytes([IO.FileStream]$Fs, [long]$Off, [int]$N) {
    $b = New-Object byte[] $N
    $Fs.Position = $Off
    $got = 0
    while ($got -lt $N) { $k = $Fs.Read($b, $got, $N - $got); if ($k -le 0) { break }; $got += $k }
    if ($got -lt $N) { if ($got -le 0) { return , (New-Object byte[] 0) }; $c = New-Object byte[] $got; [Array]::Copy($b, $c, $got); return , $c }
    , $b
}

# @{ Lba; Size; Dir } of <rel> ("PS3_GAME/PARAM.SFO") in a disc image, or $null
function Find-IsoEntry([string]$Iso, [string]$Rel) {
    $fs = [IO.File]::OpenRead($Iso)
    try {
        $root = $null; $joliet = $false
        for ($lba = 16; $lba -lt 64; $lba++) {
            $d = Read-IsoBytes $fs ($lba * 2048) 2048
            if ($d.Length -lt 2048 -or [Text.Encoding]::ASCII.GetString($d, 1, 5) -ne "CD001") { break }
            if ($d[0] -eq 255) { break }
            if ($d[0] -eq 1 -and -not $root) { $root = @{ Lba = [BitConverter]::ToUInt32($d, 158); Size = [BitConverter]::ToUInt32($d, 166) } }
            elseif ($d[0] -eq 2 -and $d[88] -eq 0x25 -and $d[89] -eq 0x2F -and @(0x40, 0x43, 0x45) -contains $d[90]) {
                $root = @{ Lba = [BitConverter]::ToUInt32($d, 158); Size = [BitConverter]::ToUInt32($d, 166) }; $joliet = $true
            }
        }
        if (-not $root) { return $null }
        $cur = $root
        $parts = @($Rel -split '/' | Where-Object { $_ })
        for ($i = 0; $i -lt $parts.Count; $i++) {
            $data = Read-IsoBytes $fs ([long]$cur.Lba * 2048) ([int][Math]::Min($cur.Size, 4MB))
            $pos = 0; $hit = $null
            while ($pos -lt $data.Length) {
                $ln = $data[$pos]
                if ($ln -eq 0) { $pos = ([Math]::Floor($pos / 2048) + 1) * 2048; continue }
                $nl = $data[$pos + 32]
                $name = if ($joliet) { [Text.Encoding]::BigEndianUnicode.GetString($data, $pos + 33, $nl) } else { [Text.Encoding]::ASCII.GetString($data, $pos + 33, $nl) }
                $name = ($name -split ';')[0].TrimEnd('.')
                if ($name -ieq $parts[$i]) {
                    $hit = @{ Lba = [BitConverter]::ToUInt32($data, $pos + 2); Size = [BitConverter]::ToUInt32($data, $pos + 10); Dir = [bool]($data[$pos + 25] -band 2) }
                    break
                }
                $pos += $ln
            }
            if (-not $hit) { return $null }
            if ($i -lt $parts.Count - 1 -and -not $hit.Dir) { return $null }
            $cur = $hit
        }
        $cur
    } finally { $fs.Close() }
}

function Read-IsoHead([string]$Iso, [string]$Rel, [int]$N) {
    $e = Find-IsoEntry $Iso $Rel
    if (-not $e -or $e.Dir) { return $null }
    $fs = [IO.File]::OpenRead($Iso)
    try { , (Read-IsoBytes $fs ([long]$e.Lba * 2048) ([int][Math]::Min($N, $e.Size))) } finally { $fs.Close() }
}

# PARAM.SFO bytes -> hashtable, or $null (Read-Sfo's format)
function Read-SfoBytes([byte[]]$B) {
    if (-not $B -or $B.Length -lt 20 -or $B[0] -ne 0 -or $B[1] -ne 0x50 -or $B[2] -ne 0x53 -or $B[3] -ne 0x46) { return $null }
    $tmp = [IO.Path]::GetTempFileName()
    try { [IO.File]::WriteAllBytes($tmp, $B); Read-Sfo $tmp } catch { $null } finally { Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue }
}

# What the player picked: Kind folder|iso|pkg, Layout disc|psn, Title, Sfo, Encrypted, Problem, Folder (a folder's
# game folder after Resolve-GameFolder)
function Get-GameSource([string]$Path, [string]$Rpcs3 = "") {
    $o = [pscustomobject]@{ Kind = ""; Layout = ""; Title = ""; Sfo = $null; Encrypted = $false; Problem = ""; Folder = ""; ContentId = "" }
    if (-not $Path) { $o.Problem = "no game chosen"; return $o }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $o.Kind = "folder"
        $f = Resolve-GameFolder $Path $Rpcs3
        $o.Folder = $f
        $gr = Get-GameRoot $f
        if (-not $gr) {
            $iso = Get-ChildItem -LiteralPath $Path -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in ".iso", ".pkg" } | Select-Object -First 1 -ExpandProperty FullName
            $o.Problem = if ($iso) { "no game folder there, but a game file: choose $iso itself" }
                         else { "no game in ${Path}: choose the folder that has PS3_GAME in it (a disc), the PSN version's NPUA80638 / NPEA00318 folder, an .iso or a .pkg file" }
            return $o
        }
        $o.Layout = if (Test-DigitalFolder $f) { "psn" } else { "disc" }
        try { $o.Sfo = Read-Sfo (Join-Path $gr "PARAM.SFO"); $o.Title = [string]$o.Sfo["TITLE_ID"] } catch { $o.Problem = "its PARAM.SFO can't be read" }
        return $o
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { $o.Problem = "$Path is not there"; return $o }
    $h = New-Object byte[] 0x60
    try { $s = [IO.File]::OpenRead($Path); try { [void]$s.Read($h, 0, $h.Length) } finally { $s.Close() } } catch { $o.Problem = "$Path can't be read"; return $o }
    if ($h[0] -eq 0x7F -and [Text.Encoding]::ASCII.GetString($h, 1, 3) -eq "PKG") {
        $o.Kind = "pkg"; $o.Layout = "psn"; $o.Encrypted = $true
        $o.ContentId = ([Text.Encoding]::ASCII.GetString($h, 0x30, 0x24)).TrimEnd([char]0)
        if ($o.ContentId.Length -ge 16) { $o.Title = $o.ContentId.Substring(7, 9) }
        if ($o.Title -like "BC*") { $o.Problem = "this is an update or DLC package of the disc version ($($o.ContentId)), not the game: choose your game (the disc's folder or .iso) instead. Setup gets the updates by itself. For DLC: install the package in RPCS3 (File > Install Packages) and run Setup again, it copies the DLC from there; or put the DLC's unlock files (*.edat) in the port's dlc folder" }
        return $o
    }
    if ($Path -match '\.pkg$') { $o.Kind = "pkg"; $o.Problem = "this is not a PS3 package"; return $o }
    $o.Kind = "iso"; $o.Layout = "disc"
    try {
        $sfoB = Read-IsoHead $Path "PS3_GAME/PARAM.SFO" 65536
        if ($null -eq $sfoB) {
            $o.Problem = if (Find-IsoEntry $Path "PS3_GAME") { "this disc image's PS3_GAME has no PARAM.SFO" } elseif ((Find-IsoEntry $Path "")) { "this disc image has no PS3_GAME folder: it is not a PS3 game disc" } else { "this file is not a disc image Setup can read (an ISO 9660 .iso)" }
            return $o
        }
        $o.Sfo = Read-SfoBytes $sfoB
        $core = Read-IsoHead $Path "PS3_GAME/USRDIR/cache/psarc/core.psarc" 4
        $o.Encrypted = (-not $o.Sfo) -or ($core -and [Text.Encoding]::ASCII.GetString($core) -ne "PSAR")
        if ($o.Sfo) { $o.Title = [string]$o.Sfo["TITLE_ID"] }
    } catch { $o.Problem = "this disc image can't be read ($($_.Exception.Message))" }
    $o
}

# A PSN copy's license kept next to the game (launchers have no way to ask for it): <CONTENT_ID>.rap in the game's
# folder, beside it, or beside the .pkg. Only the file's name is looked at.
function Find-RapNear([string]$Source, [string]$ContentId) {
    if (-not $Source -or -not $ContentId) { return $null }
    $dirs = @()
    if (Test-Path -LiteralPath $Source -PathType Container) { $dirs += $Source; $dirs += (Split-Path -Parent $Source) }
    elseif (Test-Path -LiteralPath $Source -PathType Leaf) { $dirs += (Split-Path -Parent $Source) }
    foreach ($d in $dirs) {
        if (-not $d) { continue }
        $f = Join-Path $d "$ContentId.rap"
        if (Test-Path -LiteralPath $f -PathType Leaf) { return $f }
    }
    $null
}

# One row per check: Ok (true/false/$null = a note, never blocking), Name, Detail
#   -Disc: the one pick (a game folder, an .iso, a .pkg; the port's own game\disc on an update)
#   -DiscKey / -Rap: the files Setup asked for (an encrypted image's disc key, a PSN copy's license)
function Get-SetupChecks([string]$Disc, [string]$Rpcs3, [string]$DiscKey = "", [string]$Rap = "") {
    $root = Get-RealRoot
    $rows = New-Object System.Collections.ArrayList
    function Add($ok, $name, $detail) { [void]$rows.Add([pscustomobject]@{ Ok = $ok; Name = $name; Detail = $detail }) }

    # --- this PC
    Add ([Environment]::Is64BitOperatingSystem) "64-bit Windows" ([Environment]::OSVersion.VersionString)
    $avx2 = [PortSetup.Cpu]::IsProcessorFeaturePresent(40)
    Add $avx2 "CPU with AVX2" $(if ($avx2) { "yes" } else { "the port is built for AVX2 CPUs (Intel 4th gen / AMD Ryzen or newer)" })
    $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    Add $(if ($ram -ge 16) { $true } else { $null }) "Memory" "$ram GB$(if ($ram -lt 16) { ' (16 GB or more recommended: the build is large)' })"
    $drive = (Get-Item -LiteralPath $root).PSDrive
    $free = [math]::Round($drive.Free / 1GB)
    $own = Join-Path $root "game\disc"
    $ownCopy = (Test-Path -LiteralPath (Join-Path $own ".port-source.json")) -and -not ((Get-Item -LiteralPath $own -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)
    $needGb = if ($ownCopy) { 6 } else { 16 }
    Add $(if ($free -ge $needGb) { $true } else { $null }) "Free disk space on $($drive.Name):" "$free GB (about $needGb GB needed: $(if (-not $ownCopy) { 'your game''s files, about 9 GB, are copied into the port''s folder; ' })the build needs about 5 GB, and keeps about 3 GB for quick updates)"
    # (v0.6.0) the port's folder holds everything itself, no link out: any local drive works. A network drive is
    # too slow for the build (and the game reads its files from there).
    $fsProblem = Get-DriveProblem $root
    if ($fsProblem -eq "a network drive") { Add $false "Port folder's drive" "the port's folder is on a network drive. Put Setup.exe in a folder on a drive in this PC, for example C:\Games\inFAMOUS2, and run it again" }
    elseif ($fsProblem) { Add $null "Port folder's drive" "$fsProblem (it works; an internal NTFS drive builds faster)" }
    else { Add $true "Port folder's drive" "a local drive" }
    # The build tools (the compilers, cmd) can't take every folder name (GitHub #19, #34: "&", other alphabets, very
    # long paths): such a folder is built through a junction with a plain name (a note).
    if (-not $fsProblem -and -not (Test-PlainPath $root)) {
        $br = Get-BuildRoot $root
        Add $null "Port folder's name" $(if ($br) { "built through $br (a plain path for the build tools; your files stay in $root)" }
                                          else { "the build tools can't take this folder's name or length: the build goes through a link with a plain name, made when it starts" })
    }
    $vk = Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\vulkan-1.dll")
    Add $vk "Vulkan graphics driver" $(if ($vk) { "found" } else { "update your graphics driver (NVIDIA, AMD or Intel)" })

    # --- the port's own tools (v0.6.0): the prebuilt engine, clang + lld, an embeddable Python, from the release
    try {
        . (Join-Path $PSScriptRoot "slim_tools.ps1")
        $man = Get-SlimManifest
        $sp = Get-SlimPaths
        $mb = [math]::Round(((@($man.assets) | Measure-Object -Property size -Sum).Sum) / 1MB)
        Add $(if ($sp.Engine -and $sp.Llvm -and $sp.Python) { $true } else { $null }) "The port's tools" $(
            if ($sp.Engine -and $sp.Llvm -and $sp.Python) { "tools-cache\slim (the engine, clang + lld, Python)" }
            else { "downloaded during the build from the GitHub release $($man.release): the prebuilt engine, clang + lld and Python ($mb MB, nothing is installed)" })
    } catch { Add $false "The port's tools" "$($_.Exception.Message)" }

    # --- the player's game
    $src = Get-GameSource $Disc $Rpcs3
    $title = $src.Title; $sfo = $src.Sfo
    if ($src.Kind -eq "folder" -and $src.Folder) { $Disc = $src.Folder }
    $known = if ($title) { $script:KnownTitles[$title] } else { $null }
    $digital = $title -like "NP*"
    $what = @{ folder = "Game folder"; iso = "Game disc image"; pkg = "Game package (PSN)" }[$src.Kind]
    if (-not $what) { $what = "Game disc folder" }
    if ($src.Problem) { Add $false $what $src.Problem }
    elseif (-not $title) {
        if ($src.Kind -eq "iso" -and $src.Encrypted) { Add $null $what "$(Split-Path -Leaf $Disc): an encrypted disc image" }
        else { Add $false $what "Setup can't tell which game this is" }
    } else {
        $tested = $script:TestedTitles -contains $title
        $name = if ($sfo) { $sfo["TITLE"] } else { "inFAMOUS 2" }
        $ver = if ($sfo) { " $($sfo['APP_VER'])" } else { "" }
        Add $(if ($tested) { $true } elseif ($known) { $null } else { $false }) $what `
            "$name [$title]$ver $(if ($known) { $known } else { '-- not inFAMOUS 2' })$(if ($tested) { ' (tested)' } elseif ($known) { ' -- untested: please report how it goes' })"
    }
    if ($known -and $src.Kind -eq "folder" -and -not $src.Problem) {
        if ($digital -and -not (Get-DigitalGameDir $Disc $Rpcs3 $title)) {
            # a PS3_GAME folder made by hand from a PSN copy, without the EBOOT, and RPCS3 has no copy (GitHub #40)
            Add $false "Game (digital)" "this PS3_GAME folder holds a PSN copy without its USRDIR\EBOOT.BIN, and RPCS3 has no $title folder (dev_hdd0\game\$title). Choose the $title folder itself (it holds PARAM.SFO and USRDIR)"
        } elseif (-not $digital) {
            $gp = Get-GameFileProblems $Disc $title
            Add $(if ($gp) { $false } else { $true }) "Game files" $(if ($gp) { $gp } else { "the game's archives are there and readable" })
        }
    }
    if ($src.Kind -eq "iso" -and -not $src.Problem) {
        if (-not $src.Encrypted) { Add $true "Game files" "a decrypted disc image: extracted into the port's folder" }
        else {
            # can this ps3tool decrypt disc images at all? ($null: not known before the port's tools are downloaded;
            # setup_build.ps1 asks again then, before anything is copied)
            $modes = Get-Ps3ToolModes
            $near = Find-DiscKeyNear $Disc
            if ($null -ne $modes -and $modes -notcontains "iso") { Add $false "Game disc image" $script:IsoCant }
            elseif ($DiscKey -and (Test-Path -LiteralPath $DiscKey -PathType Leaf)) { Add $true "Disc key" "$(Split-Path -Leaf $DiscKey) (handed to ps3tool with the image)" }
            elseif ($near) { Add $true "Disc key" "$(Split-Path -Leaf $near), beside the image (ps3tool finds it there)" }
            else { Add $null "Disc key" "this disc image is encrypted (a raw copy of the disc): it is decrypted with its disc key. ps3tool looks in the image itself first (a 3K3Y image carries its key); if there is none, Setup asks for the .dkey or .ird file" }
        }
    }
    # --- the game's update and the PS3 firmware files. In order: the port's own copies in game\ (an earlier
    # Setup), RPCS3's when it has them (copied: nothing to download), else Sony's own servers (tools\sony_files.py:
    # the update 1.01 + 1.03 + 1.04, 236 MB, and the system software 4.93, 197 MB, unpacked with tools\ps3tool.py).
    # The digital (PSN) version is its own update; its code is unlocked with the player's license (.rap).
    $ownUpd = $false
    if ($title) {
        $u = Join-Path $root "game\dev_hdd0\game\$title"
        if (Test-Path -LiteralPath (Join-Path $u "PARAM.SFO")) {
            $v = (Read-Sfo (Join-Path $u "PARAM.SFO"))["APP_VER"]
            $ownUpd = ($v -eq "01.04" -or ($digital -and $v -eq "01.03")) -and (Test-Path -LiteralPath (Join-Path $u "USRDIR\EBOOT.BIN"))
        }
    }
    $ownFw = -not (@("liblv2", "libsysmodule", "libsre") | Where-Object { -not (Test-Path -LiteralPath (Join-Path $root "game\dev_flash\sys\external\$_.sprx")) })
    $rpOk = [bool]$Rpcs3 -and (Test-Path -LiteralPath (Join-Path $Rpcs3 "rpcs3.exe"))
    if ($Rpcs3 -and -not $rpOk) { Add $null "RPCS3" "no rpcs3.exe in ${Rpcs3}: not used" }
    elseif ($rpOk) { Add $true "RPCS3" "$Rpcs3 (optional: its update and firmware files are copied when it has them)" }
    else { Add $null "RPCS3" "not used (not needed: the game's update and the PS3 firmware files come from Sony's servers)" }
    $rpFlash = if ($rpOk) { Get-Rpcs3Dir $Rpcs3 "dev_flash" } else { $null }
    $rpFw = $rpFlash -and (Test-Path -LiteralPath (Join-Path $rpFlash "sys\external\liblv2.sprx"))
    # the port is made with firmware 4.93's libsre / liblv2 / libsysmodule (GitHub #41): RPCS3's copy is used only
    # when it is 4.93 (or does not say); another version there is left alone and 4.93 comes from Sony
    $rpFwOther = $null
    if ($rpFw) { $v = Get-FirmwareVersion $rpFlash; if ($v -and $v -ne "4.93") { $rpFwOther = $v; $rpFw = $false } }
    if ($ownFw) { Add $true "PS3 firmware files" "in game\dev_flash" }
    elseif ($rpFw) { Add $true "PS3 firmware files" "copied from RPCS3 ($rpFlash)" }
    else { Add $null "PS3 firmware files" "downloaded from Sony's servers during the build: the system software 4.93 (197 MB), of which the 3 modules the build translates are kept$(if ($rpFwOther) { " (RPCS3 has firmware ${rpFwOther}: not used, the port is made with 4.93)" })" }
    $fwv = if ($ownFw) { Get-FirmwareVersion (Join-Path $root "game\dev_flash") } else { $null }
    if ($fwv -and $fwv -ne "4.93") { Add $null "PS3 firmware version" "$fwv. The port is tested with 4.93: if the build stops at [firmware], delete the port's game\dev_flash folder and press Try again (Setup then gets 4.93 from Sony's servers)" }
    if ($title -and $known) {
        $hdd = if ($rpOk) { Get-Rpcs3Dir $Rpcs3 "dev_hdd0" } else { $null }
        $upd = if ($hdd) { Join-Path $hdd "game\$title" } else { $null }
        $ver = $null
        if ($upd -and (Test-Path -LiteralPath (Join-Path $upd "PARAM.SFO"))) { $ver = (Read-Sfo (Join-Path $upd "PARAM.SFO"))["APP_VER"] }
        $rpUpd = $ver -eq "01.04" -and (Test-Path -LiteralPath (Join-Path $upd "USRDIR\EBOOT.BIN"))
        if ($digital) {
            # its last version, 01.03, carries the same code as the discs' 1.04 update (one PPU hash)
            $dver = if ($src.Kind -eq "folder" -and $sfo) { [string]$sfo["APP_VER"] } else { "" }
            if ($ownUpd) { Add $true "Digital game" "in game\dev_hdd0\game\$title" }
            elseif ($src.Kind -eq "folder" -and $dver -and $dver -ne "01.03") { Add $false "Digital game" "this PSN copy is version $dver; the port needs 01.03 (the PSN version's last update: install it in RPCS3, File > Install Packages, and choose RPCS3's $title folder)" }
            $cid = if ($src.ContentId) { $src.ContentId } elseif ($sfo) { [string]$sfo["CONTENT_ID"] } elseif ($ownUpd) { [string](Read-Sfo (Join-Path $root "game\dev_hdd0\game\$title\PARAM.SFO"))["CONTENT_ID"] } else { "" }
            $unlocked = Test-Path -LiteralPath (Join-Path $root "eboot\EBOOT_upd104.elf")
            if (-not $unlocked) { try { . (Join-Path $PSScriptRoot "decrypt.ps1"); if (Get-DecryptorTemplate) { $unlocked = $true } } catch {} }
            $lic = if ($rpOk -and $cid) { Find-DigitalLicense $Rpcs3 $cid } else { $null }
            $ownLic = if ($cid) { Join-Path $root "game\gamedata\dev_hdd0\home\00000001\exdata\$cid.rap" } else { $null }
            if (-not $Rap) { $near = Find-RapNear $Disc $cid; if ($near) { $Rap = $near } }
            # can this ps3tool use a license file? ($null: not known before the port's tools are downloaded;
            # setup_build.ps1 asks again then, before anything is copied). When it can't, only RPCS3 holding the
            # license unlocks the game: said here, and no file is asked for.
            $modes = if ($unlocked) { $null } else { Get-Ps3ToolModes }
            $canRap = if ($null -eq $modes) { $null } else { $modes -contains "psn-rap" }
            if ($unlocked) { Add $null "License (.rap)" "not needed: the game's code is unlocked already" }
            elseif ($canRap -ne $false -and $Rap -and (Test-Path -LiteralPath $Rap -PathType Leaf)) { Add $true "License (.rap)" "$(Split-Path -Leaf $Rap) (handed to ps3tool with the game's code)" }
            elseif ($canRap -ne $false -and $ownLic -and (Test-Path -LiteralPath $ownLic)) { Add $true "License (.rap)" "in game\gamedata\...\exdata ($cid.rap)" }
            elseif ($lic) { Add $true "License (.rap)" "in RPCS3 ($(Split-Path -Leaf $lic)): RPCS3 unlocks the game's code with it" }
            elseif ($canRap -eq $false) { Add $false "Digital game" (Get-PsnCant $title) }
            elseif ($canRap) { Add $false "License (.rap)" "the PSN version's code is unlocked with your license for it: choose your $(if ($cid) { "$cid.rap" } else { '.rap' }) file (the one RPCS3 or your PS3 uses for this game)" }
            else { Add $null "License (.rap)" "the PSN version's code is unlocked with your license for it. Whether this version can use a .rap file is checked when the port's tools are downloaded, before anything is copied; the file is asked for only if it can" }
        } elseif ($ownUpd) { Add $true "Game update 1.04" "in game\dev_hdd0\game\$title" }
        elseif ($rpUpd) { Add $true "Game update 1.04" "copied from RPCS3 ($upd)" }
        elseif ($script:SonyUpdates -contains $title) {
            Add $null "Game update 1.04" "downloaded from Sony's servers during the build: the updates 1.01, 1.03 and 1.04 (236 MB)$(if ($ver) { " (RPCS3 has version $ver)" })"
        } else { Add $false "Game update 1.04" "Setup knows no update packages for $title" }
        # DLC (GitHub #48, #56; optional, never blocking): the game's unlock files (tools\port_paths.ps1). Setup
        # copies RPCS3's into the port's folder during the build; the dlc folder's are copied then and at each start.
        try {
            $dOwn = @(Get-DlcFiles (Join-Path $root "game\dev_hdd0\game\$title\USRDIR\dlc"))
            $dRpHdd = if ($rpOk) { Get-Rpcs3Dir $Rpcs3 "dev_hdd0" "game\$title\USRDIR\dlc" } else { $null }
            $dRp = if ($dRpHdd) { @(Get-DlcFiles (Join-Path $dRpHdd "game\$title\USRDIR\dlc")) } else { @() }
            $dDrop = @(Get-DlcFiles (Join-Path $root "dlc"))
            $dSrc = if ($digital -and $src.Kind -eq "folder" -and $Disc) { @(Get-DlcFiles (Join-Path (Get-GameRoot $Disc) "USRDIR\dlc")) } else { @() }
            $nf = { param($n) "$n file$(if ($n -ne 1) { 's' })" }
            $parts = @($(if ($dRp.Count) { "$(& $nf $dRp.Count) found in RPCS3 (copied into the port's folder during the build)" }),
                       $(if ($dSrc.Count) { "$(& $nf $dSrc.Count) in your game copy" }),
                       $(if ($dDrop.Count) { "$(& $nf $dDrop.Count) in the dlc folder" }),
                       $(if ($dOwn.Count) { "$(& $nf $dOwn.Count) in the port's folder already" })) | Where-Object { $_ }
            $lic = Get-DlcNeedLicense (@($dRp) + @($dSrc) + @($dDrop) + @($dOwn))
            if ($parts) { Add $true "DLC" "$($parts -join '; ')$(if ($lic) { '. Some of this DLC needs its license (.rap): not supported yet' })" }
            else { Add $null "DLC" "none (optional: install your DLC in RPCS3 and run Setup again, or put its unlock files, *.edat, in the port's dlc folder)" }
        } catch {}
    }
    $ps3tool = Join-Path $root "tools\ps3tool.py"
    Add $null "ps3tool" $(if (Test-Path -LiteralPath $ps3tool) { "tools\ps3tool.py (unpacks and decrypts your game's files)" } else { "downloaded from GitHub during the build (tools\ps3tool.py: unpacks and decrypts your game's files)" })
    , $rows
}
