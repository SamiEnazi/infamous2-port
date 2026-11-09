# Run inFAMOUS 2 (EU disc + v1.04 update) in RPCS3 for N seconds and keep the log
# as an oracle recording.
#   tools\oracle_run.ps1 -Tag boot01 -Seconds 240
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$Seconds = 240,
    [string]$Rpcs3 = "",                   # default: the RPCS3 folder of config.local.json (Setup.cmd)
    [string]$Game = (Join-Path (Split-Path -Parent $PSScriptRoot) "game\disc\PS3_GAME\USRDIR\EBOOT.BIN"),
    [int[]]$ShotsAt = @(),                 # seconds after start to screenshot the game window
    [string]$ShotMethod = "print",
    [string]$InputConfig = "",             # e.g. "oracle": config\input_configs\global\oracle.yml (XInput pad 1,
                                           # fed by tools\vpad_script.py); empty = RPCS3's own setting
    [string]$PadScript = "",               # PAD_SCRIPT syntax, played on a virtual X360 pad (implies -InputConfig oracle)
    [string]$Probe = ""                    # file of guestmem.py command lines (read from RPCS3), run at every ShotsAt time
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Rpcs3) { . (Join-Path $PSScriptRoot "port_paths.ps1"); $Rpcs3 = Get-PortPath rpcs3 }
$out = Join-Path $root "oracle\recordings\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
if (Get-Process rpcs3 -ErrorAction SilentlyContinue) { throw "RPCS3 is already running; close it first" }

$t0 = Get-Date
$vp = $null
if ($PadScript) {
    if (-not $InputConfig) { $InputConfig = "oracle" }
    # the virtual pad first, so it is XInput pad 1 when RPCS3 enumerates
    $vp = Start-Process -FilePath "$root\work\venv\Scripts\python.exe" -WindowStyle Hidden -PassThru `
        -ArgumentList "-I `"$root\tools\vpad_script.py`" `"$PadScript`" --seconds $($Seconds + 5)" `
        -RedirectStandardOutput "$out\vpad.log"
    Start-Sleep -Seconds 2
}
$rargs = "--no-gui"
if ($InputConfig) { $rargs += " --input-config $InputConfig" }
$p = Start-Process -FilePath "$Rpcs3\rpcs3.exe" -ArgumentList "$rargs `"$Game`"" -PassThru
foreach ($ts in ($ShotsAt | Sort-Object)) {
    $wait = [int](($t0.AddSeconds($ts) - (Get-Date)).TotalMilliseconds)
    if ($wait -gt 0 -and $p.WaitForExit($wait)) { break }
    try { & "$PSScriptRoot\window_shot.ps1" -ProcessId $p.Id -Out "$out\shot_${ts}s.png" -Method $ShotMethod | Out-Null } catch { "shot at ${ts}s failed: $_" }
    if ($Probe) {
        foreach ($ln in (Get-Content $Probe | Where-Object { $_ -and -not $_.StartsWith("#") })) {
            $ln2 = $ln -replace '\{t\}', "$ts"
            $res = & python -I "$PSScriptRoot\guestmem.py" --rpcs3 @($ln2 -split '\s+') 2>&1 | Out-String
            Add-Content -Encoding utf8 "$out\probe.txt" "== t=${ts}s  guestmem --rpcs3 $ln2`n$res"
        }
    }
}
$left = [int](($t0.AddSeconds($Seconds) - (Get-Date)).TotalMilliseconds)
if (-not $p.HasExited) { $p.WaitForExit([Math]::Max(0, $left)) | Out-Null }
$exited = $p.HasExited
if (-not $exited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

if ($vp -and -not $vp.HasExited) { Stop-Process -Id $vp.Id -Force -ErrorAction SilentlyContinue }
Copy-Item "$Rpcs3\log\RPCS3.log" "$out\RPCS3.log" -Force
@{
    tag = $Tag; started = $t0.ToString("s"); seconds = $Seconds
    exited_by_itself = $exited; exit_code = $(if ($exited) { $p.ExitCode } else { $null })
    game = $Game
} | ConvertTo-Json | Set-Content -Encoding utf8 "$out\run.json"
"recording: $out  (log $((Get-Item "$out\RPCS3.log").Length) bytes, exited_by_itself=$exited)"
