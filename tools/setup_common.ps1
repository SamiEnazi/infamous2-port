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
    "BCES01143" = "Europe disc (the tested one)"; "BCES01144" = "Europe disc"; "BCES01229" = "disc";
    "BCUS98125" = "USA disc"; "NPUA80638" = "USA PSN"
}

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
    Add $(if ($free -ge 30) { $true } else { $null }) "Free disk space on $($drive.Name):" "$free GB (the build needs about 15 GB while it runs, 4 GB after)"
    $vk = Test-Path (Join-Path $env:WINDIR "System32\vulkan-1.dll")
    Add $vk "Vulkan graphics driver" $(if ($vk) { "found" } else { "update your graphics driver (NVIDIA, AMD or Intel)" })

    # --- the build tools: downloaded into tools-cache\ during the build (tools\fetch_tools.ps1) and
    # deleted after it, so a missing one is a note, never a blocker. Nothing is installed.
    $later = "downloaded during the build, deleted after it"
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
    if (-not $Disc) { Add $false "Game disc folder" "choose the folder that holds PS3_GAME" }
    elseif (-not (Test-Path (Join-Path $Disc "PS3_GAME\PARAM.SFO"))) { Add $false "Game disc folder" "no PS3_GAME\PARAM.SFO in $Disc" }
    else {
        $sfo = Read-Sfo (Join-Path $Disc "PS3_GAME\PARAM.SFO"); $title = $sfo["TITLE_ID"]
        $known = $script:KnownTitles[$title]
        Add $(if ($title -eq "BCES01143") { $true } elseif ($known) { $null } else { $false }) "Game disc" `
            "$($sfo['TITLE']) [$title] $(if ($known) { $known } else { 'not inFAMOUS 2' })$(if ($known -and $title -ne 'BCES01143') { ' -- untested: please report how it goes' })"
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
            $ok = ($ver -eq "01.04") -and (Test-Path (Join-Path $upd "USRDIR\EBOOT.BIN"))
            Add $ok "Game update 1.04 in RPCS3" $(if ($ok) { $upd } elseif ($ver) { "found version $ver; install the 1.04 update in RPCS3 (File > Install Packages)" } else { "not installed: install the 1.04 update in RPCS3 (File > Install Packages)" })
        }
    }
    , $rows
}
