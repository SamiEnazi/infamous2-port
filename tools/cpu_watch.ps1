# Contamination watcher for a measured game run (testspeed agent, 2026-11-08): samples the CPU
# time of every process OTHER than the game every -Interval seconds, until the game exits, so an
# fps number taken while something else ran (a compile outside the lock: clang-cl, lld-link,
# python, ...) is flagged instead of trusted. The lead's integ7 run measured ~10% low that way.
#   powershell -File tools\cpu_watch.ps1 -GamePid <pid> -Out <rec>\cpuwatch.tsv
# <Out> gets one row per sample: t_s (since the watcher started), other_cores, game_cores and
# the top 3 other processes; <Out>.json the summary (native_run.ps1 -CpuWatch copies it into
# run.json). tools\cpu_contam.py <rec> [t0 t1] summarises any window afterwards.
param(
    [Parameter(Mandatory = $true)][int]$GamePid,
    [Parameter(Mandatory = $true)][string]$Out,
    [double]$Interval = 2.0,
    [double]$Limit = 0.5             # other-process cores (worst 30 s average) above this = contaminated
)
$self = $PID
$skip = @{ 0 = 1; 4 = 1; $self = 1 }   # Idle, System, the watcher itself
function Snap {
    $h = @{}
    foreach ($p in [System.Diagnostics.Process]::GetProcesses()) {
        try { $h[$p.Id] = @($p.ProcessName, $p.TotalProcessorTime.TotalMilliseconds) } catch {}
        $p.Dispose()
    }
    $h
}
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$prev = Snap; $tPrev = $sw.Elapsed.TotalMilliseconds
"t_s`tother_cores`tgame_cores`ttop" | Set-Content -Encoding ascii $Out
$series = New-Object System.Collections.Generic.List[double]; $byName = @{}; $sumOther = 0.0; $sumGame = 0.0; $n = 0; $worst = 0.0
while ($true) {
    Start-Sleep -Milliseconds ([int]($Interval * 1000))
    $cur = Snap; $t = $sw.Elapsed.TotalMilliseconds; $dt = $t - $tPrev
    if ($dt -le 0) { continue }
    $other = 0.0; $game = 0.0; $rows = @{}
    foreach ($id in $cur.Keys) {
        if ($skip.ContainsKey($id)) { continue }
        $c = $cur[$id]; $p0 = if ($prev.ContainsKey($id) -and $prev[$id][0] -eq $c[0]) { $prev[$id][1] } else { $c[1] }
        $d = ($c[1] - $p0) / $dt
        if ($d -le 0) { continue }
        if ($id -eq $GamePid) { $game += $d; continue }
        $other += $d
        $rows[$c[0]] = $(if ($rows.ContainsKey($c[0])) { $rows[$c[0]] } else { 0.0 }) + $d
    }
    $top = ($rows.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 3 |
            ForEach-Object { "{0}={1:N2}" -f $_.Key, $_.Value }) -join ' '
    "{0:N1}`t{1:N3}`t{2:N3}`t{3}" -f ($t / 1000), $other, $game, $top | Add-Content -Encoding ascii $Out
    foreach ($k in $rows.Keys) { $byName[$k] = $(if ($byName.ContainsKey($k)) { $byName[$k] } else { 0.0 }) + $rows[$k] }
    $series.Add($other); $sumOther += $other; $sumGame += $game; $n++; if ($other -gt $worst) { $worst = $other }
    $prev = $cur; $tPrev = $t
    if (-not $cur.ContainsKey($GamePid)) { break }
}
$avg = if ($n) { $sumOther / $n } else { 0 }
# the worst 30 s: a compile during one measured window hides in a whole-run average
$k = [math]::Max(1, [int][math]::Ceiling(30 / $Interval)); $w30 = 0.0
for ($i = 0; $i + $k -le $series.Count; $i++) { $m = 0.0; for ($j = 0; $j -lt $k; $j++) { $m += $series[$i + $j] }; $m /= $k; if ($m -gt $w30) { $w30 = $m } }
if ($series.Count -lt $k) { $w30 = $avg }
$topAll = $byName.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 |
          ForEach-Object { [ordered]@{ name = $_.Key; avg_cores = [math]::Round($_.Value / [math]::Max(1, $n), 3) } }
[ordered]@{
    samples = $n; interval_s = $Interval; limit_cores = $Limit
    other_avg_cores = [math]::Round($avg, 3); other_max_cores = [math]::Round($worst, 3); other_worst30s_cores = [math]::Round($w30, 3)
    game_avg_cores = [math]::Round($(if ($n) { $sumGame / $n } else { 0 }), 3)
    contaminated = ($w30 -gt $Limit); top = @($topAll)
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$Out.json"
