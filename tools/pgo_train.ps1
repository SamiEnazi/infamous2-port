# "Optimise for this PC" (Setup.exe's checkbox, tools\setup_build.ps1 -Pgo): profile-guided optimisation
# (PGO) trained on the player's own PC. A profile comes from running the game, so it never leaves the PC
# (tools\publish_export.ps1 refuses *.profdata, *.profraw and work\): every player makes their own.
#   setup_build.ps1 -Pgo  1. compiles an instrumented game (rebuild_win.py PORT_PGO=gen) -> work\port\build-train
#   this script           2. runs it by itself in a window, muted, a scripted controller playing:
#                            a. a NEW GAME (always): the title, the opening montage and cutscenes, the tutorial
#                               and the fight on the docks (the inputs of tools\fast_combat.steps), ~6 minutes
#                            b. the player's newest story save, when there is one: ~2 minutes of looking
#                               around, walking, jumping and fighting wherever it is
#                         3. merges what the runs counted into one profile (llvm-profdata merge)
#   setup_build.ps1 -Pgo  4. compiles the game with it (PORT_PGO=use:<profile>), the normal way when this
#                            script exits non-zero: the player always ends with a working game
#   tools\pgo_train.ps1 -Exe <instrumented exe> -Profdata <out .profdata> -LlvmProfdata <llvm-profdata.exe>
#                       [-Runs opening,save] [-StopAfter <s>]
#
# Robust for players (work\brain\PLAYBOOK.md C.2). Nothing here looks at the screen, and nothing a run needs
# can be missed:
#  - The game is driven from its own log (the menu, the montage, the city-falls cutscene) and its own world
#    frame (the save run), never by a screen match. The title is passed by pressing START until the opening
#    montage starts. The scripted presses start 17.9 s after the city-falls cutscene opens (its log line, the
#    clock fast_combat.steps uses); when that line never comes, they start anyway, and a second, looser
#    round of the same inputs follows, so a tutorial prompt is answered even when the first round was early.
#  - The counters are written every 40 s (PS3_PGO_DUMP_AT, patches/port/0007) and the newest complete write
#    is set aside at once, so a run that is cut short (a crash, the player closing the window) still counts.
#  - Every run that reached the game itself is merged, however far it got. One rule stays: a run that never
#    left the title screen is NOT merged. In such a profile all gameplay code has zero counts, and clang
#    treats zero-count code as cold (optimised for size, not inlined): the game would be slower than the
#    normal build. "Reached the game" = past the title and 150 s on (the montage is 96 s), or the city-falls
#    cutscene seen (new game); the game's own world frame and 30 s on (save).
# The player's files are only read: each run plays on its own copy of the saves (work\pgo-setup\runs\<run>\
# gamedata) and of the settings file; the sound is off (PS3_AUDIO_VOLUME=0), the player's keyboard, mouse
# and controllers do nothing in it (PS3_KBM=0, PS3_PAD_SDL=0), and the window title says what is going on.
# The shader cache is the one the game uses for play (warmed, not a second one).
# Lines for Setup: "  progress: <n>% training", "  player-note: <text>", and on failure "  why: <reason>".
# -StopAfter <s>: a test; the game window is closed after <s> seconds, as a player closing it would.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Profdata,
    [Parameter(Mandatory = $true)][string]$LlvmProfdata,
    [string]$Runs = "opening,save",
    [int]$StopAfter = 0
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "port_paths.ps1")   # Get-DevFlash
function Why([string]$Reason) { "  why: $Reason"; exit 1 }
if (-not (Test-Path -LiteralPath $Exe)) { Why "no training build ($Exe)" }
if (-not (Test-Path -LiteralPath $LlvmProfdata)) { Why "no llvm-profdata.exe ($LlvmProfdata)" }
$elf = Join-Path $root "work\EBOOT.elf"
if (-not (Test-Path -LiteralPath $elf)) { Why "no work\EBOOT.elf" }
$work = Split-Path -Parent $Profdata
$runsDir = Join-Path $work "runs"
New-Item -ItemType Directory -Force $runsDir | Out-Null

# rd /s removes a junction itself, never what it points to
function Remove-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { cmd /c rd /s /q "$Path" 2>&1 | Out-Null } finally { $ErrorActionPreference = $eap }
}
# a counters file llvm-profdata can read (not empty, not cut off by a kill mid-write)
function Test-Profraw([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -lt 1MB) { return $false }
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $LlvmProfdata show $Path 2>&1 | Out-Null; $ok = ($LASTEXITCODE -eq 0) } catch { $ok = $false } finally { $ErrorActionPreference = $eap }
    $ok
}

