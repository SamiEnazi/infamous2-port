# The desktop shortcut "inFAMOUS 2 (PC port)". Setup makes it (tools\setup_build.ps1); tools\play.ps1 makes it again
# at a start when the port's folder was moved or copied (another drive, another PC) and the shortcut points to a
# folder that is not there any more. Only for an install whose Setup made one (work\shortcut.txt: the shortcut's file
# name, no path), and never over a shortcut that points to another install that is there.
# PORT_DESKTOP=<folder>: the desktop folder (tests).

function Get-PortDesktop {
    if ($env:PORT_DESKTOP) { return $env:PORT_DESKTOP }
    [Environment]::GetFolderPath("Desktop")
}

function New-PortShortcut([string]$Root) {
    $name = "inFAMOUS 2 (PC port).lnk"
    $desk = Get-PortDesktop
    New-Item -ItemType Directory -Force $desk | Out-Null
    $lnkPath = Join-Path $desk $name
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut($lnkPath)
    $lnk.TargetPath = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
    # the port's own folder, not a plain-named link the build may go through (play.ps1 finds that itself)
    $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Root 'tools\play.ps1')`""
    $lnk.WorkingDirectory = $Root
    $ico = Join-Path $Root "work\icon\app.ico"
    $lnk.IconLocation = if (Test-Path -LiteralPath $ico) { "$ico,0" } else { "$(Join-Path $Root 'work\port\build\Infamous2Recomp.exe'),0" }
    $lnk.WindowStyle = 7
    $lnk.Description = "inFAMOUS 2, natively on Windows (your own copy, recompiled)"
    $lnk.Save()
    New-Item -ItemType Directory -Force (Join-Path $Root "work") | Out-Null
    Set-Content -LiteralPath (Join-Path $Root "work\shortcut.txt") -Value $name -Encoding ascii
    $lnkPath
}

# At a start: the shortcut again when it points nowhere (the folder was moved) or is gone (another PC).
function Update-PortShortcut([string]$Root) {
    $mark = Join-Path $Root "work\shortcut.txt"
    if (-not (Test-Path -LiteralPath $mark)) { return }
    try {
        $name = ([string](Get-Content -LiteralPath $mark -TotalCount 1)).Trim()
        if (-not $name) { return }
        $lnkPath = Join-Path (Get-PortDesktop) $name
        if (Test-Path -LiteralPath $lnkPath) {
            $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut($lnkPath)
            if ($lnk.Arguments -match '-File\s+"([^"]+)"') {
                $t = $Matches[1]
                if ($t -ieq (Join-Path $Root "tools\play.ps1")) { return }   # ours
                if (Test-Path -LiteralPath $t) { return }                     # another install's, which is there: left alone
            }
        }
        $p = New-PortShortcut $Root
        "The desktop shortcut starts the game from here now ($p)."
    } catch { "The desktop shortcut could not be made again ($($_.Exception.Message)); the game starts the same." }
}
