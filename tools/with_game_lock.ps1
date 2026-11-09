# Run one game-launching command while holding the machine-wide game lock, so parallel workers
# (agents) never run two game instances at once: two would fight over the CPU and GPU, and every
# fps number measured meanwhile would be wrong.
#   powershell -File tools\with_game_lock.ps1 -Who <name> -Cmd "<powershell command line>"
# e.g. -Cmd "& tools\suite.ps1 -Tag ov1 -Exe work\port\build\Infamous2Recomp.exe"
# The lock is an exclusively opened file (work\game.lock): released by the OS even if the holder
# is killed. Waits, polling every 5 s, and prints who holds it.
param(
    [Parameter(Mandatory = $true)][string]$Cmd,
    [string]$Who = "anonymous",
    [int]$TimeoutMin = 120,
    [switch]$Priority          # the lead's short jobs for Sami's build: queue right behind Sami, before the agents
)
$root = Split-Path -Parent $PSScriptRoot
$lock = Join-Path $root "work\game.lock"
$info = Join-Path $root "work\game.lock.owner"
$deadline = (Get-Date).AddMinutes($TimeoutMin)
$queued = Get-Date; $got = $null
# First come, first served (2026-11-08: with four workers a waiter could lose the race for an
# hour): every waiter drops a ticket (<time>_<pid>.t) in work\game.lock.queue and only the oldest
# live ticket may take the lock. Tickets of dead processes are swept. play.ps1 (Sami) drops a
# ticket named 00000000000000000_<pid>.t, which sorts first; -Priority uses 1<time>_<pid>.t (after Sami, before every agent, first come first served among themselves).
$qdir = Join-Path $root "work\game.lock.queue"
New-Item -ItemType Directory -Force $qdir | Out-Null
$ticket = Join-Path $qdir ($(if ($Priority) { "1{0:yyyyMMddHHmmssfff}_{1}.t" } else { "{0:yyyyMMddHHmmssfff}_{1}.t" }) -f (Get-Date), $PID)
Set-Content -Path $ticket -Value $Who
$fs = $null
try {
    while (-not $fs) {
        $ahead = 0; $first = $false
        foreach ($t in (Get-ChildItem $qdir -Filter *.t -ErrorAction SilentlyContinue | Sort-Object Name)) {
            if ($t.FullName -eq $ticket) { $first = ($ahead -eq 0); break }
            $tpid = 0; [void][int]::TryParse(($t.BaseName -split '_')[-1], [ref]$tpid)
            if (-not (Get-Process -Id $tpid -ErrorAction SilentlyContinue)) {
                Remove-Item $t.FullName -ErrorAction SilentlyContinue; continue
            }
            $ahead++
        }
        if ($first) {
            try { $fs = [System.IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None') } catch {}
        }
        if (-not $fs) {
            if ((Get-Date) -gt $deadline) { throw "game lock: gave up after $TimeoutMin min" }
            $o = if (Test-Path $info) { Get-Content $info -ErrorAction SilentlyContinue } else { "?" }
            Write-Host "game lock busy ($o) -- waiting, $ahead ahead in the queue"
            Start-Sleep -Seconds 5
        }
    }
    Set-Content -Path $info -Value ("{0} since {1:HH:mm:ss}" -f $Who, (Get-Date))
    $got = Get-Date
    Invoke-Expression $Cmd
} finally {
    if ($fs) { $fs.Close() }
    # history for the test-time accounting (tools\lock_stats.py): who, queued, got the lock, released
    if ($got) { try { Add-Content -Path (Join-Path $root "work\game.lock.history.tsv") -Value ("{0}`t{1:s}`t{2:s}`t{3:s}" -f $Who, $queued, $got, (Get-Date)) } catch {} }
    Remove-Item $ticket -ErrorAction SilentlyContinue
}