# ---------------------------------------------------------------- the runs
# Pad scripts: PAD_SCRIPT events "w<s>:<mask>[:<hold s>]", seconds after PAD_SCRIPT_WAIT (anchor.flag) exists.
# Masks: START 0x0008, CROSS 0x4000, L1 0x0400, R1 0x0800; left stick 0x10000 up, right stick 0x100000 up,
# 0x400000 left, 0x800000 right. The opening's first 21 events are tools\fast_combat.steps' (keep in step):
# look up, jump, look up, walk, look left/right, look up, aim + six bolts, the dock walk, aim + five bolts.
# Then one more round of the same kinds of input, 30 s, for a prompt the first round met too early.
$openingPad = "w2:0x100000:3,w8:0x4000,w14:0x100000:3,w20:0x10000:4,w24:0x400000:3,w29:0x800000:3,w34:0x100000:2," +
              "w40:0x400:42,w42:0x800,w45:0x800,w48:0x800,w51:0x800,w54:0x800,w57:0x800,w84:0x10000:30," +
              "w116:0x400:24,w119:0x800,w123:0x800,w127:0x800,w131:0x800,w135:0x800," +
              "w142:0x100000:2,w145:0x400000:2,w148:0x10000:8,w157:0x400:12,w159:0x800,w162:0x800,w165:0x800,w168:0x4000"
# The save: from the game's first world frame 40 s or more after the first present (the in-engine cutscene
# that opens a save is a world frame too; the X presses end it): look, walk, jump, aim + fire, walk.
$savePad = "w3:0x800000:2,w6:0x10000:8,w15:0x400000:3,w19:0x10000:10,w30:0x4000,w31:0x10000:6,w38:0x800000:4," +
           "w43:0x10000:12,w56:0x400:22,w58:0x800,w61:0x800,w64:0x800,w67:0x800,w70:0x800,w73:0x800,w76:0x800," +
           "w80:0x400000:4,w85:0x10000:12,w98:0x4000"

