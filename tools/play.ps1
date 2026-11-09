# Play the native Windows build of inFAMOUS 2 (EU BCES01143 + v1.04 update).
#   tools\play.ps1 [-Fullscreen] [-Volume 100] [-Fps 60] [-SpuLighting] [-NoLog] [-NoHud]
# or double-click "Play inFAMOUS 2.cmd" in the project root.
#
# In the game:
#   F1                 FPS counter / frame-time graph on or off
#   F2, or hold L3+R3  PC settings menu (frame cap, vsync, fullscreen, anti-aliasing,
#                      sharpening, texture filtering, upscaling, volume, controls, HUD);
#                      Esc / Circle closes it, L1/R1 switch pages
#   Alt+Enter or F11   fullscreen on/off
# The menu saves to %APPDATA%\inFAMOUS2-port\settings.ini and the next start uses it.
# -Fps / -Volume / -Fullscreen given here win over the file for this session
# (-Fps: frame cap, 72 = smooth on a 144 Hz screen, 0 = uncapped).
#
# Controls (the toolkit's docs\INPUT.md has everything):
#   Controller: any pad SDL knows (the community controller database is built in) -- the
#   8BitDo in Switch mode, DualShock 4/5, Xbox -- paired before the game starts or switched on
#   later. Buttons by POSITION as on a PlayStation pad (bottom = Cross, right = Circle, ...).
#   DS4Windows is closed first (its virtual pad doubles the input).
#   Keyboard + mouse: WASD move, mouse = camera (click in the window to capture the mouse),
#   right button = aim (L1), left button = R1, Space = Cross, Shift = Circle, F = Square,
#   Q = Triangle, Enter = START, Tab/M = map.
#   Esc opens the PC menu (and frees the mouse); Alt+F4 or the window's close button quits.
#   Remapping, dead zones, sensitivity: input.toml in the project folder (made on first run
#   from the toolkit's docs\input.example.toml).
#
# Every session gets its own folder, play\<date-time>\, with a light record of how it ran:
#   playlog.tsv   one line per second (fps, frame times, slow frames, draws per frame), one per
#                 frame over 50 ms, and a MARK each time F9 is pressed in the game window --
#                 press it when something feels slow or stutters
#   threads.tsv   CPU use of the busiest threads, every 10 s
#   summary.txt   written when the game exits (tools\playlog_report.py)
#   stdout.log / stderr.log
# -NoLog turns the frame and thread records off.
param(
    [switch]$Fullscreen,
    [int]$Volume = 100,              # 0..100 (only when given: else the menu's setting)
    [int]$Fps = 60,                  # frame cap; 0 = uncapped (only when given: else the menu's)
    [switch]$SpuLighting,            # run deferred lighting + SSAO on the (slower) SPU path
    [switch]$NoLog,
    [switch]$NoHud,                  # start with the FPS counter hidden (F1 still shows it)
    [switch]$NoKbm,                  # keyboard + mouse off (controller only)
    [switch]$Trace,                  # log every controller packet the game receives (PS3_PAD_TRACE=1)
    [string]$Exe = "",
    [string]$RecordRoute = "",       # record a test route: a COPY of the test save, movies cut, your controller -> work\routes\<name>.pad (testspeed)
    [ValidateSet("city", "opening")][string]$From = "city"   # -RecordRoute from the save (city) or from a new game (opening: the anchor is the "look up" prompt)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "port_paths.ps1")   # Get-PortPath rpcs3 (config.local.json, written by Setup.cmd)
if (-not $Exe) { $Exe = Join-Path $root "work\port\build\Infamous2Recomp.exe" }
if (-not (Test-Path $Exe)) { throw "not built yet: $Exe (python -I tools\rebuild_win.py)" }
$game = Join-Path $root "game"
$out = Join-Path $root ("play\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $out, "$game\gamedata" | Out-Null

# --- saves ----------------------------------------------------------------------------
# Your saves live in game\gamedata\dev_hdd0\home\00000001\savedata\BCES01143*. (Test runs never
# touch them: native_run.ps1 gives every test its own save folder.) Before each session they are
# copied to saves-backup\<date-time>\ in the project folder, when they changed since the last
# copy; the newest 20 copies are kept.
$saves = Join-Path $game "gamedata\dev_hdd0\home\00000001\savedata"
$bk = Join-Path $root "saves-backup"
try {
    $now = @(Get-ChildItem $saves -Recurse -File -ErrorAction SilentlyContinue)
    if ($now.Count) {
        $sig = ($now | Sort-Object FullName | ForEach-Object { "{0}|{1}|{2}" -f $_.FullName.Substring($saves.Length), $_.Length, $_.LastWriteTimeUtc.Ticks }) -join "`n"
        $last = Get-ChildItem $bk -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
        $lastSig = if ($last -and (Test-Path "$($last.FullName)\files.txt")) { [System.IO.File]::ReadAllText("$($last.FullName)\files.txt") } else { "" }
        if ($sig -ne $lastSig) {
            $dst = Join-Path $bk (Get-Date -Format "yyyyMMdd-HHmmss")
            New-Item -ItemType Directory -Force $dst | Out-Null
            Copy-Item "$saves\*" $dst -Recurse -Force
            [System.IO.File]::WriteAllText("$dst\files.txt", $sig)
            "Saves backed up to $dst"
            Get-ChildItem $bk -Directory | Sort-Object Name -Descending | Select-Object -Skip 20 |
                ForEach-Object { Remove-Item $_.FullName -Recurse -Force }
        }
    }
} catch { "save backup failed (the game still starts): $_" }

# --- controller ---------------------------------------------------------------------
$ds4w = Get-Process DS4Windows -ErrorAction SilentlyContinue
if ($ds4w) {
    "Closing DS4Windows (it would add a second, virtual controller)..."
    $ds4w | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}
$probe = & python -I (Join-Path $PSScriptRoot "pad_probe.py") --positional 2>&1 |
         Where-Object { $_ -match '^\s+\[\d+\]' }
if ($probe) { "Controller(s):"; $probe | ForEach-Object { "  " + $_.Trim() } }
else { "No controller connected yet -- switch it on (8BitDo: Start), it is picked up while the game runs." }

# controls file: input.toml in the project folder (survives rebuilds), first made from the example
$inputToml = Join-Path $root "input.toml"
if (-not (Test-Path $inputToml)) {
    $ex = Join-Path $root "toolkit\docs\input.example.toml"
    if (Test-Path $ex) { Copy-Item $ex $inputToml; "Made input.toml (your controls) from the example." }
}

$vars = @{
    PS3_VFS_ROOT                = "$game\disc"
    PS3_HDD0_ROOT               = "$game\dev_hdd0"
    PS3_DEV_FLASH               = (Get-DevFlash)
    PS3_GAMEDATA_ROOT           = "$game\gamedata"
    PS3_TITLE                   = "inFAMOUS 2"
    PS3_LANG                    = "en"
    GCM_SEMA_ACQUIRE            = "1"
    GCM_SEMA_ACQUIRE_MS         = "200"
    GCM_PARK_GRACE_MS           = "1000"
    PS3RECOMP_VK_WINDOW         = "1"
    PS3RECOMP_VK_GUEST_PROGRAMS = "1"
    PS3RECOMP_ENG_GPU_LIGHT     = $(if ($SpuLighting) { "" } else { "07000B80:07384B80" })
    PS3RECOMP_ENG_GPU_SSAO      = $(if ($SpuLighting) { "" } else { "0F800000" })
    # stderr goes to the session's stderr.log, and a redirected stderr turns the per-wait
    # diagnostics on (ps3_log.h): ~500 lines/s through one FILE lock. Off for play.
    PS3_VERBOSE                 = $(if ($env:PS3_VERBOSE) { $env:PS3_VERBOSE } else { "0" })
    # defaults below settings.ini (the in-game menu); -Fps/-Volume/-Fullscreen override both.
    # The picture defaults are the best look for the cost (Sami, 2026-11-09: "best visuals to
    # performance ratio, 60-70+ fps"): softer sun shadows, full-res AO at 125%, dithered light,
    # full-resolution effects, light sharpening; together ~1 ms of GPU per frame (city ~88 -> ~83
    # fps uncapped on an RTX 2070 Super). The engine's own defaults stay the PS3 look (tests).
    # Native 1080p (PS3RECOMP_RENDER_SCALE=1.5; inf2-resolution 2026-11-09): heaviest city view
    # 81 -> 60 fps with the extras (lows 58), other views 81-91; F2 > Graphics sets 720p back.
    PS3_OVERLAY_DEFAULTS        = ("PS3RECOMP_FPS_CAP=60;PS3_AUDIO_VOLUME=100;PS3RECOMP_RENDER_SCALE=1.5;PS3RECOMP_SOFT_SHADOWS=2;PS3RECOMP_SSAO=2;" +
                                   "PS3RECOMP_SSAO_STRENGTH=125;PS3RECOMP_LIGHT_DITHER=1;PS3RECOMP_FX_RES=full;PS3RECOMP_SHARPEN=0.1" +
                                   $(if (Test-Path (Join-Path $root "work\texpacks\fire-smoke-hd")) { ";PS3_MODS=fire-smoke-hd" } else { "" }))
    # Buttons by position (a Nintendo-layout pad's bottom button is Cross, not Circle), and
    # controller input even when the game window does not have focus: SDL otherwise drops it,
    # and a game started from a console window often starts without focus.
    SDL_GAMECONTROLLER_USE_BUTTON_LABELS = "0"
    SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS = "1"
    PS3_INPUT_MAP               = $(if (Test-Path $inputToml) { $inputToml } else { "" })
}
if ($Fullscreen) { $vars["PS3RECOMP_VK_FULLSCREEN"] = "1" }
if ($PSBoundParameters.ContainsKey('Fps')) { $vars["PS3RECOMP_FPS_CAP"] = [string]$Fps }
if ($PSBoundParameters.ContainsKey('Volume')) { $vars["PS3_AUDIO_VOLUME"] = [string]$Volume }
if ($NoHud) { $vars["PS3_OVERLAY_HUD"] = "0" }
if ($NoKbm) { $vars["PS3_KBM"] = "0" }
if ($Trace) { $vars["PS3_PAD_TRACE"] = "1" }
if (-not $NoLog) { $vars["PS3RECOMP_PLAYLOG"] = "$out\playlog.tsv" }

# The machine-wide game lock (tools\with_game_lock.ps1): agents' test runs wait while Sami
# plays, and a test run already going finishes first (they last up to ~7 min).
$lockFs = $null
$lockPath = Join-Path $root "work\game.lock"
$lockInfo = Join-Path $root "work\game.lock.owner"
# Sami goes first: a ticket that sorts before every agent's in the first-come queue
# (with_game_lock.ps1 lets only the oldest live ticket take the lock).
$qdir = Join-Path $root "work\game.lock.queue"
New-Item -ItemType Directory -Force $qdir | Out-Null
$myTicket = Join-Path $qdir ("00000000000000000_{0}.t" -f $PID)
Set-Content -Path $myTicket -Value "Sami (play.ps1)"
while (-not $lockFs) {
    try { $lockFs = [System.IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None') }
    catch {
        $o = if (Test-Path $lockInfo) { Get-Content $lockInfo -ErrorAction SilentlyContinue } else { "?" }
        "A test run is going ($o) -- the game starts when it ends (a few minutes)..."
        Start-Sleep -Seconds 5
    }
}
Set-Content -Path $lockInfo -Value ("Sami playing since {0:HH:mm:ss}" -f (Get-Date))

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$root\work\EBOOT.elf`""
$psi.WorkingDirectory = $out
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
# -RecordRoute <name> (2026-11-08): Sami drives a route once, tests replay it (tools\fast.ps1, '#! replay').
# Your own saves are never touched: the game starts from a fresh COPY of the test save
# (work\saves\first-save\gamedata, "3. Forward Momentum"), the cutscene movies are the 4 s placeholder,
# and recording starts by itself when the gameplay HUD is back after the Kuo cutscene (a voice says
# "Recording"). Do not touch the controller (or press F1) before that. Quit (Alt+F4) to end it.
$routeDrv = $null
if ($RecordRoute) {
    $rdir = Join-Path $root "work\routes"
    New-Item -ItemType Directory -Force $rdir | Out-Null
    $rgd = Join-Path $rdir "$RecordRoute.gamedata"; $rdrv = Join-Path $rdir "$RecordRoute.driver"
    foreach ($d in $rgd, $rdrv) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
    New-Item -ItemType Directory -Force $rdrv | Out-Null
    if ($From -eq "city") { Copy-Item -Recurse (Join-Path $root "work\saves\first-save\gamedata") $rgd }
    else { New-Item -ItemType Directory -Force "$rgd\dev_hdd0\home\00000001\savedata" | Out-Null }   # opening: no save, a new game
    $fm = Join-Path $root "work\fastmovies"
    if (-not (Test-Path "$fm\disc\PS3_GAME\USRDIR\movies\cutscene")) { & powershell -NoProfile -File (Join-Path $PSScriptRoot "make_fastmovies.ps1") | Out-Host }
    $vars["PS3_GAMEDATA_ROOT"] = $rgd
    $vars["PS3_VFS_ROOT"] = "$fm\disc"; $vars["PS3_HDD0_ROOT"] = "$fm\dev_hdd0"
    $vars["PS3_PAD_RECORD"] = Join-Path $rdir "$RecordRoute.pad"
    $vars["PAD_SCRIPT_WAIT"] = Join-Path $rdrv "anchor.flag"
    $vars["PS3_OVERLAY_HUD"] = "0"            # the HUD check looks at the top-left corner
    $vars["PS3RECOMP_VK_FULLSCREEN"] = "0"    # the HUD check compares the 1280x720 window (settings.ini may say fullscreen)
    if ($From -eq 'opening') { "RECORDING A TEST ROUTE '$RecordRoute' FROM THE OPENING: press START at the title, then do not touch the controller until a voice says 'Recording'. Quit with Alt+F4 when done." }
    else { "RECORDING A TEST ROUTE '$RecordRoute': wait, do not touch the controller until a voice says 'Recording'. Quit with Alt+F4 when done." }
}
foreach ($k in $vars.Keys) { if ($vars[$k] -ne "") { $psi.EnvironmentVariables[$k] = $vars[$k] } }

$given = @()
if ($PSBoundParameters.ContainsKey('Volume')) { $given += "volume $Volume" }
if ($PSBoundParameters.ContainsKey('Fps')) { $given += "cap $(if ($Fps) { "$Fps fps" } else { 'off' })" }
if ($Fullscreen) { $given += "fullscreen" }
if ($SpuLighting) { $given += "SPU lighting" }
"Starting inFAMOUS 2$(if ($given) { ' (' + ($given -join ', ') + ')' })... F1 = FPS counter, F2 = settings."
if (-not $NoLog) { "Press F9 in the game window whenever something feels slow or stutters. Session folder: $out" }
$p = [System.Diagnostics.Process]::Start($psi)
if ($RecordRoute) {
    "" | Set-Content (Join-Path $rdrv "live.tsv")
    $routeDrv = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot "fast_driver.ps1"),
        '-Steps', (Join-Path $PSScriptRoot $(if ($From -eq 'opening') { 'record_route_opening.steps' } else { 'record_route.steps' })), '-Rec', $rdrv, '-GamePid', $p.Id,
        '-LiveLog', $(if ($From -eq 'opening') { "$out\stderr.log" } else { (Join-Path $rdrv "live.tsv") })   # opening: the session's live stderr (cutscene events)
}
# Stream the logs to disk while it runs (they are too big to hold in memory).
$so = [System.IO.StreamWriter]::new("$out\stdout.log")
$se = [System.IO.StreamWriter]::new("$out\stderr.log")
$t1 = $p.StandardOutput.BaseStream.CopyToAsync($so.BaseStream)
$t2 = $p.StandardError.BaseStream.CopyToAsync($se.BaseStream)

# Thread CPU every 10 s: which thread is the limit (main / RSX / SPU workers). Reading the
# thread times costs microseconds; nothing is injected into the game.
$t0 = Get-Date
if (-not $NoLog) {
    $tw = [System.IO.StreamWriter]::new("$out\threads.tsv")
    $tw.WriteLine("t_s`thost_tid`tcpu_pct")
    $prev = @{}; $prevT = Get-Date
    while (-not $p.WaitForExit(10000)) {
        try {
            $p.Refresh(); $now = Get-Date
            $wall = ($now - $prevT).TotalMilliseconds; $cur = @{}
            foreach ($th in $p.Threads) {
                try { $cur[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch {}
            }
            $rows = foreach ($id in $cur.Keys) {
                $d = $cur[$id] - $(if ($prev.ContainsKey($id)) { $prev[$id] } else { 0 })
                [pscustomobject]@{ id = $id; pct = 100.0 * $d / $wall }
            }
            $ts = [int]($now - $t0).TotalSeconds
            foreach ($r in ($rows | Where-Object { $_.pct -ge 5 } | Sort-Object pct -Descending)) {
                $tw.WriteLine(("{0}`t{1}`t{2:F0}" -f $ts, $r.id, $r.pct))
            }
            $tw.Flush(); $prev = $cur; $prevT = $now
        } catch {}
    }
    $tw.Close()
} else {
    $p.WaitForExit()
}
try { $t1.Wait(5000) | Out-Null; $t2.Wait(5000) | Out-Null } catch {}
$so.Close(); $se.Close()
$lockFs.Close()   # agents may run the game again
Remove-Item $myTicket -ErrorAction SilentlyContinue
"Exited (code 0x{0:X}) after {1:F0} min. Logs: $out" -f $p.ExitCode, ((Get-Date) - $t0).TotalMinutes
if (-not $NoLog -and (Test-Path "$out\playlog.tsv")) {
    # the cap the session ended at: the menu can change it while playing
    $cap = 60
    $capLine = Select-String -Path "$out\stderr.log" -Pattern 'frame cap (off|[\d.]+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($capLine) { $c = $capLine.Matches[0].Groups[1].Value; $cap = $(if ($c -eq 'off') { 0 } else { [double]$c }) }
    & python -I (Join-Path $PSScriptRoot "playlog_report.py") $out --cap $cap
}
