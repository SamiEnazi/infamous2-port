# Scripted run into REAL gameplay (2026-11-08): boot -> tutorial (look left / right / up with the
# right stick, hold L1 to aim, fire R1) -> "Head towards the disturbance": a walk on the docks, then
# aim and fire. The old suite (tools\suite.ps1) stopped at the frozen "look left" prompt: its
# 236-262 s "gameplay" window was a paused freeze-frame, not play.
#   tools\combat.ps1 -Tag X [-Exe <exe>] [-Extra "K=V;K2=V2"] [-Shots "320,330"] [-Ft "250:130"]
# Timeline (from boot): ~262-278 tutorial looks, 278-320 aim + fire, 322-352 walk, 354-378 aim/fire.
# Frame trace: PS3_FTRACE window -Ft "<start>:<seconds>" (must end before -Seconds).
# Run it through tools\with_game_lock.ps1 when other workers may run the game too.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Exe = "",
    [string]$Extra = "",
    [int]$Seconds = 384,
    [string]$Ft = "250:130",
    [string]$Shots = "",
    [string]$SaveFrom = "",         # start from these saves (native_run.ps1 -SaveFrom)
    [int[]]$CpuAt = @()             # per-thread CPU over 5 s from these shot times (native_run.ps1 -CpuAt)
)
$base = "35:0x0008,42:0x4000,50:0x4000,58:0x4000,80:0x4000,100:0x4000,120:0x4000,140:0x4000,160:0x4000,180:0x4000,200:0x4000,240:0x100000:3,246:0x4000,252:0x100000:3,258:0x10000:4"
$look = "262:0x400000:3,267:0x800000:3,272:0x100000:2"
$fire = (0..5 | ForEach-Object { "{0}:0x800" -f (280 + 3 * $_) }) -join ","
$walk = "322:0x10000:30,354:0x400:24"
$fire2 = (0..4 | ForEach-Object { "{0}:0x800" -f (357 + 4 * $_) }) -join ","
$pad = "$base,$look,278:0x400:42,$fire,$walk,$fire2"
$root = Split-Path -Parent $PSScriptRoot
$rec = Join-Path $root "native\recordings\$Tag"
$envs = @{ PAD_SCRIPT = $pad; PS3_FTRACE_KINDS = '5,7,10,12,16,17'; PS3_FTRACE = "${Ft}:$rec\ftrace.txt" }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $envs[$Matches[1]] = $Matches[2] } }
$shotList = if ($Shots) { @($Shots -split ',' | ForEach-Object { [int]$_ }) } else { @(266, 300, 330, 345, 360, 375) }
# FullMovies: this fixed timeline is timed for the real movies (native_run.ps1 would use the 4 s
# placeholders with -SaveFrom); tools\fast.ps1 -Steps tools\fast_combat.steps is the closed-loop version.
$runArgs = @{ Tag = $Tag; Seconds = $Seconds; ShotsAt = $shotList; Env = $envs; FullMovies = $true }
if ($Exe) { $runArgs.Exe = $Exe }
if ($SaveFrom) { $runArgs.SaveFrom = $SaveFrom }
if ($CpuWatch) { $runArgs.CpuWatch = $true }
if ($CpuAt) { $runArgs.CpuAt = $CpuAt }
& (Join-Path $PSScriptRoot "native_run.ps1") @runArgs
