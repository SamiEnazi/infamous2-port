# Folder names with any character in them (GitHub #19, #34): "E:\Decomps & Recomps\...", "C:\Program Files (x86)\...",
# "inFamous 2 [BCES01229]", a user name or folder in another alphabet. Dot-sourced by tools\setup_common.ps1, tools\port_paths.ps1 and
# tools\update_check.ps1.
#   Invoke-Cmd <line> <paths>  cmd.exe for the few things only it does (mklink /J, a .cmd file), each path handed over
#                              as an environment variable inside double quotes, so no character of a folder's name
#                              (& ^ % ! ( ) , ; = and others) is ever read as cmd syntax
#   New-Junction / Remove-Junction / Remove-Tree   links and folders without cmd (Remove-Tree never enters a link)
#   Test-PlainPath / Get-BuildRoot                 the build tools (CMake, Ninja, the compilers, cmd) are given a
#                              plain path: a folder whose path they can't take is built through a junction with a
#                              plain name (%LOCALAPPDATA%\inFAMOUS2-port\b\<id>, else %ProgramData%\..., else the
#                              system drive's \inFAMOUS2-port-b\<id>)

function Invoke-Cmd([string]$Line, [string[]]$Paths = @()) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo (Join-Path $env:WINDIR "System32\cmd.exe")
    # /s: cmd takes the text between the first and the last quote as is; /v:off: "!" is never a variable
    $psi.Arguments = '/d /v:off /s /c "' + $Line + '"'
    for ($i = 0; $i -lt $Paths.Count; $i++) { $psi.EnvironmentVariables["INF2_P$($i + 1)"] = [string]$Paths[$i] }
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    while ($null -ne ($l = $p.StandardOutput.ReadLine())) { $l }
    $p.WaitForExit()
    foreach ($l in ($err.Result -split "`r?`n")) { if ($l.Trim()) { $l } }
    $global:LASTEXITCODE = $p.ExitCode
}

# A path the "ANSI" side of Windows can hold: programs that are not Unicode (csc.exe, which Add-Type runs to
# compile a few lines of C#; the game's own C file functions) see a name in another alphabet than the PC's
# "language for non-Unicode programs" as "????".
function Test-AnsiPath([string]$Path) {
    $e = [Text.Encoding]::Default
    $e.GetString($e.GetBytes($Path)) -eq $Path
}

# TEMP for this process and what it starts: a user folder named in another alphabet than the PC's language for
# non-Unicode programs (e.g. Arabic letters on an English Windows) broke Add-Type ("Could not write to output file
# ...\Temp\xxxx.dll -- The directory name is invalid"). Then its short 8.3 name, else %ProgramData%\inFAMOUS2-port\tmp.
# An existing folder's path in a form the ANSI side can hold: itself, else its short 8.3 name (the same folder), else $null.
function Get-AnsiPath([string]$Path) {
    if (-not $Path -or (Test-AnsiPath $Path)) { return $Path }
    $short = $null
    try { $short = @(Invoke-Cmd 'for %I in ("%INF2_P1%") do @echo %~sI' @($Path) | Where-Object { $_.Trim() })[0] } catch {}
    if ($short) { $short = $short.Trim() }
    if ($short -and (Test-AnsiPath $short) -and (Test-Path -LiteralPath $short)) { return $short }
    $null
}

function Use-AnsiTemp {
    $t = $env:TEMP
    if (-not $t -or (Test-AnsiPath $t)) { return }
    $short = Get-AnsiPath $t
    if (-not $short) {
        $base = if ($env:ProgramData -and (Test-AnsiPath $env:ProgramData)) { $env:ProgramData } else { "$(if ($env:SystemDrive) { $env:SystemDrive } else { 'C:' })\" }
        $short = Join-Path $base "inFAMOUS2-port\tmp"
        New-Item -ItemType Directory -Force $short | Out-Null
    }
    $env:TEMP = $short; $env:TMP = $short
}

# A junction (no admin needed). Not New-Item: Windows PowerShell 5.1 reads [ ] in -Target as a wildcard (GitHub #19).
function New-Junction([string]$Link, [string]$Target) {
    $o = @(Invoke-Cmd 'mklink /J "%INF2_P1%" "%INF2_P2%"' @($Link, $Target))
    if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $Link)) { throw ("mklink: " + ($o -join " ")) }
}

