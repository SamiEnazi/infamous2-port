# Closed-loop test driver (testspeed agent, 2026-11-08): plays a steps file against a running
# game, reacting to its live stderr (native_run.ps1 -LiveLog -> <rec>\stderr_live.tsv) instead of
# fixed times, presses buttons through PAD_FILE (<rec>\pad.txt), and starts the in-process pad
# script's "w<sec>" clock (PAD_SCRIPT_WAIT = <rec>\anchor.flag) at a detected moment, so the
# measured part of a run starts at the same game state every time. Started by
# native_run.ps1 -Driver <steps file>; everything it does goes to <rec>\driver.tsv (ms since the
# process start, the clock of stderr_live.tsv).
#
# Steps (one per line, '#' comments):
#   wait  <regex> [timeout_s]                     next stderr line (after the last match) matching
#   spam  <mask> <every_s> <regex> [timeout_s]    press <mask> every <every_s> until a line matches
#   press <mask> [polls]                          one press (PAD_FILE; polls = frames held, default 40)
#   sleep <s>
#   anchor                                        t=0 of the measured part: anchor.flag + run.json anchor_s
#   anchorfile [timeout_s]                        t=0 = when the GAME created anchor.flag (PS3RECOMP_ENG_WORLD_MARK=<rec>\anchor.flag:
#                                                 the first world frame, > 2500 draws; frame-exact, no driver latency)
#   at    <s>                                     sleep until anchor + <s>
#   shot  <name>                                  window screenshot -> shot_<name>.png
#   waitimg <ref> <x,y,w,h> <maxdiff> [timeout_s]  until the window region looks like work\fast_refs\<ref>.png (game imagery: never committed)
#                                                 (mean |diff| of 32x18 thumbnails, 0-255); state from the screen
#   spamimg <mask> <every_s> <ref> <x,y,w,h> <maxdiff> [timeout_s]   press until the screen matches
#   close                                         end the run: WM_CLOSE (the game exits, ftrace atexit dump), then stop.flag
#   say   <text>                                  speak it (and print it)
#   threads <name> <seconds>                      per-thread CPU % over the next <seconds> -> threads_<name>.tsv
#   fail  <text>                                  give up: stop.flag + reason
# A step that times out writes "TIMEOUT" and ends the run (driver.tsv says where).
param(
    [Parameter(Mandatory = $true)][string]$Rec,
    [Parameter(Mandatory = $true)][int]$GamePid,
    [Parameter(Mandatory = $true)][string]$LiveLog,
    [Parameter(Mandatory = $true)][string]$Steps
)
$ErrorActionPreference = "Continue"
$gp = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
if (-not $gp) { exit 1 }
$start = $gp.StartTime
function Now { [int](((Get-Date) - $start).TotalMilliseconds) }
$log = Join-Path $Rec "driver.tsv"
"ms`tstep`tinfo" | Set-Content -Encoding ascii $log
function Note([string]$step, [string]$info) { "{0}`t{1}`t{2}" -f (Now), $step, $info | Add-Content -Encoding ascii $log }
$pad = Join-Path $Rec "pad.txt"
$anchorFlag = Join-Path $Rec "anchor.flag"
$script:anchorMs = -1

