# Run the native Windows build for N seconds and keep its output as a recording
# (the counterpart of oracle_run.ps1).
#   tools\native_run.ps1 -Tag boot01 -Seconds 120 [-Env @{PS3_HLE_TRACE='400'}]
#
# Layout mirrors RPCS3's run of the same copy (oracle/recordings/boot02_install):
#   /dev_bdvd  -> game\disc               (copy of the EU disc BCES01143)
#   /dev_hdd0  -> game\dev_hdd0           (game\BCES01143 = v1.04 update,
#                                          game\BCES01143_INSTALL = the install
#                                          RPCS3's first boot made)
#   /dev_flash -> RPCS3's dev_flash (firmware 4.93)
#   argv[0]    =  /dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN (the toolkit default, = RPCS3)
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$Seconds = 120,
    [hashtable]$Env = @{},
    [int[]]$ShotsAt = @(),                 # seconds after start to screenshot the window
    [string]$ShotMethod = "print",          # print (PrintWindow) | screen (desktop region)
    [string]$Probe = "",                    # file of guestmem.py command lines, run at every ShotsAt time -> probe.txt
    [int[]]$CpuAt = @(),                    # ShotsAt times at which to measure per-thread CPU over the next 5 s -> cpu_<t>s.txt
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "work\port\build\Infamous2Recomp.exe"),
    [switch]$LiveLog,                      # stderr also written live, one '<ms since start><TAB><line>' row per line, to stderr_live.tsv (default off)
    [string]$Driver = "",                    # steps file for tools\fast_driver.ps1 (closed-loop input, run alongside); it ends the run early via WM_CLOSE or <rec>\stop.flag (implies -LiveLog)
    [switch]$FullMovies,                   # keep the real cutscene movies; without it a run with -SaveFrom or -Driver uses the 4 s placeholder views (work\fastmovies)
    [switch]$CpuWatch,                     # sample other processes' CPU (tools\cpu_watch.ps1) -> run.json cpu_watch, 'CONTAMINATED' line (default off)
    [string]$SaveFrom = ""                  # saves to start from: a gamedata folder (work\saves\<name>\gamedata, a recording's gamedata) or a savedata folder; copied into this run's own
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "port_paths.ps1")   # Get-PortPath rpcs3 (config.local.json)
$game = Join-Path $root "game"
$out = Join-Path $root "native\recordings\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
# Saves (and trophies) of a test live in the recording, <rec>\gamedata, never in Sami's
# game\gamedata (play.ps1): every run starts with none (the same main menu, no "Continue"),
# or with a copy of -SaveFrom.
$gamedata = Join-Path $out "gamedata"
if (Test-Path $gamedata) { Remove-Item -Recurse -Force $gamedata }
$savedata = "$gamedata\dev_hdd0\home\00000001\savedata"
New-Item -ItemType Directory -Force $savedata | Out-Null
if ($SaveFrom) {
    if (-not (Test-Path $SaveFrom)) { throw "-SaveFrom: no such folder $SaveFrom" }
    if (Test-Path "$SaveFrom\dev_hdd0") { Copy-Item "$SaveFrom\*" $gamedata -Recurse -Force }
    else { Copy-Item "$SaveFrom\*" $savedata -Recurse -Force }
    "saves from ${SaveFrom}: $((Get-ChildItem $savedata -Directory).Name -join ', ')"
}
Remove-Item "$out\settings.ini" -ErrorAction SilentlyContinue   # a re-used tag starts from the defaults

