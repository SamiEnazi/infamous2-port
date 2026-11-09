# Drive a running game by hand through PAD_FILE (closed loop: look, decide, act), as used to find the
# route to the Beast fight (tools\combat_beast.ps1 -Scout -PadFile <file> [-RdocTrigger <file>]).
#   tools\pad_drive.ps1 -PadFile <file> [-Pad "<mask> <polls>"] [-Wait <ms>] [-Shot <png>] [-Rdoc <file>]
#   masks as PAD_SCRIPT (cellPad.c): 0x10000/0x20000/0x40000/0x80000 left stick up/down/left/right,
#   0x100000/0x200000/0x400000/0x800000 right stick, 0x400 L1, 0x800 R1, 0x4000 CROSS; OR them
#   (0x10400 = walk forward while aiming). A command is read only when the previous one's polls
#   (frames) are used up. Prints the game's age in seconds with each action.
param(
    [Parameter(Mandatory = $true)][string]$PadFile,
    [string]$Pad = "",
    [int]$Wait = 0,
    [string]$Shot = "",
    [string]$Rdoc = "",                 # the PS3RECOMP_VK_RDOC_TRIGGER file: request one RenderDoc capture
    [string]$ExeLike = "*Infamous2Recomp*"
)
$p = Get-Process Infamous2Recomp* -ErrorAction SilentlyContinue | Where-Object { $_.Path -like $ExeLike } | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
$age = { "{0:F1}s" -f ((Get-Date) - $p.StartTime).TotalSeconds }
if ($Pad) { [System.IO.File]::WriteAllText($PadFile, $Pad); "t=$(& $age) pad '$Pad'" }
if ($Rdoc) { [System.IO.File]::WriteAllText($Rdoc, "1"); "t=$(& $age) RenderDoc capture requested" }
if ($Wait) { Start-Sleep -Milliseconds $Wait }
if ($Shot) {
    & (Join-Path $PSScriptRoot "window_shot.ps1") -ProcessId $p.Id -Out $Shot | Out-Null
    "t=$(& $age) shot $Shot"
}
