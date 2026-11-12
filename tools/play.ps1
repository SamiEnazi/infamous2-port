# Play the native Windows build of inFAMOUS 2 (EU BCES01143 + v1.04 update).
#   tools\play.ps1 [-Fullscreen] [-Volume 100] [-Fps 60] [-SpuLighting] [-NoLog] [-NoHud]
# or double-click "Play inFAMOUS 2.cmd" in the project root.
#
# In the game:
#   F1                 FPS counter / frame-time graph on or off
#   F2, or hold L3+R3  PC settings menu (frame cap, vsync, fullscreen, anti-aliasing,
#                      sharpening, texture filtering, upscaling, volume, language, controls,
#                      HUD); Esc / Circle closes it, L1/R1 switch pages
#   Alt+Enter or F11   fullscreen on/off
# The menu saves to %APPDATA%\inFAMOUS2-port\settings.ini and the next start uses it (to settings.ini in the
# project folder when a portable.txt is there: then mods.ini and spots.ini stay there too).
# -Fps / -Volume / -Fullscreen given here win over the file for this session
# (-Fps: frame cap, 72 = smooth on a 144 Hz screen, 0 = uncapped).
#
# Language: the game is told the Windows display language (English when the game has
# none of it), as a PS3 set to that language would. F2 > Audio > Language picks another
# one, and the voice and the text apart (only what your disc has); saved as PS3_LANG,
# PS3_VOICE_LANG and PS3_TEXT_LANG in settings.ini, used from the next start.
#
# Controls (engine\docs\INPUT.md has everything):
#   Controller: any pad SDL knows (the community controller database is built in) -- the
#   8BitDo in Switch mode, DualShock 4/5, Xbox -- paired before the game starts or switched on
#   later. Buttons by POSITION as on a PlayStation pad (bottom = Cross, right = Circle, ...).
#   DS4Windows is closed first (its virtual pad doubles the input).
#   Keyboard + mouse: WASD move, mouse = camera (click in the window to capture the mouse),
#   right button = aim (L1), left button = R1, Space = Cross, Shift = Circle, F = Square,
#   Q = Triangle, Enter = START, Tab/M = map.
#   Esc opens the PC menu (and frees the mouse); Alt+F4 or the window's close button quits.
#   Remapping, dead zones, sensitivity: input.toml in the project folder (made on first run
#   from engine\docs\input.example.toml).
#
# Every session gets its own folder, play\<date-time>\, with a light record of how it ran:
#   playlog.tsv   one line per second (fps, frame times, slow frames, draws per frame), one per
#                 frame over 50 ms, and a MARK each time F9 is pressed in the game window --
#                 press it when something feels slow or stutters
#   threads.tsv   CPU use of the busiest threads, every 10 s
#   hitches.tsv   (a build with the hitch catcher) what every guest PPU/SPU thread was doing in
#                 each frame over 80 ms, sampled every 1 ms (tools\hitch_stacks.py names it)
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

