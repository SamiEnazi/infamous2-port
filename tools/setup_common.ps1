# Shared by tools\setup.ps1 (the window) and tools\setup_build.ps1 (the work): reading a
# PARAM.SFO, and the list of checks (this PC, the tools, the player's game and RPCS3).

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

# One row per check: Ok (true/false/$null = warning), Name, Detail
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
    Add $(if ($free -ge 30) { $true } else { $null }) "Free disk space on $($drive.Name):" "$free GB (the build needs about 15-25 GB)"
    $vk = Test-Path (Join-Path $env:WINDIR "System32\vulkan-1.dll")
    Add $vk "Vulkan graphics driver" $(if ($vk) { "found" } else { "update your graphics driver (NVIDIA, AMD or Intel)" })

    # --- tools
    $git = Get-Command git -ErrorAction SilentlyContinue
    Add ([bool]$git) "Git" $(if ($git) { $git.Source } else { "missing (winget install Git.Git)" })
    $py = Get-Command python -ErrorAction SilentlyContinue
    $pyv = $null; if ($py) { $pyv = (& $py.Source -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null) }
    $pyok = $pyv -and ([version]$pyv -ge [version]"3.10")
    Add $pyok "Python 3.10+" $(if ($pyok) { "$pyv ($($py.Source))" } else { "missing (winget install Python.Python.3.12)" })
    foreach ($t in @(@("vcvars", "Visual Studio 2022 C++ build tools", "winget install Microsoft.VisualStudio.2022.BuildTools"),
                     @("llvm", "LLVM (clang-cl)", "winget install LLVM.LLVM"),
                     @("ninja", "Ninja", "winget install Ninja-build.Ninja"))) {
        $v = $null
        if ($pyok) { $v = (& $py.Source (Join-Path $PSScriptRoot "port_paths.py") $t[0] 2>$null) }
        Add ([bool]$v) $t[1] $(if ($v) { $v } else { "missing ($($t[2]))" })
    }
    $cm = Get-Command cmake -ErrorAction SilentlyContinue
    $vsCmake = $false
    if (-not $cm -and $pyok) {
        $vc = (& $py.Source (Join-Path $PSScriptRoot "port_paths.py") vcvars 2>$null)
        if ($vc) { $vsCmake = Test-Path (Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $vc)))) "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe") }
    }
    Add ($cm -or $vsCmake) "CMake" $(if ($cm) { $cm.Source } elseif ($vsCmake) { "Visual Studio's" } else { "missing (winget install Kitware.CMake)" })

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