# live stderr reader: lines "<ms>\t<text>", read incrementally
$script:lines = New-Object System.Collections.Generic.List[string]
$script:cursor = 0
$script:fs = $null; $script:rd = $null; $script:part = ""
function Pump {
    if (-not $script:rd) {
        if (-not (Test-Path $LiveLog)) { return }
        $script:fs = [System.IO.File]::Open($LiveLog, 'Open', 'Read', 'ReadWrite')
        $script:rd = New-Object System.IO.StreamReader($script:fs)
    }
    $txt = $script:rd.ReadToEnd()
    if (-not $txt) { return }
    $txt = $script:part + $txt
    $parts = $txt -split "`n"
    $script:part = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) { $script:lines.Add($parts[$i].TrimEnd("`r")) }
}
function Alive { -not $gp.HasExited }
function Press([string]$mask, [int]$polls = 40) { [System.IO.File]::WriteAllText($pad, "$mask $polls") }
function WaitLine([string]$re, [double]$timeout) {
    $dl = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $dl -and (Alive)) {
        Pump
        for (; $script:cursor -lt $script:lines.Count; $script:cursor++) {
            $l = $script:lines[$script:cursor]
            if ($l -match $re) { $script:cursor++; return $l }
        }
        Start-Sleep -Milliseconds 40
    }
    return $null
}
function Stop([string]$why) { Note "stop" $why; Set-Content -Path (Join-Path $Rec "stop.flag") -Value $why }
function CloseGame {
    # a clean exit runs the game's atexit handlers (the PS3_FTRACE dump); kill only as a fallback
    $gp.Refresh()
    $h = $gp.MainWindowHandle
    if ($h -ne [IntPtr]::Zero) {
        if (-not ('DrvWin' -as [type])) {
            Add-Type -Namespace '' -Name DrvWin -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'
        }
        [void][DrvWin]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Note "close" "WM_CLOSE posted"
        if ($gp.WaitForExit(15000)) { Note "close" "exited by itself"; return }
    }
    Stop "close: no clean exit in 15 s"
}

