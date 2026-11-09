# Interleaved A/B with early stopping (testspeed agent, 2026-11-08). Runs A B B A | A B B A | ...
# (an ABBA block cancels slow drift: heat, background), every run with the contamination watcher,
# and after each block asks tools\ab_stats.py for a verdict per window; stops as soon as every
# window is decided (B faster / slower / same within -Same %), or after -MaxBlocks.
# Hold the game lock once for the whole set:
#   powershell -File tools\with_game_lock.ps1 -Who me -Cmd "& tools\ab_run.ps1 -Prefix x1 -ExeA <a.exe> -ExeB <b.exe>"
#   ... -ExtraA "" -ExtraB "PS3_X=0"  (same exe, a switch)   -Steps tools\fast_combat.steps (default the city)   -Test combat (old test)
# A contaminated run (other processes > 0.5 cores in a window) is left out by ab_stats and the
# block is not counted as decided on it.
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [string]$ExeA = "", [string]$ExeB = "",
    [string]$ExtraA = "", [string]$ExtraB = "",
    [ValidateSet("fast", "combat")][string]$Test = "fast",
    [string]$Steps = "",             # fast test steps file (default tools\fast_city.steps)
    [int]$MaxBlocks = 3,
    [double]$Same = 2.0,
    [string]$Metrics = "fps,low",
    [string]$Noise = ""              # calibrated per-window noise json (default: tools\ab_noise_<test>.json if present)
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Steps) { $Steps = Join-Path $PSScriptRoot "fast_city.steps" }
$script = if ($Test -eq "fast") { Join-Path $PSScriptRoot "fast.ps1" } else { Join-Path $PSScriptRoot "combat.ps1" }
if (-not $Noise) { $n = Join-Path $PSScriptRoot "ab_noise_$Test.json"; if (Test-Path $n) { $Noise = $n } }
$tagsA = @(); $tagsB = @(); $i = 0
for ($blk = 1; $blk -le $MaxBlocks; $blk++) {
    foreach ($side in "A", "B", "B", "A") {
        $i++
        $tag = "$Prefix-$side$i"
        $a = @{ Tag = $tag; CpuWatch = $true }
        if ($Test -eq "fast") { $a.Steps = $Steps }
        $exe = if ($side -eq "A") { $ExeA } else { $ExeB }; if ($exe) { $a.Exe = $exe }
        $ex = if ($side -eq "A") { $ExtraA } else { $ExtraB }; if ($ex) { $a.Extra = $ex }
        "== run $i ($side) $tag"
        & $script @a | Select-Object -Last 3
        if ($side -eq "A") { $tagsA += $tag } else { $tagsB += $tag }
    }
    $py = @("-I", (Join-Path $PSScriptRoot "ab_stats.py"), ("A=" + ($tagsA -join ",")), ("B=" + ($tagsB -join ",")), "--metrics", $Metrics, "--same", $Same)
    if ($Test -eq "fast") { $w = (Select-String -Path $Steps -Pattern '^#!\s*windows\s+(\S+)').Matches[0].Groups[1].Value; $py += @("--win", $w) }
    if ($Noise) { $py += @("--noise", $Noise) }
    $out = & python @py
    $out | Select-Object -Last 2
    if ($out -contains "DECIDED") { "decided after $blk block(s) ($($tagsA.Count) runs per side)"; break }
}
$out | Set-Content -Encoding utf8 (Join-Path $root "native\recordings\$Prefix-ab.txt")
"full table: native\recordings\$Prefix-ab.txt"
