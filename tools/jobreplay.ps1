# SPU job equivalence check: replay the job recordings in -Dir through an exe's own lifted SPU code
# and compare every output bit for bit (toolkit runtime/spu/spu_jobrec.c, PS3_JOBREPLAY).
#   tools\jobreplay.ps1 -Dir native\recordings\<tag>\jobrec [-Exe <exe>] [-Threads 2] [-Repeat 1]
#                       [-Mod "223,201"] [-Max 0] [-Extra "SPU_FLOAT_RZ=rn;K=V"] [-Report <file>]
# Exit code 0 = every replay matched. The game does not boot (no window, no lock needed); the exe
# stops in ppu_run after loading the ELF, so the lifted images and the ELF are set up as in a run.
# Recordings come from a game run with -Extra "PS3_JOBREC=<dir>;PS3_JOBREC_MAX=<n>;..." (combat.ps1).
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "work\port\build\Infamous2Recomp.exe"),
    [int]$Threads = 2,
    [int]$Repeat = 1,
    [string]$Mod = "",
    [int]$Max = 0,
    [string]$Extra = "",
    [string]$Report = "",
    [int]$TimeoutMs = 20000
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "port_paths.ps1")   # Get-PortPath rpcs3 (config.local.json)
$game = Join-Path $root "game"
$Dir = (Resolve-Path $Dir).Path
$vars = @{
    PS3_VFS_ROOT            = "$game\disc"
    PS3_HDD0_ROOT           = "$game\dev_hdd0"
    PS3_DEV_FLASH           = (Get-DevFlash)
    PS3_GAMEDATA_ROOT       = "$game\gamedata"
    PS3_TITLE               = "inFAMOUS 2"
    PS3_LANG                = "en"
    PS3_AUDIO_VOLUME        = "0"
    PS3_PAD_SDL             = "0"
    PS3_VERBOSE             = "0"
    # the same SPU function replacements as native_run.ps1 (their two jobs are not recorded)
    PS3RECOMP_ENG_GPU_LIGHT = "07000B80:07384B80"
    PS3RECOMP_ENG_GPU_SSAO  = "0F800000"
    PS3_JOBREPLAY           = $Dir
    PS3_JOBREPLAY_THREADS   = "$Threads"
    PS3_JOBREPLAY_REPEAT    = "$Repeat"
    PS3_JOBREPLAY_TIMEOUT_MS = "$TimeoutMs"
}
if ($Mod) { $vars.PS3_JOBREPLAY_MOD = $Mod }
if ($Max -gt 0) { $vars.PS3_JOBREPLAY_MAX = "$Max" }
if ($Report) { $vars.PS3_JOBREPLAY_REPORT = $(if ([System.IO.Path]::IsPathRooted($Report)) { $Report } else { [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Report)) }) }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] } }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$root\work\EBOOT.elf`""
$psi.WorkingDirectory = $Dir
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
$t0 = Get-Date
$p = [System.Diagnostics.Process]::Start($psi)
$p.PriorityClass = 'BelowNormal'      # never steal from a game run measuring fps
$se = $p.StandardError.ReadToEndAsync()
while (-not $p.StandardOutput.EndOfStream) {
    $ln = $p.StandardOutput.ReadLine()
    if ($ln -match '^\[jobreplay\]|^==|^ovl|^[0-9]+\s|^TOTAL|^first FAIL') { Write-Host $ln }
}
$p.WaitForExit()
$errf = Join-Path $Dir ("replay_stderr_{0:yyyyMMdd_HHmmss}.log" -f $t0)
[System.IO.File]::WriteAllText($errf, $se.Result)
"exit=$($p.ExitCode) after $([int]((Get-Date) - $t0).TotalSeconds) s  (stderr: $errf)"
exit $p.ExitCode
