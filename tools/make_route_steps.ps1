# Replay steps for a recorded route (testspeed, 2026-11-08):
#   powershell -File tools\make_route_steps.ps1 -Route beast1 -From opening [-Window 30]
# -> tools\fast_route_<route>.steps: the boot of fast_city / fast_combat, '#! replay work\routes\<route>.pad',
# long windows of -Window seconds over the whole route (a fight drifts: judge it by fps and ms/frame over long
# windows, not by exact scenes), per-thread CPU in each window (threads_w<n>.tsv), a shot at the end.
param([Parameter(Mandatory = $true)][string]$Route, [ValidateSet("city", "opening")][string]$From = "city", [int]$Window = 0)
$root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $root "work\routes\$Route.pad"
if (-not (Test-Path $pad)) { throw "no route $pad (record it: play.ps1 -RecordRoute $Route -From $From)" }
$last = ((Get-Content $pad | Where-Object { $_ -match '^\d' } | Select-Object -Last 1) -split ' ')[0]
$L = [int][math]::Floor([double]$last / 1000)
if (-not $Window) { $Window = $(if ($From -eq "opening") { 30 } else { 10 }) }
$wins = @(for ($w = 2; $w + $Window -le $L; $w += $Window) { "{0}:{1}" -f $w, ($w + $Window) })
$o = @("# Replay of work\routes\$Route.pad ($L s, from the $From); made by tools\make_route_steps.ps1")
if ($From -eq "opening") {
    $o += "#! seconds $($L + 200)", "#! anchor_range 60:130", "#! replay work\routes\$Route.pad", "#! windows $($wins -join ',')"
    $o += "wait load_menu_catalog 90", "spam 0x0008 1.5 intro_montage 60", "wait h_jbi_cityfalls 200", "anchorafter 17.9"
} else {
    $o += "#! seconds $($L + 160)", "#! anchor_range 55:100", "#! savefrom work\saves\first-save\gamedata", "#! replay work\routes\$Route.pad", "#! windows $($wins -join ',')"
    $o += "wait v_02_city_intro 120", "sleep 2", "waitimg city_hud 40,20,340,130 15 120", "sleep 2", "anchor"
}
$i = 0
foreach ($w in $wins) { $a, $b = $w -split ':'; $i++; $o += "at $a", "threads w$i $($Window - 1)" }
$o += "at $($L + 1)", "shot end", "close"
$f = Join-Path $PSScriptRoot "fast_route_$Route.steps"
[IO.File]::WriteAllText($f, (($o -join "`n") + "`n"))
"wrote $f : $L s, windows $($wins -join ',')"