# The link only, never what it points to (RemoveDirectory removes a junction even when its target is full).
function Remove-Junction([string]$Link) {
    $i = Get-Item -LiteralPath $Link -Force -ErrorAction SilentlyContinue
    if ($i -and ($i.Attributes -band [IO.FileAttributes]::ReparsePoint)) { [IO.Directory]::Delete($i.FullName, $false) }
}

# A folder and everything in it; a link inside is removed as a link, never entered (game\disc points at the
# player's own game). Errors are left to the caller's -ErrorAction (default: stop).
function Remove-Tree([string]$Path) {
    $top = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $top) { return }
    if ($top.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        if ($top -is [IO.DirectoryInfo]) { [IO.Directory]::Delete($top.FullName, $false) } else { $top.Delete() }
        return
    }
    if ($top -isnot [IO.DirectoryInfo]) { $top.Attributes = 'Normal'; $top.Delete(); return }
    $dirs = New-Object System.Collections.Generic.List[string]
    $stack = New-Object System.Collections.Stack
    $stack.Push($top)
    while ($stack.Count) {
        $d = $stack.Pop(); $dirs.Add($d.FullName)
        foreach ($e in $d.GetFileSystemInfos()) {
            if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                if ($e -is [IO.DirectoryInfo]) { [IO.Directory]::Delete($e.FullName, $false) } else { $e.Delete() }
            } elseif ($e -is [IO.DirectoryInfo]) { $stack.Push($e) }
            else { if ($e.Attributes -band [IO.FileAttributes]::ReadOnly) { $e.Attributes = 'Normal' }; $e.Delete() }
        }
    }
    for ($i = $dirs.Count - 1; $i -ge 0; $i--) { [IO.Directory]::Delete($dirs[$i], $false) }
}

# A path every build tool takes as is: a drive letter, then letters, digits, space, _ . - ( ) only (so a
# "Program Files (x86)" folder keeps building where it always did), and short enough that the build's deepest
# files stay under Windows' 260-character limit (the engine's object files sit at about twice the folder's
# length plus 100 characters).
function Test-PlainPath([string]$Path) {
    $Path -match '^[A-Za-z]:\\[A-Za-z0-9 _.\-\\()]*$' -and $Path.Length -le 80
}

# The folder the build tools work in for the port folder $Root: $Root itself when its path is plain, else a
# junction to it with a plain name (made when -Create and not there yet; the same one every time, so the
# build files of an update fit). $null when there is no such place (only without -Create; with it: an error
# that says what to do).
function Get-BuildRoot([string]$Root, [switch]$Create) {
    $Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (Test-PlainPath $Root) { return $Root }
    $sha = [Security.Cryptography.SHA1]::Create()
    $id = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Root.ToLowerInvariant()))[0..4] | ForEach-Object { $_.ToString("x2") })
    $bases = @()
    if ($env:LOCALAPPDATA) { $bases += Join-Path $env:LOCALAPPDATA "inFAMOUS2-port\b" }
    if ($env:ProgramData) { $bases += Join-Path $env:ProgramData "inFAMOUS2-port\b" }
    $sys = if ($env:SystemDrive) { $env:SystemDrive } else { "C:" }
    $bases += "$sys\inFAMOUS2-port-b"
    foreach ($b in $bases) {
        $link = Join-Path $b $id
        if (-not (Test-PlainPath $link) -or $link.Length -gt 60) { continue }
        $i = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
        if ($i) {
            if ($i.LinkType -eq "Junction" -and ([string]@($i.Target)[0]).TrimEnd('\') -eq $Root) { return $link }
            if (-not $Create) { continue }
            if ($i.LinkType -eq "Junction") { Remove-Junction $link } else { continue }   # a folder of something else: not ours
        }
        if (-not $Create) { continue }
        try {
            $di = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($link))
            if ($di.DriveType -ne [IO.DriveType]::Fixed -or $di.DriveFormat -notin @("NTFS", "ReFS")) { continue }
            New-Item -ItemType Directory -Force $b -ErrorAction Stop | Out-Null
            New-Junction $link $Root
            return $link
        } catch { continue }
    }
    if ($Create) {
        throw ("the build tools can't work in a folder whose path has these characters or is this long ($Root), and Setup " +
               "could not make a short link to it in $($bases -join ', '). Move the port to a folder with a plain name, " +
               "for example C:\Games\inFAMOUS2, and run Setup again")
    }
    $null
}

# (on load: every script that dot-sources this file, and what it starts, gets a TEMP that csc and the game can use)
Use-AnsiTemp