# screen state: the window region (1280x720 coordinates) as a 32x18 grey thumbnail vs a reference crop
Add-Type -AssemblyName System.Drawing
function Thumb([System.Drawing.Bitmap]$bm, [int[]]$r) {
    $sx = $bm.Width / 1280.0; $sy = $bm.Height / 720.0
    $src = New-Object System.Drawing.Rectangle ([int]($r[0] * $sx)), ([int]($r[1] * $sy)), ([int]($r[2] * $sx)), ([int]($r[3] * $sy))
    $t = New-Object System.Drawing.Bitmap 32, 18
    $g = [System.Drawing.Graphics]::FromImage($t); $g.InterpolationMode = 'HighQualityBilinear'
    $g.DrawImage($bm, (New-Object System.Drawing.Rectangle 0, 0, 32, 18), $src, 'Pixel'); $g.Dispose()
    $v = New-Object double[] 576
    for ($y = 0; $y -lt 18; $y++) { for ($x = 0; $x -lt 32; $x++) { $c = $t.GetPixel($x, $y); $v[$y * 32 + $x] = 0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B } }
    $t.Dispose(); , $v
}
$script:refCache = @{}
function ScreenDiff([string]$ref, [int[]]$r) {
    if (-not $script:refCache.ContainsKey("$ref$r")) {
        $rb = New-Object System.Drawing.Bitmap (Join-Path (Split-Path -Parent $PSScriptRoot) "work\fast_refs\$ref.png")
        $full = New-Object System.Drawing.Bitmap 1280, 720; $g = [System.Drawing.Graphics]::FromImage($full)
        # a reference is either a full 1280x720 shot or exactly the region
        if ($rb.Width -eq 1280 -and $rb.Height -eq 720) { $g.DrawImage($rb, 0, 0, 1280, 720); $script:refCache["$ref$r"] = Thumb $full $r }
        else { $g.DrawImage($rb, $r[0], $r[1], $r[2], $r[3]); $script:refCache["$ref$r"] = Thumb $full $r }
        $g.Dispose(); $full.Dispose(); $rb.Dispose()
    }
    $tmp = Join-Path $Rec "driver_look.png"
    try { & (Join-Path $PSScriptRoot "window_shot.ps1") -ProcessId $GamePid -Out $tmp | Out-Null } catch { return 999 }
    if (-not (Test-Path $tmp)) { return 999 }
    $bm = New-Object System.Drawing.Bitmap $tmp
    $v = Thumb $bm $r; $bm.Dispose()
    $a = $script:refCache["$ref$r"]; $d = 0.0
    for ($i = 0; $i -lt 576; $i++) { $d += [math]::Abs($v[$i] - $a[$i]) }
    $d / 576
}
function Region([string]$s) { , ([int[]]($s -split ',')) }
$stepsTxt = Get-Content $Steps | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith("#") }
Note "start" "steps=$Steps"
foreach ($raw in $stepsTxt) {
    if (-not (Alive)) { Note "gone" "the game exited"; break }
    $s = $raw.Trim()
    $op, $rest = $s -split '\s+', 2
    switch ($op) {
        "wait" {
            $m = [regex]::Match($rest, '^(.*?)(?:\s+(\d+(?:\.\d+)?))?$'); $re = $m.Groups[1].Value
            $to = if ($m.Groups[2].Success) { [double]$m.Groups[2].Value } else { 300 }
            $l = WaitLine $re $to
            if ($null -eq $l) { Note "TIMEOUT" "wait $re"; Stop "timeout: wait $re"; exit 2 }
            Note "wait" ("matched {0} at game ms {1}" -f $re, ($l -split "`t")[0])
        }
        "spam" {
            $a = $rest -split '\s+', 3
            $mask = $a[0]; $every = [double]$a[1]
            $m = [regex]::Match($a[2], '^(.*?)(?:\s+(\d+(?:\.\d+)?))?$'); $re = $m.Groups[1].Value
            $to = if ($m.Groups[2].Success) { [double]$m.Groups[2].Value } else { 300 }
            $dl = (Get-Date).AddSeconds($to); $n = 0; $hit = $null
            while ((Get-Date) -lt $dl -and (Alive) -and -not $hit) {
                Press $mask; $n++
                $hit = WaitLine $re $every
            }
            if (-not $hit) { Note "TIMEOUT" "spam $mask until $re ($n presses)"; Stop "timeout: spam $re"; exit 2 }
            Note "spam" ("{0} x{1} until {2} (game ms {3})" -f $mask, $n, $re, ($hit -split "`t")[0])
        }
        "press" { $a = $rest -split '\s+'; $pl = if ($a.Count -gt 1) { [int]$a[1] } else { 40 }; Press $a[0] $pl; Note "press" "$($a[0]) $pl" }
        "sleep" { Start-Sleep -Milliseconds ([int]([double]$rest * 1000)); Note "sleep" $rest }
        "anchor" {
            $script:anchorMs = Now
            Set-Content -Path $anchorFlag -Value $script:anchorMs
            Note "anchor" "$script:anchorMs"
        }
        "anchorfile" {
            $to = if ($rest) { [double]$rest } else { 300 }; $dl = (Get-Date).AddSeconds($to)
            while (-not (Test-Path $anchorFlag) -and (Get-Date) -lt $dl -and (Alive)) { Start-Sleep -Milliseconds 40 }
            if (-not (Test-Path $anchorFlag)) { Note "TIMEOUT" "anchorfile"; Stop "timeout: anchorfile"; exit 2 }
            $script:anchorMs = [int](((Get-Item $anchorFlag).CreationTime - $start).TotalMilliseconds)
            Note "anchor" "$script:anchorMs"
        }
        "at" {
            if ($script:anchorMs -lt 0) { Note "error" "at before anchor"; continue }
            $w = $script:anchorMs + [int]([double]$rest * 1000) - (Now)
            while ($w -gt 0 -and (Alive)) { Start-Sleep -Milliseconds ([Math]::Min($w, 250)); $w = $script:anchorMs + [int]([double]$rest * 1000) - (Now) }
        }
        "shot" {
            try { & (Join-Path $PSScriptRoot "window_shot.ps1") -ProcessId $GamePid -Out (Join-Path $Rec "shot_$rest.png") | Out-Null; Note "shot" $rest }
            catch { Note "shot" "failed $rest : $_" }
        }
        "waitimg" {
            $a = $rest -split '\s+'; $r = Region $a[1]; $mx = [double]$a[2]; $to = if ($a.Count -gt 3) { [double]$a[3] } else { 120 }
            $dl = (Get-Date).AddSeconds($to); $d = 999
            while ((Get-Date) -lt $dl -and (Alive)) { $d = ScreenDiff $a[0] $r; if ($d -le $mx) { break }; Start-Sleep -Milliseconds 200 }
            if ($d -gt $mx) { Note "TIMEOUT" ("waitimg {0} (last diff {1:N1})" -f $a[0], $d); Stop "timeout: waitimg $($a[0])"; exit 2 }
            Note "waitimg" ("{0} diff {1:N1}" -f $a[0], $d)
        }
        "spamimg" {
            $a = $rest -split '\s+'; $every = [double]$a[1]; $r = Region $a[3]; $mx = [double]$a[4]; $to = if ($a.Count -gt 5) { [double]$a[5] } else { 120 }
            $dl = (Get-Date).AddSeconds($to); $d = 999; $n = 0
            while ((Get-Date) -lt $dl -and (Alive)) {
                $d = ScreenDiff $a[2] $r; if ($d -le $mx) { break }
                Press $a[0]; $n++
                $w = (Get-Date).AddSeconds($every)
                while ((Get-Date) -lt $w) { Start-Sleep -Milliseconds 200; $d = ScreenDiff $a[2] $r; if ($d -le $mx) { break } }
                if ($d -le $mx) { break }
            }
            if ($d -gt $mx) { Note "TIMEOUT" ("spamimg {0} x{1} (last diff {2:N1})" -f $a[2], $n, $d); Stop "timeout: spamimg $($a[2])"; exit 2 }
            Note "spamimg" ("{0} x{1} until {2} diff {3:N1}" -f $a[0], $n, $a[2], $d)
        }
        "close" { CloseGame; break }
        "threads" {   # per-thread CPU % over <seconds> -> threads_<name>.tsv (guest names from the log); CPU% x frame ms / 100 = ms per frame
            $a = $rest -split '\s+'; $sec = [double]$a[1]
            $gp.Refresh(); $c0 = @{}; foreach ($th in $gp.Threads) { try { $c0[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch {} }
            $w0 = Get-Date; Start-Sleep -Milliseconds ([int]($sec * 1000)); $gp.Refresh(); $wall = ((Get-Date) - $w0).TotalMilliseconds
            Pump; $names = @{}; $gname = @{}
            foreach ($l in $script:lines) {
                if ($l -match 'sys_ppu_thread_create tid=(\d+) name="([^"]*)"') { $gname[$Matches[1]] = $Matches[2] }
                elseif ($l -match '\[THREAD (\d+)\] host thread started.*hosttid=(\d+)') { $names[[int]$Matches[2]] = "ppu$($Matches[1]) " + $gname[$Matches[1]] }
            }
            $rows = foreach ($th in $gp.Threads) { try { $d = $th.TotalProcessorTime.TotalMilliseconds - $(if ($c0.ContainsKey($th.Id)) { $c0[$th.Id] } else { 0 })
                    if ($d -gt 0.02 * $wall) { "{0}`t{1:N1}`t{2}" -f $th.Id, (100 * $d / $wall), $names[$th.Id] } } catch {} }
            (@("hosttid`tcpu_pct`tname") + ($rows | Sort-Object { -[double](($_ -split "`t")[1]) })) | Set-Content -Encoding ascii (Join-Path $Rec "threads_$($a[0]).tsv")
            Note "threads" "$($a[0]) $sec s"
        }
        "say" {   # spoken message (Sami records a route: the game window keeps the focus)
            try { Add-Type -AssemblyName System.Speech; $sp = New-Object System.Speech.Synthesis.SpeechSynthesizer; $sp.Speak($rest); $sp.Dispose() } catch {}
            Write-Host $rest; Note "say" $rest
        }
        "fail" { Stop "fail: $rest"; exit 3 }
        default { Note "error" "unknown step: $s" }
    }
}
Note "end" ""
