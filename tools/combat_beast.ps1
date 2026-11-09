# Run into the prologue BEAST FIGHT (2026-11-08, render worker): tools\combat.ps1's boot ->
# tutorial (look left / right / up, hold L1 + fire R1), then "Head towards the disturbance".
#
#   tools\combat_beast.ps1 -Tag X [-Exe <exe>] [-Extra "K=V;K2=V2"] [-Shots "430,440"]
#   -Scout -PadFile <file> [-RdocTrigger <file>]   (the way that reached the fight, twice)
#   -RdocAt "440,450"  RenderDoc frame captures at those seconds; -RdocTrigger <file>: one capture
#                      each time <file> appears. Needs RenderDoc installed (winget
#                      BaldurKarlsson.RenderDoc): its Vulkan layer is switched on with
#                      ENABLE_VULKAN_RENDERDOC_CAPTURE=1, the backend triggers the captures
#                      (PS3RECOMP_VK_RDOC_*) -> native\recordings\<Tag>\rdoc_frameN.rdc, read
#                      headless with tools\rdoc_inspect.py. Under RenderDoc the game runs ~30-80 fps.
#
# THE ROUTE (as driven through PAD_FILE in rd-scout1 and rd-v2; ~4 min after the tutorial when
# driven well): after "Head towards the disturbance" (~320 s) the camera looks into the sky --
# tilt it down. Turn until the EMPIRE CITY arch (lamps, bunting, orange glow) is ahead: from the
# view of the gangway with the "DANGER ramp closed" sign it is ~50 degrees to the right. Run
# there down the middle of the pier: the planters along both railings stop Cole dead, so step
# off a railing (left/right stick) before running on. At the closed gate ("Blast open the gate")
# hold L1 and shoot the chain lock at the centre of the two gate halves -- aiming with L1 held is
# the reliable way to turn: the camera does not swing after Cole. Run through (cutscene: the pier
# collapses, ~20 s), then straight on ~10 s: Cole climbs the post at the end of the pier
# ("Hold L1 to aim"). Hold L1 and fire ("Shoot at the source of disturbance") -> cutscene (the
# Beast rises, Cole hangs on the post, ~50 s) -> "Take down the Beast" / "Kessler told me
# about you!": the fight.
#
# The scripted route below (no -Scout) is a best effort and does NOT reach the fight yet: the
# camera yaw at the end of the tutorial differs from run to run (the tutorial's prompts arrive a
# few seconds apart), so a fixed right-stick turn faces a different way each time (rd-v1: into
# the PIER 13 sign). PAD_FILE driving: write "<mask> <polls>" to the file (masks as PAD_SCRIPT,
# e.g. 0x10000 left stick up, 0x400000 right stick left, 0x400 L1, 0xC00 L1+R1); a new command
# is read only when the previous one's polls are used up; keep L1 held between commands by
# re-writing "0x400 20" whenever the file is empty.
# Run it through tools\with_game_lock.ps1 when other workers may run the game too.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Exe = "",
    [string]$Extra = "",
    [int]$Seconds = 0,
    [string]$Shots = "",
    [string]$SaveFrom = "",         # start from these saves (native_run.ps1 -SaveFrom)
    [switch]$Scout,
    [string]$PadFile = "",
    [string]$RdocAt = "",
    [string]$RdocTrigger = ""      # a file: each time it appears, one RenderDoc capture (PS3RECOMP_VK_RDOC_TRIGGER)
)
# --- the tutorial, as tools\combat.ps1 (25 PAD_SCRIPT events) -----------------------------------
$base = "35:0x0008,42:0x4000,50:0x4000,58:0x4000,80:0x4000,100:0x4000,120:0x4000,140:0x4000,160:0x4000,180:0x4000,200:0x4000,240:0x100000:3,246:0x4000,252:0x100000:3,258:0x10000:4"
$look = "262:0x400000:3,267:0x800000:3,272:0x100000:2"
$fire = (0..5 | ForEach-Object { "{0}:0x800" -f (280 + 3 * $_) }) -join ","
$pad = "$base,$look,278:0x400:42,$fire"
# --- best-effort scripted route to the Beast (cellPad.c now takes up to 64 events) -------------
$route = @(
    "322:0x200000:1.0", "323.2:0x100000:0.45",          # camera: down, then level
    "324:0x800000:0.3",                                 # camera: right, onto the arch (yaw varies!)
    "325:0x50000:1.2", "326.5:0x10000:9",               # up-left off the railing, run to the gate
    "335.6:0x20000:0.6",                                # step back from the gate
    "336.5:0x400:6", "337.5:0xA00000:0.05",             # aim (L1), nudge onto the lock
    "338.2:0x800", "339.2:0x800", "340.2:0x800", "341.2:0x800",
    "343:0x10000:14",                                   # through the gate (cutscene on the way)
    "372:0x10000:12",                                   # on along the pier to the end post
    "385:0x400:5", "386:0x800", "387.2:0x800", "388.4:0x800"  # shoot the source of disturbance
)
$fightEv = (0..7 | ForEach-Object { "{0}:0x800" -f (448 + 2 * $_) }) -join ","
$beast = ($route -join ",") + ",446:0x400:20,$fightEv"
if (-not $Scout) { $pad = "$pad,$beast" }
if (-not $Seconds) { $Seconds = if ($Scout) { 1000 } else { 480 } }

$root = Split-Path -Parent $PSScriptRoot
$rec = Join-Path $root "native\recordings\$Tag"
New-Item -ItemType Directory -Force $rec | Out-Null
$envs = @{ PAD_SCRIPT = $pad }
if ($PadFile) { $envs["PAD_FILE"] = $PadFile }
if ($RdocAt -or $RdocTrigger) {
    $envs["ENABLE_VULKAN_RENDERDOC_CAPTURE"] = "1"
    $envs["PS3RECOMP_VK_RDOC_AT"] = $RdocAt
    if ($RdocTrigger) { $envs["PS3RECOMP_VK_RDOC_TRIGGER"] = $RdocTrigger }
    $envs["PS3RECOMP_VK_RDOC_FILE"] = "$rec\rdoc"
}
foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $envs[$Matches[1]] = $Matches[2] } }
$shotList = if ($Shots) { @($Shots -split ',' | ForEach-Object { [int]$_ }) } else { @(266, 300, 330) }
$runArgs = @{ Tag = $Tag; Seconds = $Seconds; ShotsAt = $shotList; Env = $envs }
if ($Exe) { $runArgs.Exe = $Exe }
if ($SaveFrom) { $runArgs.SaveFrom = $SaveFrom }
$runArgs.FullMovies = $true   # fixed timeline: keep the real movies (native_run.ps1 uses placeholders with -SaveFrom)
& (Join-Path $PSScriptRoot "native_run.ps1") @runArgs
