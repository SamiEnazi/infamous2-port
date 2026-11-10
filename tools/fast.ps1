# Fast closed-loop test run (testspeed agent, 2026-11-08). Instead of a fixed-time pad script that
# waits ~250 s with big safety margins, tools\fast_driver.ps1 reacts to the game (its live stderr)
# and presses as soon as each screen is ready; the measured part then runs from an ANCHOR (a game
# event) with the in-process pad script's "w<sec>" clock, so every run measures the same moment.
#   tools\fast.ps1 -Tag X [-Exe <exe>] [-Extra "K=V;K2=V2"] [-Steps tools\fast_combat.steps]
# Prints fps per window (anchor-relative, tools\ab_stats.py), the contamination check, and the
# picture check against work\goldens\<steps name> (tools\shotcheck.py) when it exists.
# A steps file carries its own settings in '#!' lines:
#   #! pad      w0:0x100000:3,w5:0x4000         in-process PAD_SCRIPT, times from the anchor
#   #! windows  0:8,60:90                        measured windows, seconds from the anchor
#   #! seconds  400                              hard cap for the whole run
#   #! replay   work\routes\<name>.pad          replay a recorded route from the anchor (PS3_PAD_REPLAY; needs the recorder in the exe)
#   #! savefrom work\saves\first-save\gamedata   boot from these saves (unless -SaveFrom is given)
#   #! fastmovies 0                              keep the real cutscene movies (default: the 4 s placeholder, native_run.ps1)
#   #! worldmark 0                               the GAME creates anchor.flag on its first world frame
#                                                (> 2500 draws, PS3RECOMP_ENG_WORLD_MARK); <s> > 0 = not before
#                                                <s> s after the first present; use the 'anchorfile' step
# Run it through tools\with_game_lock.ps1 like any game run.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Exe = "",
    [string]$Extra = "",
    [string]$Steps = "",
    [string]$SaveFrom = "",
    [switch]$CpuWatch = $true,       # always on here (the parameter exists so ab_run.ps1 can pass it)
    [switch]$NoPictureCheck
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Steps) { $Steps = Join-Path $PSScriptRoot "fast_combat.steps" }
$Steps = (Resolve-Path $Steps).Path
$rec = Join-Path $root "native\recordings\$Tag"
$hdr = @{}
foreach ($l in (Get-Content $Steps)) { if ($l -match '^#!\s*(\w+)\s+(.*)$') { $hdr[$Matches[1]] = $Matches[2].Trim() } }
if (-not $SaveFrom -and $hdr.savefrom) { $SaveFrom = Join-Path $root $hdr.savefrom }
$secs = if ($hdr.seconds) { [int]$hdr.seconds } else { 400 }
$envs = @{
    PAD_FILE = "$rec\pad.txt"; PAD_SCRIPT_WAIT = "$rec\anchor.flag"
    PS3_FTRACE_KINDS = '5,7,10,12,16,17'; PS3_FTRACE = "5:$($secs + 60):$rec\ftrace.txt"
    # (rsx5, 2026-11-10) shots and waitimg from the game's own image (window_shot.ps1 -ReqFile): they work
    # with the display asleep; a build without it never makes shot.req.ready -> PrintWindow as before
    PS3RECOMP_SHOT_REQ = "$rec\shot.req"
}
if ($hdr.pad) { $envs.PAD_SCRIPT = $hdr.pad }
if ($hdr.replay) { $envs.PS3_PAD_REPLAY = $(if ([IO.Path]::IsPathRooted($hdr.replay)) { $hdr.replay } else { Join-Path $root $hdr.replay }) }   # a recorded route (play.ps1 -RecordRoute), anchor-relative
$fullMovies = ($hdr.fastmovies -eq '0')   # fast movies are native_run's default for -Driver runs
if ($hdr.ContainsKey('worldmark')) { $envs.PS3RECOMP_ENG_WORLD_MARK = "$rec\anchor.flag"; if ([int]$hdr.worldmark -gt 0) { $envs.PS3RECOMP_ENG_WORLD_MARK_AFTER = $hdr.worldmark } }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $envs[$Matches[1]] = $Matches[2] } }
Remove-Item "$rec\anchor.flag", "$rec\pad.txt", "$rec\ftrace.txt", "$rec\shot_*.png", "$rec\shot.req*" -ErrorAction SilentlyContinue
$runArgs = @{ Tag = $Tag; Seconds = $secs; Env = $envs; Driver = $Steps; CpuWatch = $true; FullMovies = $fullMovies }
if ($Exe) { $runArgs.Exe = $Exe }
if ($SaveFrom) { $runArgs.SaveFrom = $SaveFrom }
$t = Measure-Command { & (Join-Path $PSScriptRoot "native_run.ps1") @runArgs | Write-Host }

