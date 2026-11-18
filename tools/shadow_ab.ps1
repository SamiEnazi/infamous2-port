# Shadow-map resolution A/B in ONE boot ([shadow], 2026-11-16; tools\light_ab.ps1's scheme): tools\fast.ps1 with
# tools\shadow_city.steps (one still city view) while a scheduler rewrites the PS3_TOGGLE_FILE at anchor-relative
# times: PS3RECOMP_SHADOW_RES and PS3RECOMP_SHADOW_EDGE (the draw engine's eng_fx_toggle_poll) and
# PS3RECOMP_SOFT_SHADOWS (ps3_lightfx). Phase keys: res=1|2|4, soft=0..3, edge=0|1 (default 1),
# trim=0|1 (PS3RECOMP_MESH_TRIM, Edge's triangle culling; default 1 = the game's). -Steps: another steps file.
# Prints fps per phase (fast.ps1 / ab_stats.py windows) and the GPU ms/frame per phase from PS3RECOMP_VK_GPU_PROF's
# 5 s prints that fall inside a phase.
#   powershell -File tools\shadow_ab.ps1 -Tag X -Exe <exe> [-Phases "res=1,soft=0|res=2,soft=0|..."] [-Extra "K=V;..."]
#   [-Tools <folder of fast.ps1>] (default: this script's folder; a worktree gives the main project's tools, whose root has the game, saves and fast refs)
# Run it through tools\with_game_lock.ps1 like any game run.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Exe = "",
    [string]$Phases = "res=1,soft=0|res=2,soft=0|res=4,soft=0|res=2,soft=0,edge=0|res=1,soft=2|res=2,soft=2|res=4,soft=2|res=2,soft=1|res=1,soft=0|res=2,soft=0|res=4,soft=0|res=1,soft=2|res=2,soft=2|res=4,soft=2",
    [string]$Starts = "0,12,24,36,48,60,72,84,102,114,126,138,150,162",
    [string]$Extra = "",
    [string]$Tools = "",
    [string]$Steps = (Join-Path $PSScriptRoot "shadow_city.steps")
)
if (-not $Tools) { $Tools = $PSScriptRoot }
$mainRoot = Split-Path -Parent $Tools
$rec = Join-Path $mainRoot "native\recordings\$Tag"
New-Item -ItemType Directory -Force $rec | Out-Null
$toggles = Join-Path $rec "shadow_toggles.txt"
Set-Content -Encoding ascii -Path $toggles -Value ""
Remove-Item "$rec\anchor.flag" -ErrorAction SilentlyContinue
$ph = @($Phases -split '\|'); $st = @($Starts -split ',' | ForEach-Object { [double]$_ })
if ($ph.Count -ne $st.Count) { throw "-Phases has $($ph.Count) entries, -Starts $($st.Count)" }
$texts = foreach ($p in $ph) {
    $v = @{ res = "1"; soft = "0"; edge = "1"; trim = "1"; bias = "0"; skip = "0" }
    foreach ($kv in ($p -split ',')) { if ($kv -match '^\s*(\w+)\s*=\s*(\d+)\s*$') { $v[$Matches[1]] = $Matches[2] } }
    # skip=1: the character shadow masks' draws dropped (PS3RECOMP_ENG_SKIP_FP, live)
    $skipfp = if ($v.skip -ne "0") { "c70407ba5c029d1c,25f427a67ef667f7" } else { "0" }
    ("PS3RECOMP_SHADOW_RES=$($v.res)`nPS3RECOMP_SOFT_SHADOWS=$($v.soft)`nPS3RECOMP_SHADOW_EDGE=$($v.edge)`nPS3RECOMP_MESH_TRIM=$($v.trim)" +
     "`nPS3RECOMP_SHADOW_BIAS=$($v.bias)`nPS3RECOMP_ENG_SKIP_FP=$skipfp")
}
$sched = Join-Path $rec "shadow_sched.ps1"
$lines = @(
    ('$f = "' + $toggles + '"; $a = "' + "$rec\anchor.flag" + '"; $log = "' + "$rec\shadow_sched.log" + '"'),
    '$dl = (Get-Date).AddMinutes(6); while (-not (Test-Path $a) -and (Get-Date) -lt $dl) { Start-Sleep -Milliseconds 50 }',
    'if (-not (Test-Path $a)) { "no anchor" | Set-Content $log; exit }',
    '$t0 = Get-Date'
)
for ($k = 0; $k -lt $texts.Count; $k++) {
    $body = ($texts[$k] -replace "`n", '`n')
    $lines += ('$w = ($t0.AddSeconds({0}) - (Get-Date)).TotalMilliseconds; if ($w -gt 0) {{ Start-Sleep -Milliseconds ([int]$w) }}' -f $st[$k])
    $lines += ('Set-Content -Encoding ascii -Path $f -Value "' + $body + '"; Add-Content $log ("{0:N1}s phase ' + $k + ': ' + ($texts[$k] -replace "`n", ' ') + '" -f ((Get-Date) - $t0).TotalSeconds)')
}
Set-Content -Encoding ascii -Path $sched -Value ($lines -join "`r`n")
$sp = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', $sched
$ex = "PS3_TOGGLE_FILE=$toggles;PS3RECOMP_OVERLAY_MENU_AT=300:lighting;PS3RECOMP_VK_GPU_PROF=1"
if ($Extra) { $ex += ";$Extra" }
$fa = @{ Tag = $Tag; Steps = $Steps; Extra = $ex; NoPictureCheck = $true }
if ($Exe) { $fa.Exe = $Exe }
& (Join-Path $Tools "fast.ps1") @fa
if (-not $sp.HasExited) { $sp.Kill() }
# GPU ms/frame per phase: the 5 s prints whose interval lies inside a phase (after 1.5 s of settling)
$d = Import-Csv -Delimiter "`t" "$rec\driver.tsv" -ErrorAction SilentlyContinue
$a = $d | Where-Object { $_.step -eq "anchor" } | Select-Object -First 1
$gpu = @{}
if ($a -and (Test-Path "$rec\stderr_live.tsv")) {
    $am = [double]$a.info
    foreach ($m in (Select-String -Path "$rec\stderr_live.tsv" -Pattern '\[gpu-prof\] \d+ frames, \d+ batches: GPU busy ([\d.]+) ms/frame')) {
        $t = ([double](($m.Line -split "`t")[0]) - $am) / 1000.0
        for ($k = 0; $k -lt $st.Count; $k++) {
            if ($t - 5.0 -ge $st[$k] + 1.5 -and $t -le $st[$k] + 12.0) {
                if (-not $gpu.ContainsKey($k)) { $gpu[$k] = @() }
                $gpu[$k] += [double]$m.Matches[0].Groups[1].Value
            }
        }
    }
}
"phases (GPU busy ms/frame from the 5 s prints inside each phase):"
for ($k = 0; $k -lt $ph.Count; $k++) {
    $g = if ($gpu.ContainsKey($k)) { "{0:N2} ms ({1} prints)" -f (($gpu[$k] | Measure-Object -Average).Average), $gpu[$k].Count } else { "-" }
    "  p{0,-2} ({1,5}-{2,5} s) {3,-18} GPU {4}" -f $k, $st[$k], ($st[$k] + 12), $ph[$k], $g
}
Select-String -Path "$rec\stderr_live.tsv" -Pattern '\[shadow\]' -ErrorAction SilentlyContinue | Select-Object -First 30 | ForEach-Object { ($_.Line -split "`t", 2)[1] }
