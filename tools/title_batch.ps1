# Run the native build N times with the same scripted input and report, per run,
# whether it got past the loading emblem to the 3D title/menu.
#   tools\title_batch.ps1 -Tag fix1 -Runs 4 [-Seconds 55] [-Env @{...}]
# Success = the draw engine created more than the 3 boot surfaces (the 3D menu
# allocates its render targets); stuck = only the boot surfaces. A run that ends
# with almost no stderr is the separate early hang (bug #2) and is reported apart.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$Runs = 4,
    [int]$Seconds = 55,
    [hashtable]$Env = @{}
)
$root = Split-Path -Parent $PSScriptRoot
# PS3_VERBOSE=1: the stderr-size test below (an early hang stops logging) needs the
# per-wait lines native_run now turns off by default.
$base = @{ PS3RECOMP_VK_WINDOW = '1'; PS3RECOMP_VK_GUEST_PROGRAMS = '1'; PS3_VERBOSE = '1'
           PAD_SCRIPT = '35:0x0008,42:0x4000,50:0x4000,58:0x4000' }
foreach ($k in $Env.Keys) { $base[$k] = [string]$Env[$k] }
$ok = 0; $stuck = 0; $early = 0
for ($i = 1; $i -le $Runs; $i++) {
    $t = "${Tag}_$i"
    & "$PSScriptRoot\native_run.ps1" -Tag $t -Seconds $Seconds -Env $base | Out-Null
    $err = Join-Path $root "native\recordings\$t\stderr.log"
    $surf = (Select-String -Path $err -Pattern "rsx engine\] surface" -SimpleMatch:$false | Measure-Object).Count
    $size = (Get-Item $err).Length
    if ($surf -gt 3) { $verdict = "TITLE"; $ok++ }          # the 3D title's surfaces exist
    elseif ($size -lt 200000) { $verdict = "EARLY-HANG"; $early++ }
    else { $verdict = "STUCK"; $stuck++ }
    "{0,-14} surfaces={1,-3} stderr={2,9} B  {3}" -f $t, $surf, $size, $verdict
}
"== $Tag : title $ok / stuck $stuck / early-hang $early  (of $Runs)"