$game = Join-Path $root "game"
$want = @($Runs -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$installed = @(Get-ChildItem -LiteralPath (Join-Path $game "dev_hdd0\game") -Directory -Filter "*_INSTALL" -ErrorAction SilentlyContinue).Count -gt 0
# the player's saves (read only): a story save is a save folder with a STATE file
$gamedata = Join-Path $game "gamedata"
$story = @(Get-ChildItem -LiteralPath (Join-Path $gamedata "dev_hdd0\home\00000001\savedata") -Directory -ErrorAction SilentlyContinue |
           Where-Object { $st = Join-Path $_.FullName "STATE"; (Test-Path -LiteralPath $st) -and (Get-Item -LiteralPath $st).Length -gt 0 })
$plan = @()
if ($want -contains "opening") { $plan += @{ Tag = "opening"; Est = $(if ($installed) { 360 } else { 660 }); Cap = 1200 } }
if ($want -contains "save") {
    if ($story.Count) { $plan += @{ Tag = "save"; Est = 170; Cap = 480 } }
    else { "  no story save yet: training on a new game only" }
}
if (-not $plan.Count) { Why "nothing to train on (-Runs $Runs)" }
$estAll = 0; foreach ($r in $plan) { $estAll += $r.Est }
$min = [int][math]::Ceiling($estAll / 60.0)
"  player-note: The game now opens by itself for about $min minutes$(if ($plan.Count -gt 1) { ' (twice)' }) to learn how your PC runs it. The sound is off. Please don't touch it or close it."
if (-not $installed) { "  the game's HDD data is not installed yet: the first run installs it (a few minutes more)" }

# The settings the player plays with (a copy per run; what a run writes stays in it): the caller's
# PS3_OVERLAY_INI, a portable folder's settings.ini, else %APPDATA%'s (as tools\play.ps1 picks it).
$playerIni = if ($env:PS3_OVERLAY_INI) { $env:PS3_OVERLAY_INI }
             elseif (Test-Path -LiteralPath (Join-Path $root "portable.txt")) { Join-Path $root "settings.ini" }
             else { Join-Path $env:APPDATA "inFAMOUS2-port\settings.ini" }

# stderr/stdout of the game: to a file, each line with the ms since the start, and the lines into a queue the
# loop below reads (the game's stderr is unbuffered, so each line is seen as it is written)
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Text; using System.Threading; using System.Diagnostics; using System.Collections.Concurrent;
public class PgoTee {
    readonly StreamWriter w; readonly Stopwatch sw; readonly ManualResetEvent done = new ManualResetEvent(false);
    public readonly ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
    public PgoTee(string path, Stopwatch sw) { w = new StreamWriter(path, false, new UTF8Encoding(false)); w.AutoFlush = true; this.sw = sw; }
    public void Pump(StreamReader r, bool queue) {
        Thread t = new Thread(() => { try { string l; while ((l = r.ReadLine()) != null) { long ms = sw.ElapsedMilliseconds;
            lock (w) { w.Write(ms); w.Write('\t'); w.WriteLine(l); } if (queue) Lines.Enqueue(ms + "\t" + l); } } catch (Exception) {} done.Set(); });
        t.IsBackground = true; t.Start();
    }
    public void Close() { done.WaitOne(10000); lock (w) { w.Close(); } }
}
public static class PgoWin {
    [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@

$shaderCache = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "ps3recomp\shader_cache\inFAMOUS_2" } else { "" }
$good = @(); $notes = @(); $done = 0.0; $lastPct = -1
$progSw = [Diagnostics.Stopwatch]::StartNew()
foreach ($r in $plan) {
    $tag = $r.Tag; $isSave = $tag -eq "save"
    $rd = Join-Path $runsDir $tag
    Remove-Tree $rd
    $gd = Join-Path $rd "gamedata"
    $sv = Join-Path $gd "dev_hdd0\home\00000001\savedata"
    New-Item -ItemType Directory -Force $sv | Out-Null
    if ($isSave) {   # the player's saves, copied (the game loads the newest by itself at boot)
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        try { robocopy $gamedata $gd /E /NFL /NDL /NJH /NJS /NP 2>&1 | Out-Null } finally { $ErrorActionPreference = $eap }
        if ($LASTEXITCODE -ge 8) { $notes += "save: could not copy the saves"; "  run save: not used (could not copy the saves)"; $done += $r.Est; continue }
    }
    $ini = Join-Path $rd "settings.ini"
    if (Test-Path -LiteralPath $playerIni) { Copy-Item -LiteralPath $playerIni -Destination $ini -Force }
    $raw = Join-Path $rd "counters.profraw"; $last = Join-Path $rd "counters-last.profraw"
    $anchor = Join-Path $rd "anchor.flag"; $world = Join-Path $rd "world.flag"; $pad = Join-Path $rd "pad.txt"
    $dump = @(for ($t = 40; $t -lt $r.Cap; $t += 40) { $t }) -join ','
    $vars = [ordered]@{
        # the game as tools\play.ps1 starts it (keep in step)
        PS3_VFS_ROOT = Join-Path $game "disc"; PS3_HDD0_ROOT = Join-Path $game "dev_hdd0"; PS3_DEV_FLASH = (Get-DevFlash)
        PS3_GAMEDATA_ROOT = $gd
        GCM_SEMA_ACQUIRE = "1"; GCM_SEMA_ACQUIRE_MS = "200"; GCM_PARK_GRACE_MS = "1000"
        PS3RECOMP_VK_WINDOW = "1"; PS3RECOMP_VK_GUEST_PROGRAMS = "1"
        PS3RECOMP_ENG_GPU_LIGHT = "07000B80:07384B80"; PS3RECOMP_ENG_GPU_SSAO = "0F800000"
        PS3_VERBOSE = "0"
        PS3_OVERLAY_DEFAULTS = ("PS3RECOMP_FPS_CAP=60;PS3_AUDIO_VOLUME=100;PS3RECOMP_RENDER_SCALE=1.5;PS3RECOMP_SOFT_SHADOWS=2;PS3RECOMP_SSAO=2;" +
                                "PS3RECOMP_SSAO_STRENGTH=125;PS3RECOMP_LIGHT_DITHER=1;PS3RECOMP_FX_RES=full;PS3RECOMP_SHARPEN=0.1" +
                                $(if (Test-Path -LiteralPath (Join-Path $root "work\texpacks\fire-smoke-hd")) { ";PS3_MODS=fire-smoke-hd" } else { "" }))
        PS3_OVERLAY_INI = $ini                    # the copy (spots.ini and mods.ini beside it, as in a test run)
        PS3RECOMP_VK_SHADER_CACHE = $shaderCache  # the cache play uses (its default follows the window title, changed below)
        # the training's own
        PS3_TITLE = "inFAMOUS 2 - Setup is optimising the game for this PC: it plays by itself for a few minutes, sound off. Please don't close this window."
        PS3_LANG = "en"; PS3_VOICE_LANG = "same"; PS3_TEXT_LANG = "same"   # the same movies and timings on every copy
        PS3_AUDIO_VOLUME = "0"; PS3RECOMP_VK_FULLSCREEN = "0"; PS3RECOMP_FPS_CAP = "0"   # muted, a window, uncapped
        PS3_KBM = "0"; PS3_PAD_SDL = "0"          # the player's keyboard, mouse and controllers do nothing in it
        LLVM_PROFILE_FILE = $raw; PS3_PGO_DUMP_AT = $dump
        PAD_FILE = $pad; PAD_SCRIPT = $(if ($isSave) { $savePad } else { $openingPad })
        PAD_SCRIPT_WAIT = $(if ($isSave) { $world } else { $anchor })
        PS3RECOMP_ENG_WORLD_MARK = $world         # the game writes it on its first world frame
    }
    if ($isSave) { $vars.PS3RECOMP_ENG_WORLD_MARK_AFTER = "40" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$elf`""
    $psi.WorkingDirectory = $rd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    foreach ($k in $vars.Keys) { if ($vars[$k] -ne "") { $psi.EnvironmentVariables[$k] = [string]$vars[$k] } else { $psi.EnvironmentVariables.Remove($k) } }
    "  run ${tag}: started $(Get-Date -Format HH:mm:ss)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $teeE = New-Object PgoTee (Join-Path $rd "stderr.log"), $sw; $teeE.Pump($p.StandardError, $true)
    $teeO = New-Object PgoTee (Join-Path $rd "stdout.log"), $sw; $teeO.Pump($p.StandardOutput, $true)

    $ev = @{}                      # first time (ms) of each log event
    $writes = 0; $closedBy = ""; $endAt = -1.0; $anchorAt = -1.0; $startAt = -1.0; $nextStart = 0.0; $startUntil = -1.0
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 250
        $now = $sw.Elapsed.TotalMilliseconds
        $ln = $null
        foreach ($q in $teeE.Lines, $teeO.Lines) {
            while ($q.TryDequeue([ref]$ln)) {
                $tab = $ln.IndexOf("`t"); $ms = [double]$ln.Substring(0, $tab); $text = $ln.Substring($tab + 1)
                if ($text -match '\[pgo\] profile written at \d+ s \(rc=0\)') {
                    # a complete write: set it aside at once (the next one may be cut off by the end of the run)
                    try { Move-Item -LiteralPath $raw -Destination $last -Force; $writes++ } catch {}
                }
                elseif (-not $ev.menu -and $text -match 'load_menu_catalog') { $ev.menu = $ms }
                elseif (-not $ev.montage -and $text -match 'intro_montage') { $ev.montage = $ms }
                elseif (-not $ev.story -and $text -match 'h_jbi_cityfalls') { $ev.story = $ms }
                elseif (-not $ev.world -and $text -match '\[rsx engine\] world mark') { $ev.world = $ms }
            }
        }
        if ($isSave) {
            if ($ev.world -and $endAt -lt 0) { $endAt = $ev.world + 110000 }               # 105 s of moves, then close
            if (-not $ev.world -and $now -gt 300000) { $closedBy = "no world frame in 300 s"; $endAt = $now }
        } else {
            # the title: START every 1.5 s from the menu (or from 600 s: a first start installing the HDD data
            # may say nothing for minutes) until the montage starts; never once it did (START would pause the game)
            $passed = $ev.montage -or $ev.story
            if ($startAt -lt 0 -and ($ev.menu -or $now -gt 600000)) { $startAt = $now; $startUntil = $now + 300000 }
            if ($startAt -ge 0 -and -not $passed -and $now -lt $startUntil -and $now -ge $nextStart) {
                try { [IO.File]::WriteAllText($pad, "0x0008 40") } catch {}
                $nextStart = $now + 1500
            }
            if ($startAt -ge 0 -and -not $passed -and $now -ge $startUntil -and $endAt -lt 0) { $closedBy = "the title was not passed"; $endAt = $now }
            # the scripted presses: 17.9 s after the city-falls cutscene opened; without it, 300 s after the montage
            if ($anchorAt -lt 0) {
                if ($ev.story) { $anchorAt = $ev.story + 17900 }
                elseif ($ev.montage -and $now -gt $ev.montage + 300000) { $anchorAt = $now }
            }
            if ($anchorAt -ge 0 -and $now -ge $anchorAt -and -not (Test-Path -LiteralPath $anchor)) {
                [IO.File]::WriteAllText($anchor, "$([int]$now)"); $endAt = $anchorAt + 175000   # the last press is at 168 s
            }
        }
        if ($StopAfter -gt 0 -and $now -ge $StopAfter * 1000 -and -not $closedBy) { $closedBy = "test: closed after $StopAfter s"; $endAt = $now }
        $pct = [int][math]::Min(99, 100 * ($done + [math]::Min($sw.Elapsed.TotalSeconds, $r.Est)) / $estAll)
        if ($pct -ne $lastPct) { "  progress: $pct% training"; $lastPct = $pct }
        if (($endAt -ge 0 -and $now -ge $endAt) -or $now -ge $r.Cap * 1000) {
            if ($now -ge $r.Cap * 1000 -and -not $closedBy) { $closedBy = "still going after $($r.Cap) s" }
            # a clean exit (WM_CLOSE) runs the profile writer once more at exit; kill only as the last resort
            $p.Refresh()
            if ($p.MainWindowHandle -ne [IntPtr]::Zero) { [void][PgoWin]::PostMessage($p.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
            if (-not $p.WaitForExit(20000)) {
                $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
                try { taskkill /T /F /PID $p.Id 2>&1 | Out-Null } finally { $ErrorActionPreference = $eap }
            }
            if (-not $closedBy) { $closedBy = "done" }
            break
        }
    }
    $p.WaitForExit(30000) | Out-Null
    $end = $sw.Elapsed.TotalMilliseconds
    $teeE.Close(); $teeO.Close()
    if (-not $closedBy) { $closedBy = "the game closed by itself after $([int]($end / 1000)) s (exit code $($p.ExitCode))" }
    $done += $r.Est

    # did it reach the game itself? (see the top: a title-only profile would slow the game down)
    $why = ""
    if ($isSave) {
        if (-not $ev.world) { $why = "the save did not load into the world" }
        elseif ($end - $ev.world -lt 30000) { $why = "it played only $([int](($end - $ev.world) / 1000)) s in the world" }
    } else {
        if (-not $ev.montage -and -not $ev.story) { $why = "it did not get past the title screen" }
        elseif (-not $ev.story -and $end - $ev.montage -lt 150000) { $why = "it stopped $([int](($end - $ev.montage) / 1000)) s after the title, before the game's story" }
    }
    $pick = $null
    if (-not $why) {
        foreach ($f in @($raw, $last)) { if (Test-Profraw $f) { $pick = $f; break } }
        if (-not $pick) { $why = "no readable counters ($writes complete write(s) seen)" }
    }
    $seen = @(foreach ($k in "menu", "montage", "story", "world") { if ($ev[$k]) { "$k $([int]($ev[$k] / 1000)) s" } }) -join ", "
    "  run ${tag}: $closedBy at $([int]($end / 1000)) s ($(if ($seen) { $seen } else { 'no game events' }))"
    if ($pick) {
        $good += $pick
        "  run ${tag}: used, counters $([math]::Round((Get-Item -LiteralPath $pick).Length / 1MB)) MB$(if ($pick -eq $last) { ' (the last complete write)' })"
    } else {
        if ($closedBy -like "the game closed by itself*" -and $why) { $why += " (the game window was closed or the game stopped)" }
        $notes += $why
        "  run ${tag}: not used ($why)"
    }
    Remove-Tree $gd   # the save copy
}
"  progress: 100% training"
"  training took $([int]$progSw.Elapsed.TotalSeconds) s"

if (-not $good.Count) { Why $(if ($notes.Count) { $notes[0] } else { "no training run finished" }) }
Remove-Item -LiteralPath $Profdata -ErrorAction SilentlyContinue
$eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
try { & $LlvmProfdata merge "--failure-mode=all" -o $Profdata @good 2>&1 | ForEach-Object { "    $_" }; $mrc = $LASTEXITCODE }
finally { $ErrorActionPreference = $eap }
foreach ($r in $plan) { Get-ChildItem -LiteralPath (Join-Path $runsDir $r.Tag) -Filter "*.profraw" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue }
if ($mrc -or -not (Test-Profraw $Profdata)) {
    Remove-Item -LiteralPath $Profdata -ErrorAction SilentlyContinue
    Why "merging the counters failed (llvm-profdata exit $mrc)"
}
"  profile: $Profdata ($([math]::Round((Get-Item -LiteralPath $Profdata).Length / 1MB)) MB, from $($good.Count) run(s))"
exit 0
