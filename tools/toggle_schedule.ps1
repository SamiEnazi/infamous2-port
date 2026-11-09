# Rewrite a PS3_TOGGLE_FILE on a schedule, so one run tries several knob
# settings at known moments (A/B inside one boot instead of across boots).
#   tools\toggle_schedule.ps1 -File <toggle file> -StepsFile <steps.txt>
# steps.txt: one step per line, "<sec>|<toggle file text>" (seconds from the
# scheduler's start; ';' separates the toggle file's lines; empty text = no
# overrides). Start it in the background right before native_run.
param(
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$StepsFile
)
$t0 = Get-Date
Set-Content -Encoding ascii -Path $File -Value ""
foreach ($s in (Get-Content $StepsFile | Where-Object { $_ -and -not $_.StartsWith("#") })) {
    $i = $s.IndexOf("|")
    $at = [double]$s.Substring(0, $i)
    $text = $s.Substring($i + 1) -replace ';', "`n"
    $wait = ($t0.AddSeconds($at) - (Get-Date)).TotalMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
    Set-Content -Encoding ascii -Path $File -Value $text
    "{0,6:F1}s  {1}" -f ((Get-Date) - $t0).TotalSeconds, ($text -replace "`n", " | ")
}