$vars = @{
    PS3_VFS_ROOT          = "$game\disc"
    PS3_HDD0_ROOT         = "$game\dev_hdd0"
    PS3_DEV_FLASH         = (Join-Path (Get-PortPath rpcs3) "dev_flash")
    PS3_GAMEDATA_ROOT     = $gamedata
    PS3_TITLE             = "inFAMOUS 2"
    PS3_LANG              = "en"          # the EU disc ships the English dub
    PS3_AUDIO_VOLUME      = "15"          # test runs quietly audible (Sami, 2026-11-07: "add a little sound so I can hear it"); -Env @{PS3_AUDIO_VOLUME='0'} = silent
    PS3_PAD_SDL           = "0"           # SDL reads pads whatever window has focus: keep a real controller out of scripted runs
    PS3_VERBOSE           = "0"           # the per-wait [WAIT]/[evt] lines (~500/s, one FILE lock): -Env @{PS3_VERBOSE='1'} to capture them
    # run.sh's timing knobs (Mac port defaults)
    GCM_SEMA_ACQUIRE      = "1"
    GCM_SEMA_ACQUIRE_MS   = "200"
    GCM_PARK_GRACE_MS     = "1000"
    # deferred lighting and SSAO on the GPU (Vulkan passes, toolkit patch 0010),
    # as the Mac port runs them: the SPU versions held the 3D title at ~10 fps,
    # these at ~43. -Env @{PS3RECOMP_ENG_GPU_LIGHT=''; PS3RECOMP_ENG_GPU_SSAO=''}
    # puts both jobs back on the SPUs (A/B).
    PS3RECOMP_ENG_GPU_LIGHT = "07000B80:07384B80"
    PS3RECOMP_ENG_GPU_SSAO  = "0F800000"
    # the Vulkan window with the game's own (guest) shader programs, as play.ps1 runs it
    PS3RECOMP_VK_WINDOW         = "1"
    PS3RECOMP_VK_GUEST_PROGRAMS = "1"
    # the in-game overlay (F1 HUD / F2 menu): a test never reads or writes Sami's
    # %APPDATA% settings.ini, and the HUD stays off so screenshots compare cleanly
    PS3_OVERLAY_INI             = "$out\settings.ini"
    PS3_OVERLAY_HUD             = "0"
}
# Fast movies (Sami 2026-11-08: "cut down all videos"): runs from a save (-SaveFrom) or closed-loop
# runs (-Driver) see every pre-rendered cutscene movie as the disc's own 4 s placeholder, through
# junction/hard-link views of game\disc and game\dev_hdd0 (tools\make_fastmovies.ps1; nothing copied
# or changed). Fixed-timeline runs (combat.ps1, suite.ps1) keep the real movies: their presses are
# timed for them. -FullMovies forces the real ones; -Env PS3_VFS_ROOT/PS3_HDD0_ROOT still win.
$movies = "full"
if (($SaveFrom -or $Driver) -and -not $FullMovies) {
    $fm = Join-Path $root "work\fastmovies"
    if (-not (Test-Path "$fm\disc\PS3_GAME\USRDIR\movies\cutscene") -or -not (Test-Path "$fm\dev_hdd0\game")) {
        & powershell -NoProfile -File (Join-Path $PSScriptRoot "make_fastmovies.ps1") | Out-Host
    }
    $vars.PS3_VFS_ROOT = "$fm\disc"; $vars.PS3_HDD0_ROOT = "$fm\dev_hdd0"; $movies = "fast"
}
foreach ($k in $Env.Keys) { $vars[$k] = [string]$Env[$k] }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$root\work\EBOOT.elf`""
$psi.WorkingDirectory = $out            # gamedata-relative writes land in the recording
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }

if ($Driver) { $LiveLog = $true }
if ($LiveLog) {
    # stderr is unbuffered in the game: each line is seen as it is written (movie opens, cutscene
    # subtitles, thread starts), with the time since start -> closed-loop drivers and timelines
    if (-not ('PortLineTee2' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.IO; using System.Text; using System.Threading; using System.Diagnostics;
public class PortLineTee2 {
    readonly StreamWriter w; readonly StringBuilder sb = new StringBuilder(); readonly Stopwatch sw; readonly double base0; readonly ManualResetEvent done = new ManualResetEvent(false);
    public PortLineTee2(string path, Stopwatch sw, double base0) { w = new StreamWriter(path, false, new UTF8Encoding(false)); w.AutoFlush = true; this.sw = sw; this.base0 = base0; }
    public void Pump(StreamReader r) {
        var t = new Thread(() => { try { string l; while ((l = r.ReadLine()) != null) lock (sb) { sb.AppendLine(l); w.Write((long)(base0 + sw.Elapsed.TotalMilliseconds)); w.Write('\t'); w.WriteLine(l); } } catch {} done.Set(); });
        t.IsBackground = true; t.Start();
    }
    public string Text() { done.WaitOne(10000); lock (sb) { w.Close(); return sb.ToString(); } }
}
"@ }
}
$t0 = Get-Date
$p = [System.Diagnostics.Process]::Start($psi)
# -LiveLog rows: ms since the process creation time (the clock fast_driver.ps1 uses too)
$sw = [System.Diagnostics.Stopwatch]::StartNew(); $base0 = 0.0; if ($LiveLog) { try { $base0 = ((Get-Date) - $p.StartTime).TotalMilliseconds } catch {} }
$cw = $null
if ($CpuWatch) { $cw = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', "$PSScriptRoot\cpu_watch.ps1", '-GamePid', $p.Id, '-Out', "$out\cpuwatch.tsv" }
$so = $p.StandardOutput.ReadToEndAsync()
$tee = $null
if ($LiveLog) {
    $tee = New-Object PortLineTee2 "$out\stderr_live.tsv", $sw, $base0
    $tee.Pump($p.StandardError)
    $se = $null
} else { $se = $p.StandardError.ReadToEndAsync() }
Remove-Item "$out\stop.flag" -ErrorAction SilentlyContinue
$drv = $null
if ($Driver) { $drv = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', "$PSScriptRoot\fast_driver.ps1", '-Steps', $Driver, '-Rec', $out, '-GamePid', $p.Id, '-LiveLog', "$out\stderr_live.tsv" }
$exited = $false
foreach ($ts in ($ShotsAt | Sort-Object)) {
    $wait = [int](($t0.AddSeconds($ts) - (Get-Date)).TotalMilliseconds)
    if ($wait -gt 0 -and $p.WaitForExit($wait)) { $exited = $true; break }
    try { & "$PSScriptRoot\window_shot.ps1" -ProcessId $p.Id -Out "$out\shot_${ts}s.png" -Method $ShotMethod | Out-Null } catch { "shot at ${ts}s failed: $_" }
    # live guest-memory probes (read-only) at the same moments
    if ($Probe) {
        foreach ($ln in (Get-Content $Probe | Where-Object { $_ -and -not $_.StartsWith("#") })) {
            $r = & python -I "$PSScriptRoot\guestmem.py" @($ln -split '\s+') 2>&1 | Out-String
            Add-Content -Encoding utf8 "$out\probe.txt" "== t=${ts}s  guestmem $ln`n$r"
        }
    }
    # per-thread CPU over the next 5 s (host thread ids; map them to guest names
    # with the "[THREAD n] host thread started ... hosttid=" lines in stderr)
    if ($CpuAt -contains $ts) {
        try {
            $p.Refresh(); $a = @{}; foreach ($th in $p.Threads) { $a[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds }
            $w0 = Get-Date; Start-Sleep -Seconds 5; $p.Refresh(); $wall = ((Get-Date) - $w0).TotalMilliseconds
            $rows = foreach ($th in $p.Threads) {
                $d = $th.TotalProcessorTime.TotalMilliseconds - $(if ($a.ContainsKey($th.Id)) { $a[$th.Id] } else { 0 })
                [pscustomobject]@{ hosttid = $th.Id; cpu_pct = [math]::Round(100 * $d / $wall, 1) } }
            $rows | Sort-Object cpu_pct -Descending | Format-Table -AutoSize | Out-String |
                Set-Content -Encoding utf8 "$out\cpu_${ts}s.txt"
        } catch { "cpu sample at ${ts}s failed: $_" }
    }
    # the Vulkan backend's own frame dump (latest presented frame), if enabled
    $dump = $vars["PS3RECOMP_VK_DUMP"]
    try { if ($dump -and (Test-Path $dump)) {
        # the backend rewrites this file on every present: retry until a complete frame is copied
        $dst = "$out\vkframe_${ts}s.ppm"
        for ($k = 0; $k -lt 20; $k++) {
            Copy-Item $dump $dst -Force
            if ((Get-Item $dst).Length -ge 2764800) { break }
            Start-Sleep -Milliseconds 15
        } } } catch { "frame copy at ${ts}s failed: $_" }
}
if (-not $exited) {
    if ($Driver) {   # until the driver's stop.flag, -Seconds, or the game exits
        while (-not $exited -and (Get-Date) -lt $t0.AddSeconds($Seconds) -and -not (Test-Path "$out\stop.flag")) { $exited = $p.WaitForExit(250) }
    } else {
        $left = [int](($t0.AddSeconds($Seconds) - (Get-Date)).TotalMilliseconds)
        $exited = $p.WaitForExit([Math]::Max(0, $left))
    }
}
if (-not $exited) { $p.Kill(); $p.WaitForExit(5000) | Out-Null }
[System.IO.File]::WriteAllText("$out\stdout.log", $so.Result)
[System.IO.File]::WriteAllText("$out\stderr.log", $(if ($tee) { $tee.Text() } else { $se.Result }))
if ($drv) { $drv.WaitForExit(10000) | Out-Null; if (-not $drv.HasExited) { $drv.Kill() } }
$cwSum = $null
if ($cw) {
    $cw.WaitForExit(15000) | Out-Null
    try { $cwSum = Get-Content "$out\cpuwatch.tsv.json" -Raw | ConvertFrom-Json } catch { "cpu watch: no summary ($_)" }
    if ($cwSum) {
        "cpu watch: other processes avg {0} cores, worst 30 s {1} (limit {2}), top {3}{4}" -f $cwSum.other_avg_cores, $cwSum.other_worst30s_cores,
            $cwSum.limit_cores, (($cwSum.top | ForEach-Object { "$($_.name)=$($_.avg_cores)" }) -join ' '),
            $(if ($cwSum.contaminated) { "  -> CONTAMINATED: do not trust this run's fps (per-window: tools\ab_stats.py)" } else { "" })
    }
}
@{
    cpu_watch = $cwSum
    movies = $movies
    contaminated = $(if ($cwSum) { [bool]$cwSum.contaminated } else { $null })
    tag = $Tag; started = $t0.ToString("s"); seconds = $Seconds; exe = $Exe
    exited_by_itself = $exited; exit_code = $(if ($exited) { $p.ExitCode } else { $null })
    env = $vars
} | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 "$out\run.json"
"recording: $out  exited_by_itself=$exited exit=$(if ($exited) { '0x{0:X}' -f $p.ExitCode } else { '-' })  stderr=$((Get-Item "$out\stderr.log").Length) B"