# --- your game folder (GitHub issue #13) ------------------------------------------------
# game\disc is a link to your game folder (the one that holds PS3_GAME), with that folder's full path.
# A game folder kept inside the port's folder (or beside it) keeps the OLD path in the link when the
# port's folder is renamed or moved: the game then shows its intro logos (they come from the HDD
# install) and a black screen. Find the folder at the new place and link it again; else say so.
$discLink = Join-Path $game "disc"
$discItem = Get-Item -LiteralPath $discLink -Force -ErrorAction SilentlyContinue
if ($discItem -and $discItem.LinkType -eq "Junction" -and -not (Test-Path -LiteralPath (Join-Path $discLink "PS3_GAME\PARAM.SFO"))) {
    $old = ([string]@($discItem.Target)[0]).TrimEnd('\')
    $parts = @($old -split '\\' | Where-Object { $_ })
    $found = $null
    foreach ($base in @($root, (Split-Path -Parent $root))) {      # the same folder under the port's (or its parent's) new place
        for ($i = 1; $i -lt $parts.Count -and -not $found; $i++) {
            $cand = Join-Path $base ($parts[$i..($parts.Count - 1)] -join '\')
            if ($cand -ne $old -and (Test-Path -LiteralPath (Join-Path $cand "PS3_GAME\PARAM.SFO"))) { $found = $cand }
        }
        if ($found) { break }
    }
    if ($found) {
        cmd /c rmdir "$discLink" | Out-Null                          # the link only, never your game
        New-Item -ItemType Junction -Path $discLink -Target $found | Out-Null
        "Your game folder moved (it was $old): game\disc links to $found now."
    } else {
        "Your game folder is not where it was: game\disc still points to $old, and that folder is gone."
        "Move it back there, or run Setup.exe and press Install to choose your game folder again."
        "(Renaming or moving the port's own folder is fine; this is about the folder that holds PS3_GAME.)"
        exit 1
    }
}
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
# The controller list and the session summary are Python helpers: optional. Setup deletes its
# build tools (Python among them) after the build, so a player's PC may have none; the game
# starts the same without them. Not the Microsoft Store's "python" stand-in (it only offers an install).
$pyCmd = Get-Command python -CommandType Application -ErrorAction SilentlyContinue |
         Where-Object { $_.Source -notlike "*\WindowsApps\*" } | Select-Object -First 1
function Invoke-PyHelper([string[]]$PyArgs) {
    if (-not $pyCmd) { return }
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $pyCmd.Source @PyArgs 2>&1 | ForEach-Object { "$_" } } catch {} finally { $ErrorActionPreference = $eap }
}
if ($pyCmd) {
    $probe = Invoke-PyHelper @("-I", (Join-Path $PSScriptRoot "pad_probe.py"), "--positional") | Where-Object { $_ -match '^\s+\[\d+\]' }
    if ($probe) { "Controller(s):"; $probe | ForEach-Object { "  " + $_.Trim() } }
    else { "No controller connected yet -- switch it on (8BitDo: Start), it is picked up while the game runs." }
}

# controls file: input.toml in the project folder (survives rebuilds), first made from the example
$inputToml = Join-Path $root "input.toml"
if (-not (Test-Path $inputToml)) {
    $ex = Join-Path $root "engine\docs\input.example.toml"
    if (Test-Path $ex) { Copy-Item $ex $inputToml; "Made input.toml (your controls) from the example." }
}

# game mods: the "mods" folder (F2 > Mods), made on the first start with a note on what goes in it
$modsDir = Join-Path $root "mods"
if (-not (Test-Path $modsDir)) { New-Item -ItemType Directory $modsDir | Out-Null }
$modsNote = Join-Path $modsDir "README.txt"
if (-not (Test-Path $modsNote)) {
    Set-Content -LiteralPath $modsNote -Encoding utf8 -Value @(
        "inFAMOUS 2 mods go here: one folder per mod, the folder with the mod's mod.json in it.",
        "",
        "Mods made for the inFAMOUS Mod Manager work as they are: unzip the mod into this folder",
        "(e.g. mods\Red Jacket\mod.json) and start the game. No Unlocker is needed and your game",
        "files are never changed: the port hands the game the mod's files in their place.",
        "",
        "In the game, F2 > Mods lists them: turn each one on or off and pick its options there, or",
        "add one with ""Add a mod from a zip"". Changes apply the next time the game starts (""Restart",
        "to apply"" does that at once). When two mods change the same file, the one you chose with",
        """Use this one instead"" is used, else the first by folder name, and the other is left out.")
}

$vars = @{
    PS3_VFS_ROOT                = "$game\disc"
    PS3_HDD0_ROOT               = "$game\dev_hdd0"
    PS3_DEV_FLASH               = (Get-DevFlash)
    PS3_GAMEDATA_ROOT           = "$game\gamedata"
    PS3_TITLE                   = "inFAMOUS 2"
    PS3_GAME_MODS_DIR           = $modsDir   # F2 > Mods (the Mod Manager's mod folders; engine\docs\MODS.md)
    # F2 > Mods > "Restart to apply": the game ends with exit code 0x52455354 and this script starts
    # it again (below); without this variable the game starts itself again.
    PS3_LAUNCHER_RESTART        = "1"
    # No PS3_LANG here (GitHub issues #2, #3): a variable set here wins over settings.ini, so
    # it pinned every player to English. Unset, the game follows the Windows display language,
    # or the menu's PS3_LANG / PS3_VOICE_LANG / PS3_TEXT_LANG lines (read before the game starts).
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
    PS3_INPUT_MAP               = $(if (Test-Path $inputToml) { $inputToml } else { "" })
}
# portable.txt in the project folder (the sign RecompLauncher and other launchers know): the menu's settings.ini,
# the mods.ini beside it and the saved spots stay in this folder, as the saves, input.toml and the session logs do.
$settingsIni = Join-Path $env:APPDATA "inFAMOUS2-port\settings.ini"
if (Test-Path -LiteralPath (Join-Path $root "portable.txt")) {
    $settingsIni = Join-Path $root "settings.ini"
    $vars["PS3_OVERLAY_INI"] = $settingsIni
    $vars["PS3_CHEATS_SPOTS"] = Join-Path $root "spots.ini"
}
# Controller input in the background: on, unless settings.ini has its own SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS
# line (RecompLauncher's "Controller in the background" setting writes one; the game exports the file's lines
# before SDL starts). A variable set here would win over that line.
$bgOwn = $false
try { $bgOwn = [bool](Select-String -LiteralPath $settingsIni -Pattern '^\s*SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS\s*=' -Quiet -ErrorAction Stop) } catch {}
if (-not $bgOwn) { $vars["SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"] = "1" }
if ($Fullscreen) { $vars["PS3RECOMP_VK_FULLSCREEN"] = "1" }
if ($PSBoundParameters.ContainsKey('Fps')) { $vars["PS3RECOMP_FPS_CAP"] = [string]$Fps }
if ($PSBoundParameters.ContainsKey('Volume')) { $vars["PS3_AUDIO_VOLUME"] = [string]$Volume }
if ($NoHud) { $vars["PS3_OVERLAY_HUD"] = "0" }
if ($NoKbm) { $vars["PS3_KBM"] = "0" }
if ($Trace) { $vars["PS3_PAD_TRACE"] = "1" }
if (-not $NoLog) { $vars["PS3RECOMP_PLAYLOG"] = "$out\playlog.tsv" }
# --- updates (tools\update_check.ps1) -----------------------------------------------------
# A copy Setup.exe installed (port-version.txt) asks GitHub for a newer version in the background while
# the game starts (at most every 6 hours; the game never waits, offline it just starts). The game shows
# the answer (a notice, F2 > Update), and when the player picks "Update when I quit" or "Quit and update
# now" it ends with exit code 0x55504454 ("UPDT"): Setup.exe --update then installs the new version
# (below, after the game ended). Not for RecompLauncher (it updates from the releases itself, and
# Setup.exe --play says so), a git clone, a folder without port-version.txt, or a -RecordRoute test.
# PS3_UPDATE_CHECK=0 in the settings file (F2 > Update's checkbox): only "Check now" asks GitHub.
$updDir = Join-Path $root "work\update"
$updBy = if ($RecordRoute) { "test" }
         elseif ($env:PS3_PORT_LAUNCHER -or $env:RECOMPLAUNCHER_VERSION) { "recomplauncher" }
         elseif (Test-Path (Join-Path $root ".git")) { "git" }
         elseif (-not (Test-Path (Join-Path $root "port-version.txt"))) { "none" }
         else { "" }
$updAuto = $true
if (-not $updBy) {
    New-Item -ItemType Directory -Force $updDir | Out-Null
    # the last session's answer may be about the version this copy had before an update: the game waits for a new one
    foreach ($f in "status.txt", "check-now") { Remove-Item -LiteralPath (Join-Path $updDir $f) -ErrorAction SilentlyContinue }
    $vars["PS3_UPDATE_DIR"] = $updDir
    $vars["PS3_LAUNCHER_UPDATE"] = "1"
    # the settings file the game reads (PS3_OVERLAY_INI: set above, e.g. a portable folder, or by the caller)
    $ini = if ($vars["PS3_OVERLAY_INI"]) { $vars["PS3_OVERLAY_INI"] } elseif ($null -ne $env:PS3_OVERLAY_INI) { $env:PS3_OVERLAY_INI }
           else { Join-Path $env:APPDATA "inFAMOUS2-port\settings.ini" }
    $auto = if ($env:PS3_UPDATE_CHECK) { $env:PS3_UPDATE_CHECK }
            elseif ($ini -and (Test-Path -LiteralPath $ini)) {
                Select-String -LiteralPath $ini -Pattern '^\s*PS3_UPDATE_CHECK\s*=\s*(\S*)' | Select-Object -Last 1 |
                    ForEach-Object { $_.Matches[0].Groups[1].Value }
            }
    $updAuto = -not ($auto -eq "0")
} else { $vars["PS3_UPDATE_BY"] = $updBy }

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
    # the tests replay it in English (native_run.ps1: PS3_LANG=en): the same voice lines and
    # movie lengths, whatever settings.ini or Windows say ("same" = the game's own dub and text)
    $vars["PS3_LANG"] = "en"; $vars["PS3_VOICE_LANG"] = "same"; $vars["PS3_TEXT_LANG"] = "same"
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
# Stream the logs to disk while it runs (they are too big to hold in memory).
$so = [System.IO.StreamWriter]::new("$out\stdout.log")
$se = [System.IO.StreamWriter]::new("$out\stderr.log")
# Thread CPU every 10 s: which thread is the limit (main / RSX / SPU workers). Reading the
# thread times costs microseconds; nothing is injected into the game.
$t0 = Get-Date
$tw = $null
if (-not $NoLog) {
    $tw = [System.IO.StreamWriter]::new("$out\threads.tsv")
    $tw.WriteLine("t_s`thost_tid`tcpu_pct")
}
# F2 > Mods > "Restart to apply" ends the game with this exit code ("REST"): it is started again
# here, with the same settings, in the same session folder (stdout.log / stderr.log go on after a
# "=== restart" line; each earlier run's playlog is kept as playlog.<n>.tsv). The game comes back
# at its title screen: Continue there goes on from the last save.
$restartCode = 0x52455354
# The update check, in this process but on its own runspace: the game starts at once. It writes
# work\update\status.txt, then answers the game's "Check now" (work\update\check-now) while it runs.
$updWatch = $null
if ($vars["PS3_UPDATE_DIR"]) {
    try {
        $ps = [powershell]::Create()
        [void]$ps.AddScript({
            param($Script, $Root, $Dir, $Auto)
            $ErrorActionPreference = "Continue"
            try { if ($Auto) { & $Script -Root $Root | Out-Null } else { & $Script -Root $Root -CacheOnly | Out-Null } } catch {}
            $req = Join-Path $Dir "check-now"
            while ($true) {
                Start-Sleep -Seconds 2
                if (Test-Path -LiteralPath $req) {
                    Remove-Item -LiteralPath $req -ErrorAction SilentlyContinue
                    try { & $Script -Root $Root -Force | Out-Null } catch {}
                }
            }
        }).AddArgument((Join-Path $PSScriptRoot "update_check.ps1")).AddArgument($root).AddArgument($updDir).AddArgument($updAuto)
        $updWatch = @{ PS = $ps; Async = $ps.BeginInvoke() }
    } catch { "The update check did not start ($_); the game starts the same." }
}
$run = 0
do {
    $run++
    if ($run -gt 1) {
        "Restarting inFAMOUS 2 for the new mods (restart $($run - 1)): it comes back at the title screen -- choose Continue to go on from your last save."
        $mark = "=== {0:HH:mm:ss} restart {1} (F2 > Mods: Restart to apply) ===" -f (Get-Date), ($run - 1)
        $so.WriteLine($mark); $so.Flush(); $se.WriteLine($mark); $se.Flush()
        if (-not $NoLog -and (Test-Path "$out\playlog.tsv")) { Move-Item -Force "$out\playlog.tsv" "$out\playlog.$($run - 1).tsv" }
        # the hitch catcher's stacks of that run's frozen frames (same clock as its playlog)
        if (-not $NoLog -and (Test-Path "$out\hitches.tsv")) { Move-Item -Force "$out\hitches.tsv" "$out\hitches.$($run - 1).tsv" }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if ($RecordRoute) {
        "" | Set-Content (Join-Path $rdrv "live.tsv")
        $routeDrv = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot "fast_driver.ps1"),
            '-Steps', (Join-Path $PSScriptRoot $(if ($From -eq 'opening') { 'record_route_opening.steps' } else { 'record_route.steps' })), '-Rec', $rdrv, '-GamePid', $p.Id,
            '-LiveLog', $(if ($From -eq 'opening') { "$out\stderr.log" } else { (Join-Path $rdrv "live.tsv") })   # opening: the session's live stderr (cutscene events)
    }
    $t1 = $p.StandardOutput.BaseStream.CopyToAsync($so.BaseStream)
    $t2 = $p.StandardError.BaseStream.CopyToAsync($se.BaseStream)
    if ($tw) {
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
    } else {
        $p.WaitForExit()
    }
    try { $t1.Wait(5000) | Out-Null; $t2.Wait(5000) | Out-Null } catch {}
} while ($p.ExitCode -eq $restartCode -and $run -lt 20 -and -not $RecordRoute)
if ($updWatch) { try { [void]$updWatch.PS.BeginStop($null, $null) } catch {} }
if ($tw) { $tw.Close() }
$so.Close(); $se.Close()
$lockFs.Close()   # agents may run the game again
Remove-Item $myTicket -ErrorAction SilentlyContinue
"Exited (code 0x{0:X}) after {1:F0} min. Logs: $out" -f $p.ExitCode, ((Get-Date) - $t0).TotalMinutes
# F2 > Update ("Update when I quit" / "Quit and update now"): the game ended with 0x55504454 ("UPDT").
# Setup.exe --update installs the new version in its own window (the saves, settings, mods and
# controls are kept) and offers Play when it is done.
if ($p.ExitCode -eq 0x55504454 -and $vars["PS3_UPDATE_DIR"]) {
    try { & (Join-Path $PSScriptRoot "update_check.ps1") -Root $root -StartUpdate } catch { "Could not start the update: $_ (run Setup.exe in $root)" }
}
if (-not $NoLog -and (Test-Path "$out\playlog.tsv")) {
    # the cap the session ended at: the menu can change it while playing
    $cap = 60
    $capLine = Select-String -Path "$out\stderr.log" -Pattern 'frame cap (off|[\d.]+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($capLine) { $c = $capLine.Matches[0].Groups[1].Value; $cap = $(if ($c -eq 'off') { 0 } else { [double]$c }) }
    Invoke-PyHelper @("-I", (Join-Path $PSScriptRoot "playlog_report.py"), $out, "--cap", [string]$cap)
}