# anchor in ftrace time: the ftrace clock starts at its first hook, which prints "[ftrace] recording"
$anchorMs = $null; $ftMs = $null
$d = Import-Csv -Delimiter "`t" "$rec\driver.tsv" -ErrorAction SilentlyContinue
$a = $d | Where-Object { $_.step -eq "anchor" } | Select-Object -First 1
if ($a) { $anchorMs = [double]$a.info }
$fl = Select-String -Path "$rec\stderr_live.tsv" -Pattern '\[ftrace\] recording' -List -ErrorAction SilentlyContinue
if ($fl) { $ftMs = [double](($fl.Line -split "`t")[0]) }
$j = Get-Content "$rec\run.json" -Raw | ConvertFrom-Json
$j | Add-Member -Force anchor_ms $anchorMs
$j | Add-Member -Force anchor_s $(if ($null -ne $anchorMs -and $null -ne $ftMs) { [math]::Round(($anchorMs - $ftMs) / 1000, 3) } else { $null })
$j | Add-Member -Force wall_s ([math]::Round($t.TotalSeconds, 1))
$j | Add-Member -Force steps $Steps
$j | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$rec\run.json"   # anchor_s must be on disk before ab_stats.py reads it (the summary rows used offset 0 from 15:58 to 18:00)
$fails = @($d | Where-Object { $_.step -in "TIMEOUT", "error" -or ($_.step -eq "stop" -and $_.info -notlike "close:*") })
# Validity (2026-11-08, wake's wko-* runs measured a frozen prompt): a run is INVALID when the driver timed out
# or failed, there is no anchor or no trace, the anchor is outside the steps file's '#! anchor_range a:b'
# (an early false match), or the picture check flags at least half the shots. ab_stats.py skips invalid runs.
$why = @()
if ($fails) { $why += "driver: " + (($fails | ForEach-Object { "$($_.step) $($_.info)" }) -join '; ') }
if ($null -eq $anchorMs) { $why += "no anchor" }
if (-not (Test-Path "$rec\ftrace.txt")) { $why += "no ftrace.txt (no clean exit)" }
if ($hdr.anchor_range -and $null -ne $anchorMs) {
    $lo, $hi = [double[]]($hdr.anchor_range -split ':')
    if ($anchorMs / 1000 -lt $lo -or $anchorMs / 1000 -gt $hi) { $why += ("anchor at {0:N1} s is outside {1}-{2} s (a false screen match?)" -f ($anchorMs / 1000), $lo, $hi) }
}
"fast run ${Tag}: wall $([math]::Round($t.TotalSeconds))s, anchor at $([math]::Round($anchorMs / 1000, 1))s"
if (-not $why -and $null -ne $j.anchor_s -and $hdr.windows) {
    & python -I (Join-Path $PSScriptRoot "ab_stats.py") "R=$Tag" --win $hdr.windows --metrics fps,low --all | Where-Object { $_ -match '^\s+R ' }
}
if (-not $NoPictureCheck -and -not $why) {
    $gold = Join-Path $root ("work\goldens\" + [IO.Path]::GetFileNameWithoutExtension($Steps))
    if (Test-Path $gold) {
        & python (Join-Path $PSScriptRoot "shotcheck.py") $rec --ref $gold | Select-Object -Last 8
        try { $sc = Get-Content "$rec\shotcheck.json" -Raw | ConvertFrom-Json
              $n = @($sc.shots.PSObject.Properties).Count
              $cal = @((Get-Content "$gold\noise.json" -Raw | ConvertFrom-Json).calibrated_with).Count   # only a calibrated reference may invalidate a run
              if ($cal -ge 2 -and $n -and 2 * @($sc.flagged).Count -ge $n) { $why += "picture check flagged $(@($sc.flagged).Count) of $n shots (wrong or frozen scene)" } } catch {}
    }
}
$j | Add-Member -Force valid (-not $why)
$j | Add-Member -Force invalid_reason ($why -join ' | ')
$j | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$rec\run.json"
if ($why) { "INVALID RUN ${Tag}: " + ($why -join ' | ') + "  -- not counted by ab_stats.py" }
