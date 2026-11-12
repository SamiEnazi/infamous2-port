# Shared by Setup (tools\setup.ps1, Setup.exe) and the scripts it runs: reading a PARAM.SFO, the
# list of checks (this PC, the build tools, the player's game and RPCS3), downloads, tool paths.

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
    "BCES01143" = "Europe disc"; "BCES01144" = "Europe disc"; "BCES01229" = "disc";
    "BCUS98125" = "USA disc"; "NPUA80638" = "USA PSN (digital)"; "NPEA00318" = "Europe PSN (digital)"
}
# Players have built and played these (README: EU disc, US disc, the PSN version 01.03); the others
# have the same code but nobody has reported on them yet.
$script:TestedTitles = @("BCES01143", "BCUS98125", "NPUA80638")

# A build tool's path, the way tools\port_paths.py finds it for Setup: environment variable
# (PORT_PYTHON ...), then config.local.json, then tools-cache\ (fetch_tools.ps1). PORT_ONLY_CACHE=1:
# only the environment variable and tools-cache. Setup never uses tools found on PATH: it downloads
# its own, so every player builds with the same versions. $null when not there (yet).
function Get-PortTool([string]$Key) {
    $root = Split-Path -Parent $PSScriptRoot
    $v = [Environment]::GetEnvironmentVariable("PORT_" + $Key.ToUpper())
    if (-not $v -and $env:PORT_ONLY_CACHE -ne "1") {
        $cfg = Join-Path $root "config.local.json"
        if (Test-Path $cfg) { try { $v = (Get-Content $cfg -Raw | ConvertFrom-Json).$Key } catch {} }
    }
    if (-not $v) {
        $cache = @{ python = "python\python.exe"; cmake = "cmake\bin\cmake.exe"; ninja = "ninja\ninja.exe"; llvm = "llvm"; msvc = "msvc"; sdk = "sdk" }[$Key]
        if ($cache) {
            $dir = Join-Path (Join-Path $root "tools-cache") ($cache -split '\\')[0]
            if (Test-Path (Join-Path $dir ".ready")) { $v = Join-Path (Join-Path $root "tools-cache") $cache }
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
                $h = (Get-FileHash "$Out.part" -Algorithm SHA256).Hash
                if ($h -ne $Sha256) { Remove-Item "$Out.part"; throw "$What has the wrong SHA-256 ($h)" }
            }
            Move-Item -Force "$Out.part" $Out
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

# A digital (PSN) copy runs from a PS3_GAME folder the player makes by hand (README): PARAM.SFO,
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

# One row per check: Ok (true/false/$null = a note, never blocking), Name, Detail
function Get-SetupChecks([string]$Disc, [string]$Rpcs3) {
    $root = Split-Path -Parent $PSScriptRoot
    $rows = New-Object System.Collections.ArrayList
    function Add($ok, $name, $detail) { [void]$rows.Add([pscustomobject]@{ Ok = $ok; Name = $name; Detail = $detail }) }

    # --- this PC
    Add ([Environment]::Is64BitOperatingSystem) "64-bit Windows" ([Environment]::OSVersion.VersionString)
    $avx2 = [PortSetup.Cpu]::IsProcessorFeaturePresent(40)
    Add $avx2 "CPU with AVX2" $(if ($avx2) { "yes" } else { "the port is built for AVX2 CPUs (Intel 4th gen / AMD Ryzen or newer)" })
    $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    Add $(if ($ram -ge 16) { $true } else { $null }) "Memory" "$ram GB$(if ($ram -lt 16) { ' (16 GB or more recommended: the build is large)' })"
    $drive = (Get-Item $root).PSDrive
    $free = [math]::Round($drive.Free / 1GB)
    Add $(if ($free -ge 30) { $true } else { $null }) "Free disk space on $($drive.Name):" "$free GB (the build needs about 15 GB while it runs, about 7 GB after: 4 GB of game files, 3 GB of build files kept for quick updates)"
    # The port's folder must hold a junction (game\disc -> your game folder): a local NTFS or ReFS
    # drive. FAT32/exFAT (USB sticks, many external disks) and network drives fail with "Incorrect
    # function" when step 3 makes the link (GitHub issue #2) -- say so before 1.3 GB of downloads.
    $fsProblem = Get-DriveProblem $root
    Add $(if (-not $fsProblem) { $true } else { $false }) "Port folder's drive" $(if (-not $fsProblem) { "can hold the link to your game folder" } else { "the port's folder is on $fsProblem, which cannot hold the link to your game folder. Put Setup.exe in a folder on your PC's internal drive (NTFS), for example C:\Games\inFAMOUS2, and run it again" })
    $vk = Test-Path (Join-Path $env:WINDIR "System32\vulkan-1.dll")
    Add $vk "Vulkan graphics driver" $(if ($vk) { "found" } else { "update your graphics driver (NVIDIA, AMD or Intel)" })

    # --- the build tools: downloaded into tools-cache\ during the build (tools\fetch_tools.ps1) and
    # kept for quick updates (tools\setup_cleanup.ps1 -KeepBuildCache), so a missing one is a note, never a
    # blocker. Nothing is installed.
    $later = "downloaded during the build, kept for quick updates"
    foreach ($t in @(@("python", "Python", "CPython 3.12, ~47 MB"), @("cmake", "CMake", "CMake 3.31, ~47 MB"),
                     @("ninja", "Ninja", "Ninja 1.11, under 1 MB"), @("llvm", "LLVM (clang-cl)", "LLVM 22.1.8, ~862 MB"))) {
        $p = Get-PortTool $t[0]
        Add $(if ($p) { $true } else { $null }) $t[1] $(if ($p) { $p } else { "$later ($($t[2]))" })
    }
    $msvc = (Get-PortTool msvc) -and (Get-PortTool sdk)
    $vcvars = if ($env:PORT_ONLY_CACHE -ne "1") { Get-PortTool vcvars } else { $null }
    Add $(if ($msvc -or $vcvars) { $true } else { $null }) "MSVC + Windows SDK" $(if ($msvc) { "tools-cache\msvc, tools-cache\sdk" } elseif ($vcvars) { $vcvars } else { "$later (Microsoft's C++ headers and libraries, ~400 MB)" })

    # --- the player's game
    $title = $null
    # a game kept as an .iso (players' comments 2026-11-13): the port needs the files, not the image
    $iso = $null
    if ($Disc -and (Test-Path -LiteralPath $Disc -PathType Leaf) -and $Disc -match '\.iso$') { $iso = $Disc }
    elseif ($Disc -and (Test-Path -LiteralPath $Disc -PathType Container) -and -not (Test-Path (Join-Path $Disc "PS3_GAME\PARAM.SFO"))) {
        $iso = Get-ChildItem -LiteralPath $Disc -Filter *.iso -File -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $Disc) { Add $false "Game disc folder" "choose the folder that holds PS3_GAME" }
    elseif ($iso) { Add $false "Game disc folder" "your game is an .iso file ($iso). Extract it into a folder first (7-Zip can open it: right-click > 7-Zip > Extract to), then choose that folder: the one with PS3_GAME in it" }
    elseif (-not (Test-Path (Join-Path $Disc "PS3_GAME\PARAM.SFO"))) {
        $deeper = Get-ChildItem -LiteralPath $Disc -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName "PS3_GAME\PARAM.SFO") } | Select-Object -First 1 -ExpandProperty FullName
        Add $false "Game disc folder" $(if ($deeper) { "PS3_GAME is one folder deeper: choose $deeper" } else { "no PS3_GAME folder in ${Disc}: choose the folder that has PS3_GAME in it (with PARAM.SFO inside PS3_GAME)" })
    }
    elseif ((Get-DriveProblem $Disc) -eq "a network drive") {
        # a junction cannot point at a network share
        Add $false "Game folder's drive" "your game folder ($Disc) is on a network drive, which the port's link cannot point to. Copy the game folder (the one with PS3_GAME) to a drive in this PC and choose it there"
    }
    else {
        $sfo = Read-Sfo (Join-Path $Disc "PS3_GAME\PARAM.SFO"); $title = $sfo["TITLE_ID"]
        $known = $script:KnownTitles[$title]
        $tested = $script:TestedTitles -contains $title
        Add $(if ($tested) { $true } elseif ($known) { $null } else { $false }) "Game disc" `
            "$($sfo['TITLE']) [$title] $(if ($known) { $known } else { 'not inFAMOUS 2' })$(if ($tested) { ' (tested)' } elseif ($known) { ' -- untested: please report how it goes' })"
    }
    if (-not $Rpcs3) { Add $false "RPCS3 folder" "choose the folder that holds rpcs3.exe" }
    elseif (-not (Test-Path (Join-Path $Rpcs3 "rpcs3.exe"))) { Add $false "RPCS3 folder" "no rpcs3.exe in $Rpcs3" }
    else {
        Add $true "RPCS3" $Rpcs3
        $fw = Test-Path (Join-Path $Rpcs3 "dev_flash\sys\external\liblv2.sprx")
        Add $fw "PS3 firmware in RPCS3" $(if ($fw) { "installed" } else { "install it in RPCS3: File > Install Firmware (PS3UPDAT.PUP from playstation.com)" })
        if ($title) {
            $upd = Join-Path $Rpcs3 "dev_hdd0\game\$title"
            $ver = $null
            if (Test-Path (Join-Path $upd "PARAM.SFO")) { $ver = (Read-Sfo (Join-Path $upd "PARAM.SFO"))["APP_VER"] }
            # The digital (PSN) game is its own folder in RPCS3, and its last version, 01.03, carries the
            # same code as the discs' 1.04 update (RPCS3 lists them under one PPU hash): no update to install.
            $digital = $title -like "NP*"
            $ok = ($ver -eq "01.04" -or ($digital -and $ver -eq "01.03")) -and (Test-Path (Join-Path $upd "USRDIR\EBOOT.BIN"))
            if ($digital) {
                Add $ok "Digital game in RPCS3" $(if ($ok) { $upd } elseif ($ver) { "found version $ver in $upd; this port needs 01.03" } else { "not installed: install the game's package and its license in RPCS3 (File > Install Packages/Raps/Edats)" })
                if ($ok -and $Disc) {
                    $gaps = @(Get-DigitalCopyGaps -Disc $Disc -GameDir $upd)
                    if ($gaps.Count -eq 0) { Add $true "Digital copy's PS3_GAME folder" "has all of the game's files from RPCS3's copy" }
                    else {
                        # A note, never a blocker: the port reads the game from this hand-made folder.
                        # The save icon is the one that hurt: without it every save failed in builds
                        # before 2026-11-12 ("There was an error saving." again and again).
                        $icon = if ($gaps -contains "ui\xmb\savedata\icon.png") { " -- among them the save icon (ui\xmb\savedata\icon.png)" } else { "" }
                        Add $null "Digital copy's PS3_GAME folder" ("$($gaps.Count) of the game's files in $upd\USRDIR are missing from $Disc\PS3_GAME\USRDIR$icon (e.g. " +
                            (($gaps | Select-Object -First 3) -join ", ") + "): copy the whole USRDIR folder from RPCS3 into $Disc\PS3_GAME again")
                    }
                }
            } else {
                Add $ok "Game update 1.04 in RPCS3" $(if ($ok) { $upd } elseif ($ver) { "found version $ver; install the 1.04 update in RPCS3 (File > Install Packages)" } else { "not installed: install the 1.04 update in RPCS3 (File > Install Packages)" })
            }
        }
    }
    , $rows
}
