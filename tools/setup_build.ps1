# The work behind Setup.exe and Setup.cmd (also usable on its own):
#   1. check this PC, the game and RPCS3                             (tools\setup_common.ps1)
#   2. the build tools, downloaded into tools-cache\ (no installs)    (tools\fetch_tools.ps1)
#   3. your game: the disc folder linked into game\disc; the 1.04 update, the firmware files and
#      the game's HDD install copied into game\ (so RPCS3 is not needed after Setup)
#   4. decrypt the 1.04 EBOOT with RPCS3                             -> eboot\EBOOT_upd104.elf
#   5. Python tools                                                  -> work\venv
#   6. SDL2, Vulkan headers, glslang                                 -> deps\
#   7. the exe's icon from the player's own ICON0.PNG                -> work\icon\app.ico
#   8. translate and compile the game                                -> work\port\build\Infamous2Recomp.exe
#   9. a desktop shortcut
#  10. with -Cleanup: delete what only this build needed (tools\setup_cleanup.ps1). The build tools and the
#      build files stay for the next update (about 2.6 GB): an update that changes a few files then downloads
#      no tools and rebuilds only what changed, in minutes. -NoBuildCache (or PORT_BUILD_CACHE=0: Setup.exe
#      --prepare, for RecompLauncher) deletes them too, as before.
#   tools\setup_build.ps1 -Disc <folder holding PS3_GAME> -Rpcs3 <RPCS3 folder> [-NoShortcut]
#                         [-AcceptMsvcLicense] [-Cleanup] [-NoBuildCache] [-Pgo]
# -AcceptMsvcLicense: Microsoft's MSVC + Windows SDK files are under the Visual Studio Build Tools
# license (https://go.microsoft.com/fwlink/?LinkId=2179911); Setup asks first.
# -Pgo ("Optimise for this PC", Setup.exe's checkbox): profile-guided optimisation trained on this PC,
# 12 steps instead of 10 (steps 1-7 as above):
#   8. translate the game, then compile a training build (instrumented) -> work\port\build-train
#   9. train it: the game plays by itself in a window for a few minutes, sound off (tools\pgo_train.ps1)
#  10. compile the game with what it learned -> work\port\build\Infamous2Recomp.exe
#  11. a desktop shortcut   12. cleaning up
# PGO never makes Setup fail: when the training build, the training or the optimised compile fails, the
# game is compiled the normal way and the line "  pgo: not used: <why>" says why (Setup.exe shows it).
# Updates: the profile is kept (work\pgo-setup\setup-<time>.profdata, ~30 MB, never published) and the new
# version is compiled with it in step 8: clang uses it for every function whose code did not change and
# compiles a changed one without it (it never misapplies counts). When rebuild_win.py then reports that hot
# code changed since the training (its "retrain" hint), or the profile cannot be read, Setup trains again
# (steps 8-10, the build made with the old profile stays as the fallback); else the update stays quick.
param(
    [Parameter(Mandatory = $true)][string]$Disc,
    [Parameter(Mandatory = $true)][string]$Rpcs3,
    [switch]$NoShortcut,
    [switch]$AcceptMsvcLicense,
    [switch]$Cleanup,
    [switch]$NoBuildCache,
    [switch]$Pgo
)
$ErrorActionPreference = "Stop"
# Setup.exe reads these lines through a pipe, in UTF-8 (a console keeps its own code page)
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.Encoding]::UTF8 }
trap { "== FAILED: $_"; exit 1 }   # the reason lands in the log Setup shows
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "setup_common.ps1")
$sw = [Diagnostics.Stopwatch]::StartNew()
$nSteps = if ($Pgo) { 12 } else { 10 }
function Step([int]$n, [string]$text) { "== [$n/$nSteps] $text   ($([int]$sw.Elapsed.TotalMinutes) min so far)" }
# Native tools (git, pip, cmd, python, robocopy) write progress and warnings to stderr; under
# "Stop" with the output redirected to Setup's log, Windows PowerShell would turn the first such
# line into a failure. Run them under "Continue", keep their lines, judge them by exit code.
function Invoke-Native([scriptblock]$Cmd) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $Cmd 2>&1 | ForEach-Object { "    $_" } } finally { $ErrorActionPreference = $eap }
}
function Copy-Tree([string]$Src, [string]$Dst, [string]$What) {
    if (Test-Path -LiteralPath $Dst) {
        $i = Get-Item -LiteralPath $Dst -Force
        if ($i.LinkType -eq "Junction") { cmd /c rmdir "$Dst" | Out-Null }   # an older Setup linked it
    }
    "  copying $What"
    Invoke-Native { robocopy $Src $Dst /E /NFL /NDL /NJH /NJS /NP } | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copying $Src failed (robocopy $LASTEXITCODE)" }
}
$Disc = (Resolve-Path -LiteralPath $Disc).Path.TrimEnd('\'); $Rpcs3 = (Resolve-Path -LiteralPath $Rpcs3).Path.TrimEnd('\')
# The disc folder given as a junction (e.g. this port's own game\disc from an earlier Setup): use
# the folder it points to, or step 3 would replace the link with a link to itself.
$di = Get-Item -LiteralPath $Disc -Force
if ($di.LinkType -eq "Junction" -and @($di.Target)[0]) { $Disc = ([string]@($di.Target)[0]).TrimEnd('\') }

Step 1 "checking"
# The engine (engine\) and the port base (base\) are folders of this repository since v0.5.0 (no
# submodules any more). A git clone has them with LF line ends too (.gitattributes: the patches in
# patches\port are LF, as base\'s files are).
foreach ($sub in @(@("engine", "CMakeLists.txt"), @("base", "infamous2-lle\CMakeLists.txt"))) {
    if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $root $sub[0]) $sub[1]))) {
        throw "$($sub[0])\$($sub[1]) is missing: download the port again with Setup.exe (or 'git clone')"
    }
    "  $($sub[0]): ok"
}
# A Setup.exe of v0.4.2 or older updates by copying the new version over the old one, never deleting, and
# it still downloads toolkit\ and community\ (the engine and the port base before v0.5.0: nothing builds them
# now). A file the new engine\ or base\ no longer has would still be built (the engine's CMake builds every
# file it finds), so what they hold beyond the version's own list (.port-files, from the public repository)
# is deleted here. Not in a git clone (git keeps them), never through a link.
$portFiles = Join-Path $root ".port-files"
if ((Test-Path -LiteralPath $portFiles) -and -not (Test-Path -LiteralPath (Join-Path $root ".git"))) {
    $want = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($l in [IO.File]::ReadAllLines($portFiles)) { if ($l.Trim()) { [void]$want.Add($l.Trim().Replace('/', '\')) } }
    $stale = New-Object System.Collections.Generic.List[string]
    foreach ($top in "engine", "base") {
        $stack = New-Object System.Collections.Stack
        $stack.Push((Get-Item -LiteralPath (Join-Path $root $top) -Force))
        while ($stack.Count) {
            $d = $stack.Pop()
            foreach ($e in $d.GetFileSystemInfos()) {
                if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
                if ($e -is [IO.DirectoryInfo]) { if ($e.Name -ne "__pycache__") { $stack.Push($e) }; continue }   # (Python's own cache)
                $rel = $e.FullName.Substring($root.Length + 1)
                if (-not $want.Contains($rel)) { $stale.Add($e.FullName) }
            }
        }
    }
    if ($want.Count -lt 100) { "  note: .port-files lists only $($want.Count) files: nothing deleted" }
    elseif ($stale.Count) {
        foreach ($f in $stale) { Remove-Item -LiteralPath $f -Force }
        "  engine\ and base\: $($stale.Count) file(s) from an earlier version deleted (this version has no such files)"
    }
}
$rows = Get-SetupChecks -Disc $Disc -Rpcs3 $Rpcs3
foreach ($r in $rows) { "  {0,-5} {1}: {2}" -f $(if ($r.Ok -eq $true) { "ok" } elseif ($r.Ok -eq $false) { "MISS" } else { "note" }), $r.Name, $r.Detail }
if ($rows | Where-Object { $_.Ok -eq $false }) { throw "fix the MISS items first" }
$title = (Read-Sfo (Join-Path $Disc "PS3_GAME\PARAM.SFO"))["TITLE_ID"]
$cfgPath = Join-Path $root "config.local.json"
$cfg = [ordered]@{}
if (Test-Path -LiteralPath $cfgPath) { (Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json).psobject.Properties | ForEach-Object { $cfg[$_.Name] = $_.Value } }
$cfg["rpcs3"] = $Rpcs3
[System.IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json))

Step 2 "the build tools (a temporary download into tools-cache\, nothing is installed)"
& (Join-Path $PSScriptRoot "fetch_tools.ps1") -AcceptMsvcLicense:$AcceptMsvcLicense -Pgo:$Pgo
$py = Get-PortTool python
if (-not $py) { throw "no Python after getting the build tools" }

Step 3 "your game: the disc linked, the update, firmware files and HDD install copied into game\"
$game = Join-Path $root "game"
New-Item -ItemType Directory -Force (Join-Path $game "dev_hdd0\game"), (Join-Path $game "gamedata") | Out-Null
$discLink = Join-Path $game "disc"
if (Test-Path -LiteralPath $discLink) {
    $i = Get-Item -LiteralPath $discLink -Force
    if ($i.LinkType -eq "Junction" -and -not (@($i.Target) -contains $Disc)) { cmd /c rmdir "$discLink" | Out-Null }
}
if (-not (Test-Path -LiteralPath $discLink)) {
    # GitHub issue #2: on FAT32/exFAT or a network drive Windows says only "Incorrect function"
    # mklink, not New-Item: Windows PowerShell 5.1 reads [ ] in -Target as a wildcard ("inFamous 2 [BCES01229]", GitHub #19)
    try { $mk = cmd /c mklink /J "$discLink" "$Disc" 2>&1; if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $discLink)) { throw ($mk -join " ") } }
    catch {
        throw ("could not link your game folder into the port ($($_.Exception.Message)). The port's folder must be on a " +
               "local NTFS drive and your game folder on a drive in this PC: put Setup.exe in a folder on your internal drive, " +
               "for example C:\Games\inFAMOUS2, and run it again")
    }
}
"  $discLink -> $Disc (keep your disc folder where it is)"
Copy-Tree (Join-Path $Rpcs3 "dev_hdd0\game\$title") (Join-Path $game "dev_hdd0\game\$title") "the 1.04 update (0.2 GB)"
Copy-Tree (Join-Path $Rpcs3 "dev_flash") (Join-Path $game "dev_flash") "the PS3 firmware files the game reads (0.2 GB)"
# The game copies 2.4 GB to the HDD on its first boot (<title>_INSTALL): RPCS3's copy if there is
# one, else the port's first boot does the install itself.
$inst = Join-Path $Rpcs3 "dev_hdd0\game\${title}_INSTALL"
$instDst = Join-Path $game "dev_hdd0\game\${title}_INSTALL"
if ((Test-Path -LiteralPath $inst) -and -not (Test-Path -LiteralPath $instDst)) { Copy-Tree $inst $instDst "the game's HDD install (2.4 GB)" }
elseif (-not (Test-Path -LiteralPath $instDst)) { "  note: no ${title}_INSTALL in RPCS3 -- the port's first boot installs it (if that fails: start the game once in RPCS3)" }

Step 4 "decrypting the game's 1.04 EBOOT (RPCS3, or your own decryptor: tools\decrypt.ps1)"
$eb = Join-Path $root "eboot"
New-Item -ItemType Directory -Force $eb | Out-Null
$elf = Join-Path $eb "EBOOT_upd104.elf"
if (-not (Test-Path -LiteralPath $elf)) {
    # The pluggable step (tools\decrypt.ps1): config.local.json's "decryptor", else RPCS3 run hidden
    # (tools\rpcs3-adapter.ps1). The port never decrypts anything itself.
    . (Join-Path $PSScriptRoot "decrypt.ps1")
    $script:DecryptorTemplate = Get-DecryptorTemplate
    if ((-not $script:DecryptorTemplate -or $script:DecryptorTemplate -match "rpcs3") -and (Get-Process rpcs3 -ErrorAction SilentlyContinue)) {
        throw "close RPCS3 first (it decrypts one file per start)"
    }
    $bin = Join-Path $eb "EBOOT_upd104.BIN"
    Copy-Item -LiteralPath (Join-Path $Rpcs3 "dev_hdd0\game\$title\USRDIR\EBOOT.BIN") -Destination $bin -Force
    try { Invoke-Decryptor -InPath $bin -OutPath $elf }
    catch { throw "decrypting the game's EBOOT failed: $($_.Exception.Message) (is the firmware installed in RPCS3?)" }
}
"  $elf"

Step 5 "Python tools (work\venv)"
$venvPy = Join-Path $root "work\venv\Scripts\python.exe"
if (Test-Path -LiteralPath $venvPy) {
    # a venv whose base Python was deleted (an earlier cleanup) does not start, and one a kept build made with
    # another Python version has its packages in the wrong place: make it again
    $pyMm = "$(Invoke-Native { & $py -c "import sys; print('%d.%d' % sys.version_info[:2])" } | Select-Object -Last 1)".Trim()
    $cfgLine = Select-String -LiteralPath (Join-Path $root "work\venv\pyvenv.cfg") -Pattern '^\s*version\s*=\s*(\d+\.\d+)' -ErrorAction SilentlyContinue |
               Select-Object -First 1
    $venvMm = if ($cfgLine) { $cfgLine.Matches[0].Groups[1].Value } else { "" }
    Invoke-Native { & $venvPy -c "import sys" } | Out-Null
    if ($LASTEXITCODE -or ($pyMm -match '^\d+\.\d+$' -and $venvMm -and $venvMm -ne $pyMm)) { cmd /c rd /s /q (Join-Path $root "work\venv") | Out-Null }
}
if (-not (Test-Path -LiteralPath $venvPy)) {
    Invoke-Native { & $py -m venv (Join-Path $root "work\venv") }
    if ($LASTEXITCODE) { throw "python -m venv failed" }
}
Invoke-Native { & $venvPy -m pip install --disable-pip-version-check -q -r (Join-Path $root "engine\requirements.txt") }
if ($LASTEXITCODE) { throw "pip install failed" }

Step 6 "SDL2, Vulkan headers and glslang (download, then build glslang: a few minutes)"
& (Join-Path $PSScriptRoot "fetch_deps.ps1")
if (-not (Test-Path -LiteralPath (Join-Path $root "deps\SDL2-2.30.10\cmake"))) { throw "fetching the libraries failed (see above)" }
# built again when its sources are another version than the kept build (deps\glslang-install\.source)
$gSrc = Join-Path $root "deps\glslang-src\.source"
$gMark = Join-Path $root "deps\glslang-install\.source"
$gWant = if (Test-Path -LiteralPath $gSrc) { (Get-Content -LiteralPath $gSrc -Raw).Trim() } else { "" }
$gHave = if (Test-Path -LiteralPath $gMark) { (Get-Content -LiteralPath $gMark -Raw).Trim() } else { "" }
if (-not (Test-Path -LiteralPath (Join-Path $root "deps\glslang-install\lib\cmake\glslang")) -or ($gWant -and $gHave -and $gWant -ne $gHave)) {
    Invoke-Native { cmd /c "`"$(Join-Path $PSScriptRoot 'build_deps.cmd')`"" }
    if ($LASTEXITCODE) { throw "building glslang failed" }
    if ($gWant) { Set-Content -LiteralPath $gMark -Value $gWant }
}

Step 7 "the exe's icon (from your copy's ICON0.PNG)"
try { & (Join-Path $PSScriptRoot "make_icon.ps1") -Icon0 (Join-Path $Disc "PS3_GAME\ICON0.PNG") } catch { "  no icon: $_ (the game still builds)" }

# With the build files of an earlier build (an update): a translation step whose inputs did not change is
# skipped, and only what changed is compiled again (tools\rebuild_win.py, PORT_LIFT_REUSE)
$env:PORT_LIFT_REUSE = "1"
$exe = Join-Path $root "work\port\build\Infamous2Recomp.exe"
$buildFailed = "the build failed: see work\logs\ (python -I tools\build_errors.py work\logs\compile.log)"
if (-not $Pgo) {
    Step 8 "translating and compiling the game (the long part: 15-40 minutes)"
    Invoke-Native { & $venvPy -I (Join-Path $PSScriptRoot "rebuild_win.py") }
    if ($LASTEXITCODE) { throw $buildFailed }
} else {
    $rebuild = Join-Path $PSScriptRoot "rebuild_win.py"
    # rebuild_win.py with PORT_PGO / PORT_BUILD set for this one call; its lines go on to Setup and into $script:rbOut
    function Invoke-Rebuild([string[]]$RbArgs, [string]$PgoMode, [string]$BuildDir) {
        $keep = @{ PORT_PGO = $env:PORT_PGO; PORT_BUILD = $env:PORT_BUILD }
        $script:rbOut = @(); $script:rbRc = 1
        try {
            if ($PgoMode) { $env:PORT_PGO = $PgoMode } else { Remove-Item env:PORT_PGO -ErrorAction SilentlyContinue }
            if ($BuildDir) { $env:PORT_BUILD = $BuildDir } else { Remove-Item env:PORT_BUILD -ErrorAction SilentlyContinue }
            Invoke-Native { & $venvPy -I $rebuild @RbArgs } | ForEach-Object { $script:rbOut += $_; $_ }
            $script:rbRc = $LASTEXITCODE
        } finally {
            foreach ($k in @($keep.Keys)) {
                if ($null -ne $keep[$k]) { Set-Item "env:$k" $keep[$k] } else { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
            }
        }
    }
    # llvm-profdata.exe beside the clang-cl that compiles (port_paths.py: tools-cache, config.local.json or this PC's)
    function Find-LlvmProfdata {
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        try { $d = & $venvPy -I (Join-Path $PSScriptRoot "port_paths.py") llvm 2>$null | Select-Object -Last 1 } catch { $d = $null }
        finally { $ErrorActionPreference = $eap }
        if (-not $d) { return $null }
        $p = Join-Path ([string]$d).Trim() "bin\llvm-profdata.exe"
        if (Test-Path -LiteralPath $p) { $p } else { $null }
    }
    function Test-Profile([string]$Path) {   # this LLVM reads it
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        try { & $llvmProf show $Path 2>&1 | Out-Null; $ok = ($LASTEXITCODE -eq 0) } catch { $ok = $false } finally { $ErrorActionPreference = $eap }
        $ok
    }
    function Remove-Tree([string]$Path) {   # rd /s removes a junction itself, never what it points to
        if (-not (Test-Path -LiteralPath $Path)) { return }
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        try { cmd /c rd /s /q "$Path" 2>&1 | Out-Null } finally { $ErrorActionPreference = $eap }
    }
    $train = Join-Path $root "work\port\build-train"
    $trainExe = Join-Path $train "Infamous2Recomp.exe"
    $pgoDir = Join-Path $root "work\pgo-setup"
    # tests (tools\setup_exe\README.md): PORT_PGO_TEST=fail:llvm | fail:trainbuild | fail:compile | stale | stopafter:<s>,
    # PORT_PGO_KEEP_TRAIN=1 keeps the training build; PORT_PGO_TRAIN=opening|save|opening,save picks the runs
    $test = [string]$env:PORT_PGO_TEST
    $why = $null; $used = $null; $pd = $null; $fallback = $false
    $llvmProf = if ($test -eq "fail:llvm") { $null } else { Find-LlvmProfdata }
    if (-not $llvmProf) { $why = "this LLVM has no llvm-profdata.exe" }
    # an earlier training's profile (an update): kept when this LLVM reads it
    $kept = $null
    if ($llvmProf) {
        $kept = Get-ChildItem -LiteralPath $pgoDir -Filter "setup-*.profdata" -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
        if ($kept -and -not (Test-Profile $kept.FullName)) {
            "  the last training's profile ($($kept.Name)) cannot be read by this LLVM: training again"
            Remove-Item -LiteralPath $kept.FullName -Force -ErrorAction SilentlyContinue; $kept = $null
        }
    }

    if ($kept) {
        Step 8 "translating the game, then compiling it with what the last training on this PC learned (the long part: 15-40 minutes)"
    } elseif ($why) {
        Step 8 "translating the game (the long part: 5-15 minutes)"
    } else {
        Step 8 "translating the game, then compiling a training build (the long part: 20-50 minutes)"
    }
    Invoke-Native { & $venvPy -I $rebuild --only "inputs,port,firmware,ppu,spu,jobs" }
    if ($LASTEXITCODE) { throw $buildFailed }
    if ($kept) {
        "  compile log: work\logs\compile.log"
        Invoke-Rebuild @("--only", "compile") ("use:" + $kept.FullName) ""
        if ($script:rbRc -eq 0 -and (Test-Path -LiteralPath $exe)) {
            $fallback = $true   # a game built with the last training: kept if training again does not work
            # rebuild_win.py: "PGO: n function(s) changed since the profile ... -- retrain when these are hot"
            $stale = @($script:rbOut | Where-Object { $_ -match 'PGO WARNING|-- retrain when these are hot' })
            if (-not $stale.Count -and $test -ne "stale") { $used = "the training of $($kept.LastWriteTime.ToString('yyyy-MM-dd')) still fits this version" }
            else { "  hot code changed since the last training: training again (the game just built stays if that does not work)" }
        } else { "  compiling with the last training failed: training again" }
    }
    if (-not $used -and -not $why) {
        Remove-Item -LiteralPath $trainExe -ErrorAction SilentlyContinue   # never train an older one
        "  compile log: work\logs\compile-build-train.log"
        Invoke-Rebuild @("--only", "compile") "gen" $train
        if ($script:rbRc -or -not (Test-Path -LiteralPath $trainExe) -or $test -eq "fail:trainbuild") { $why = "the training build did not compile (work\logs\compile-build-train.log)" }
    }

    Step 9 $(if ($used) { "training: not needed ($used)" } elseif ($why) { "training: skipped" }
             else { "training the build on your PC (the game plays by itself in a window: please don't touch it)" })
    if (-not $used -and -not $why) {
        New-Item -ItemType Directory -Force $pgoDir | Out-Null
        # a new name every time: ninja rebuilds nothing when only the content of a profile at the same path changed
        $pd = Join-Path $pgoDir ("setup-{0:yyyyMMdd-HHmmss}.profdata" -f (Get-Date))
        $runs = if ($env:PORT_PGO_TRAIN) { $env:PORT_PGO_TRAIN } else { "opening,save" }
        $stopAfter = if ($test -match '^stopafter:(\d+)$') { [int]$Matches[1] } else { 0 }
        $psExe = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
        $tOut = @()
        Invoke-Native { & $psExe -NoProfile -ExecutionPolicy RemoteSigned -File (Join-Path $PSScriptRoot "pgo_train.ps1") -Exe $trainExe -Profdata $pd -LlvmProfdata $llvmProf -Runs $runs -StopAfter $stopAfter } |
            ForEach-Object { $tOut += $_; $_ }
        if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $pd)) {
            $w = @($tOut | ForEach-Object { if ("$_" -match '^\s*why:\s*(.+)$') { $Matches[1].Trim() } }) | Select-Object -Last 1
            $why = "the training run gave nothing usable: $(if ($w) { $w } else { 'see the lines above' })"
            Remove-Item -LiteralPath $pd -ErrorAction SilentlyContinue; $pd = $null
        }
    } elseif (-not $used) { "  skipped: $why" }
    if ($env:PORT_PGO_KEEP_TRAIN -ne "1") { Remove-Tree $train }   # the training build is never the game

    if ($used) { Step 10 "compiling the game, optimised for this PC: done in step 8" }
    else {
        Step 10 $(if ($pd) { "compiling the game, optimised for this PC" } else { "compiling the game" })
        $built = $false
        if ($pd) {
            "  compile log: work\logs\compile.log"
            Invoke-Rebuild @("--only", "compile") ("use:" + $pd) ""
            if ($script:rbRc -or $test -eq "fail:compile") { $why = "the optimised compile failed (work\logs\compile.log)" }
            else { $built = $true; $used = "trained on this PC now" }
        }
        if (-not $built) {
            if ($fallback -and -not $pd) { $used = "the last training, partly out of date: $why" }   # the game step 8 built
            else {
                "  note: Optimise for this PC did not work this time ($why): compiling the game the normal way"
                "  compile log: work\logs\compile.log"
                Invoke-Rebuild @("--only", "compile") "" ""
                if ($script:rbRc) { throw $buildFailed }
            }
        }
    }
    if ($used) {
        "  pgo: used ($used)"
        if ($pd -and $used -like "trained*") {   # the profile the game was built with stays for updates; older ones go
            Get-ChildItem -LiteralPath $pgoDir -Filter "setup-*.profdata" -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -ne $pd } | Remove-Item -Force -ErrorAction SilentlyContinue
        }
    } else { "  pgo: not used: $why" }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "no $exe after the build" }

Step ($nSteps - 1) "desktop shortcut"
if (-not $NoShortcut) {
    $ws = New-Object -ComObject WScript.Shell
    $lnkPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "inFAMOUS 2 (PC port).lnk"
    $lnk = $ws.CreateShortcut($lnkPath)
    $lnk.TargetPath = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
    $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'play.ps1')`""
    $lnk.WorkingDirectory = $root
    $lnk.IconLocation = "$exe,0"
    $lnk.WindowStyle = 7
    $lnk.Description = "inFAMOUS 2, natively on Windows (your own copy, recompiled)"
    $lnk.Save()
    "  $lnkPath"
} else { "  skipped (-NoShortcut)" }

Step $nSteps "cleaning up"
if ($Cleanup) { & (Join-Path $PSScriptRoot "setup_cleanup.ps1") -KeepBuildCache:(-not $NoBuildCache -and $env:PORT_BUILD_CACHE -ne "0") }
else { "  kept the build tools and files (no -Cleanup): tools\setup_cleanup.ps1 deletes them" }
"== done in $([int]$sw.Elapsed.TotalMinutes) min: play with the desktop shortcut or 'Play inFAMOUS 2.cmd'. RPCS3 is not needed to play (an update reads the game's update and firmware from it again: keep it)."
exit 0
