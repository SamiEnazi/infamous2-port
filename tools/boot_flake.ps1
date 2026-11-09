# Boot the native build N times and report how often it reaches the frame loop.
#   tools\boot_flake.ps1 -Runs 4 -Seconds 40 [-Env @{...}]
# Per run: flips (bucketed count of _cellGcmSetFlipCommand), the last NEW
# milestone (where a stalled run stopped making progress), stderr size.
param([int]$Runs = 4, [int]$Seconds = 40, [hashtable]$Env = @{}, [string]$Prefix = "flake")
$root = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format "MMdd_HHmm"
$rows = @()
for ($i = 1; $i -le $Runs; $i++) {
    $tag = "${Prefix}_${stamp}_$i"
    $ms = Join-Path $root "native\recordings\$tag\milestones.txt"
    $e = @{} + $Env; $e["PS3_MILESTONE_OUT"] = $ms
    & "$PSScriptRoot\native_run.ps1" -Tag $tag -Seconds $Seconds -Env $e | Out-Null
    $lines = Get-Content $ms -ErrorAction SilentlyContinue
    $order = $lines | Where-Object { $_ -match '^\d+\t' }
    $flip = ($lines | Where-Object { $_ -match "^count`thle:_cellGcmSetFlipCommand`t" } | Select-Object -Last 1) -replace '^.*\t', ''
    $rows += [pscustomobject]@{
        run = $i; flips = $(if ($flip) { $flip } else { "0" })
        milestones = @($order).Count
        last_new = $(if ($order) { ($order | Select-Object -Last 1) -replace '^\d+\t', '' } else { "-" })
        stderr_kb = [int]((Get-Item (Join-Path $root "native\recordings\$tag\stderr.log")).Length / 1KB)
    }
}
$rows | Format-Table -AutoSize | Out-String -Width 160
$ok = @($rows | Where-Object { $_.flips -ne "0" -and $_.flips -ne "1" }).Count
"reached the frame loop: $ok / $Runs"
