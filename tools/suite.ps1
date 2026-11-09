# The perf + picture suite (PLAN.md "Work plan: speed WITH precision", Phase 0).
#
# One scripted run of the native build (the same pad script every time: title -> new game ->
# intro comic -> boat cutscene -> Empire City skyline -> gameplay tutorial), with a frame-time
# trace and screenshots at fixed times, then tools\suite_report.py:
#   - one perf row per scene (avg fps, p50/p99, frames over 16.7/33 ms, hitches),
#   - every screenshot compared with the golden set (native\goldens\<name>), flagged when it
#     differs more than two good runs of the same build differ from each other,
#   - Vulkan errors from the log.
#
#   tools\suite.ps1 -Tag s01                       # run + report against native\goldens\current
#   tools\suite.ps1 -Tag s01 -Extra "K=V;K2=V2"    # A/B: environment switches for this run
#   tools\suite.ps1 -Tag g1 -MakeGolden current    # this run's shots become the golden set
#   tools\suite.ps1 -Tag g2 -Calibrate current     # a second good run: per-shot noise levels
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Exe = "",
    [string]$Extra = "",
    [string]$Golden = "current",
    [string]$MakeGolden = "",
    [string]$Calibrate = "",
    [int]$Seconds = 272
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$rec = Join-Path $root "native\recordings\$Tag"
$pad = "35:0x0008,42:0x4000,50:0x4000,58:0x4000,80:0x4000,100:0x4000,120:0x4000,140:0x4000,160:0x4000,180:0x4000,200:0x4000,240:0x100000:3,246:0x4000,252:0x100000:3,258:0x10000:4"
# presents, GPU waits, compiles, texture uploads, RSX semaphore/drain only. The trace is written
# when an event arrives after its window: the window must end before the run does (265 < 272 s).
$envs = @{ PAD_SCRIPT = $pad; PS3_FTRACE_KINDS = '5,7,10,12,16,17'; PS3_FTRACE = "20:245:$rec\ftrace.txt" }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $envs[$Matches[1]] = $Matches[2] } }
# Dense shots around the key moments: builds of different speed reach a scene at slightly
# different times, so each golden key frame is matched against the closest-looking run frame
# within +-6 s (suite_report.py); a real rendering fault shows in every nearby frame.
$shots = @(24..36 | Where-Object { $_ % 2 -eq 0 }) + @(54..66 | Where-Object { $_ % 2 -eq 0 }) +
         @(104..116 | Where-Object { $_ % 2 -eq 0 }) + @(149..205) + @(230..266 | Where-Object { $_ % 2 -eq 0 })
$runArgs = @{ Tag = $Tag; Seconds = $Seconds; ShotsAt = $shots; Env = $envs }
if ($Exe) { $runArgs.Exe = $Exe }
& (Join-Path $PSScriptRoot "native_run.ps1") @runArgs

$py = @((Join-Path $PSScriptRoot "suite_report.py"), $rec)
if ($MakeGolden) { $py += @("--make-golden", $MakeGolden) }
elseif ($Calibrate) { $py += @("--calibrate", $Calibrate) }
else { $py += @("--golden", $Golden) }
if ($Extra) { $py += @("--extra", $Extra) }
& python @py
