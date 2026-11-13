# Is there a newer version of the port? tools\play.ps1 runs this in the background at every start (the
# game never waits for it) and the game shows the answer: a notice, and F2 > Update.
#   tools\update_check.ps1 [-Root <port folder>] [-Force] [-CacheOnly]   one check; prints the status file
#   tools\update_check.ps1 -StartUpdate [-DryRun]                         start Setup.exe --update (play.ps1,
#                                                                         after the game asked for the update)
# It compares this copy (port-version.txt: the commit Setup.exe downloaded, or the release a zip came from)
# with the newest GitHub release. GitHub's public API, no sign-in (60 questions an hour per network): the
# latest release and the repository's tags, each asked with the ETag of the last answer (an unchanged
# answer is a short "304"), and only for a copy between two releases, once, how its commit relates to the
# release. At most every 6 hours (-Force: now; -CacheOnly: never), 5 s per question; offline or limited,
# the last answer stays.
# Files, in work\update\ (never published, never in a release zip):
#   status.txt   what the game reads (engine\libs\video\vk_overlay_update_status.h: key=value)
#   check.json   the answers kept between starts
#   setup\       Setup.exe built from this copy's tools\setup_exe for -StartUpdate (the in-box C# compiler)
param(
    [string]$Root = "",
    [switch]$Force,
    [switch]$CacheOnly,
    [switch]$StartUpdate,
    [switch]$DryRun,
    [string]$ApiBase = "https://api.github.com",
    [double]$MaxAgeHours = 6,
    [int]$TimeoutMs = 5000
)
$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
$Repo = "SamiEnazi/infamous2-port"
$Dir = Join-Path $Root "work\update"
$StatusFile = Join-Path $Dir "status.txt"
$CacheFile = Join-Path $Dir "check.json"
$Utf8 = New-Object System.Text.UTF8Encoding($false)

# ---------------------------------------------------------------- this copy

# port-version.txt: line 1 "Downloaded by Setup.exe <time>" or "Release v0.4.1 (<zip>)", then
# "SamiEnazi/infamous2-port <commit>" (the engine and the port base are in that commit since v0.5.0; older
# copies also list the toolkit / port base pins, which are not needed here).
function Get-InstalledPort {
    $pv = Join-Path $Root "port-version.txt"
    if (-not (Test-Path -LiteralPath $pv)) { return $null }
    $lines = @([IO.File]::ReadAllLines($pv))
    $sha = $null
    foreach ($l in $lines) {
        $t = @($l.Trim() -split '\s+')
        if ($t.Count -eq 2 -and $t[0] -eq $Repo -and $t[1] -match '^[0-9a-f]{40}$') { $sha = $t[1] }
    }
    if (-not $sha) { return $null }
    $rel = $null
    if ($lines.Count -and $lines[0] -match '^Release (v?\d+(\.\d+)+)') { $rel = $Matches[1] }
    [pscustomobject]@{ Sha = $sha; Release = $rel }
}

function Get-Ver([string]$Tag) {
    try { return [version]($Tag.TrimStart('v', 'V') -replace '[-+].*$', '') } catch { return $null }
}

# ---------------------------------------------------------------- GitHub

function Invoke-Gh([string]$Path, [string]$Accept, [string]$ETag) {
    $r = [Net.HttpWebRequest]::Create("$ApiBase/$Path")
    $r.UserAgent = "inFAMOUS2-port-updater"
    $r.Accept = $Accept
    $r.Timeout = $TimeoutMs
    $r.ReadWriteTimeout = $TimeoutMs
    if ($ETag) { $r.Headers["If-None-Match"] = $ETag }
    try { $resp = $r.GetResponse() }
    catch [Net.WebException] {
        $hr = $_.Exception.Response
        if (-not $hr) { throw }                         # no answer at all: offline, DNS, timeout
        $code = [int]$hr.StatusCode
        $o = [pscustomobject]@{ Code = $code; Body = $null; ETag = $ETag; Reset = $hr.Headers["X-RateLimit-Reset"] }
        $hr.Close()
        return $o
    }
    try {
        $sr = New-Object IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)
        $body = $sr.ReadToEnd()
        [pscustomobject]@{ Code = 200; Body = $body; ETag = $resp.Headers["ETag"]; Reset = $null }
    } finally { $resp.Close() }
}

function Read-Cache {
    $c = $null
    try { if (Test-Path -LiteralPath $CacheFile) { $c = [IO.File]::ReadAllText($CacheFile) | ConvertFrom-Json } } catch { $c = $null }
    if (-not $c) { $c = [pscustomobject]@{} }
    foreach ($k in "installed", "release", "release_etag", "tags", "tags_etag", "compares", "last_ok", "last_try", "retry_after", "error") {
        if (-not ($c.PSObject.Properties.Name -contains $k)) { $c | Add-Member -NotePropertyName $k -NotePropertyValue $null }
    }
    if (-not $c.tags) { $c.tags = @() }
    if (-not $c.compares) { $c.compares = @() }
    $c
}

function Save-Cache($c) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $tmp = "$CacheFile.tmp"
    [IO.File]::WriteAllText($tmp, ($c | ConvertTo-Json -Depth 6), $Utf8)
    Move-Item -LiteralPath $tmp -Destination $CacheFile -Force
}

function Get-TagSha($c, [string]$Name) {
    foreach ($t in @($c.tags)) { if ($t.name -eq $Name) { return $t.sha } }
    $null
}
function Get-TagOf($c, [string]$Sha) {
    foreach ($t in @($c.tags)) { if ($t.sha -eq $Sha) { return $t.name } }
    $null
}

# Asks GitHub what changed since the kept answers; fills $c. Errors stay in $c.error.
function Update-FromGitHub($c) {
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072 } catch {}   # TLS 1.2
    $c.last_try = (Get-Date).ToUniversalTime().ToString("o")
    try {
        $r = Invoke-Gh "repos/$Repo/releases/latest" "application/vnd.github+json" $(if ($c.release) { $c.release_etag } else { $null })
        if ($r.Code -eq 200) {
            $j = $r.Body | ConvertFrom-Json
            $c.release = [pscustomobject]@{ tag = [string]$j.tag_name; name = [string]$j.name; body = [string]$j.body;
                                            url = [string]$j.html_url; published = [string]$j.published_at }
            $c.release_etag = $r.ETag
        } elseif ($r.Code -eq 404) { $c.release = $null; $c.release_etag = $null }
        elseif ($r.Code -ne 304) { return Set-GhError $c $r }
        $r = Invoke-Gh "repos/$Repo/tags?per_page=100" "application/vnd.github+json" $(if (@($c.tags).Count) { $c.tags_etag } else { $null })
        if ($r.Code -eq 200) {
            $list = $r.Body | ConvertFrom-Json            # (Windows PowerShell passes a JSON array on as one object)
            $c.tags = @(foreach ($t in $list) { [pscustomobject]@{ name = [string]$t.name; sha = [string]$t.commit.sha } })
            $c.tags_etag = $r.ETag
        } elseif ($r.Code -ne 304) { return Set-GhError $c $r }
        $c.last_ok = $c.last_try
        $c.error = $null
        $c.retry_after = $null
    } catch {
        $m = $_.Exception.Message
        if ($_.Exception.InnerException) { $m = $_.Exception.InnerException.Message }
        $c.error = "GitHub could not be reached ($($m.Trim().TrimEnd('.')))"
        # offline: try again at a later start, not at every one
        $c.retry_after = (Get-Date).ToUniversalTime().AddMinutes(30).ToString("o")
    }
}

function Set-GhError($c, $r) {
    if ($r.Code -eq 403 -or $r.Code -eq 429) {
        $c.error = "GitHub is limiting questions from this network right now"
        $until = (Get-Date).ToUniversalTime().AddHours(1)
        try { if ($r.Reset) { $until = [DateTimeOffset]::FromUnixTimeSeconds([long]$r.Reset).UtcDateTime.AddMinutes(1) } } catch {}
        $c.retry_after = $until.ToString("o")
    } else {
        $c.error = "GitHub answered $($r.Code)"
        $c.retry_after = (Get-Date).ToUniversalTime().AddHours(1).ToString("o")
    }
}

# How this copy's commit relates to the release's: identical / ahead / behind / diverged / unknown
# (GitHub does not know the commit). Asked once per pair, kept.
function Get-Relation($c, [string]$TagSha, [string]$Sha, [bool]$MayAsk) {
    $key = "$TagSha...$Sha"
    foreach ($x in @($c.compares)) { if ($x.key -eq $key) { return $x } }
    if (-not $MayAsk) { return $null }
    try {
        $r = Invoke-Gh "repos/$Repo/compare/$key`?per_page=1" "application/vnd.github+json" $null
        $x = $null
        if ($r.Code -eq 200) {
            $j = $r.Body | ConvertFrom-Json
            $x = [pscustomobject]@{ key = $key; status = [string]$j.status; ahead = [int]$j.ahead_by; behind = [int]$j.behind_by }
        } elseif ($r.Code -eq 404) { $x = [pscustomobject]@{ key = $key; status = "unknown"; ahead = 0; behind = 0 } }
        else { Set-GhError $c $r; return $null }
        $c.compares = @(@($c.compares) + $x | Select-Object -Last 20)
        return $x
    } catch { return $null }
}

# ---------------------------------------------------------------- release notes: plain lines

function ConvertTo-PlainNotes([string]$Md) {
    $out = New-Object System.Collections.Generic.List[string]
    if (-not $Md) { return ,$out }
    $blank = $false
    foreach ($raw in (($Md -replace "`r", "") -split "`n")) {
        $l = $raw
        $l = [regex]::Replace($l, '[\uD800-\uDFFF\u2600-\u27BF\uFE0F\u200D]', '')    # emoji and symbols: the game's fonts lack them
        $l = [regex]::Replace($l, '!\[[^\]]*\]\([^)]*\)', '')                       # pictures
        $l = [regex]::Replace($l, '\[([^\]]+)\]\(([^)]+)\)', '$1')                  # links: their text
        $l = [regex]::Replace($l, '<[^>]+>', '')                                    # HTML tags
        $l = $l.Replace('**', '').Replace('`', '')
        $l = [regex]::Replace($l, '^\s{0,3}#{1,6}\s*', '')                          # headings
        $l = [regex]::Replace($l, '^\s*>\s?', '')                                   # quotes
        $l = [regex]::Replace($l, '^(\s*)[\*\+]\s+', '$1- ')                        # bullets
        $l = [regex]::Replace($l, '^\s*([-*_]\s*){3,}$', '')                        # rules
        $l = $l.TrimEnd()
        if (-not $l.Trim()) {
            if ($out.Count -and -not $blank) { $out.Add(""); $blank = $true }
            continue
        }
        $blank = $false
        $out.Add($l)
        if ($out.Count -ge 120) { $out.Add("(more on the release page)"); break }
    }
    while ($out.Count -and -not $out[$out.Count - 1]) { $out.RemoveAt($out.Count - 1) }
    return ,$out
}

# ---------------------------------------------------------------- the answer

function Get-Answer($c, $inst, [bool]$MayAsk) {
    $a = [ordered]@{ state = "unknown"; installed = ""; latest = ""; title = ""; published = ""; url = ""; checked = ""; message = "" }
    $short = $inst.Sha.Substring(0, 7)
    $mine = Get-TagOf $c $inst.Sha
    $a.installed = if ($mine) { $mine } elseif ($inst.Release) { $inst.Release } else { "commit $short" }
    if ($c.last_ok) { try { $a.checked = ([DateTime]::Parse($c.last_ok, $null, 'RoundtripKind')).ToLocalTime().ToString("yyyy-MM-dd HH:mm") } catch {} }
    $rel = $c.release
    if (-not $rel -or -not $rel.tag) {
        $a.message = if ($c.error) { "$($c.error)." } elseif ($c.last_ok) { "No release on GitHub yet." } else { "GitHub has not been asked yet." }
        return $a
    }
    $a.latest = $rel.tag; $a.title = $rel.name; $a.url = $rel.url
    if ($rel.published) { try { $a.published = ([DateTime]::Parse($rel.published, $null, 'RoundtripKind')).ToLocalTime().ToString("yyyy-MM-dd") } catch {} }
    $tagSha = Get-TagSha $c $rel.tag
    $vRel = Get-Ver $rel.tag
    if ($tagSha -and $tagSha -eq $inst.Sha) { $a.state = "current" }
    elseif ($mine -and $vRel -and (Get-Ver $mine)) { $a.state = if ((Get-Ver $mine) -lt $vRel) { "available" } else { "current" } }
    elseif ($inst.Release -and $vRel -and (Get-Ver $inst.Release)) { $a.state = if ((Get-Ver $inst.Release) -lt $vRel) { "available" } else { "current" } }
    elseif ($tagSha) {
        $x = Get-Relation $c $tagSha $inst.Sha $MayAsk
        if (-not $x) { $a.message = "Could not compare this copy with $($rel.tag) yet." }
        elseif ($x.status -eq "identical") { $a.state = "current" }
        elseif ($x.status -eq "ahead") { $a.state = "current"; $a.installed = "$($rel.tag) + $($x.ahead) change(s) (commit $short)" }
        elseif ($x.status -eq "behind" -or $x.status -eq "diverged") { $a.state = "available"; $a.installed = "an earlier version (commit $short)" }
        else { $a.message = "GitHub does not know this copy's commit ($short): it was not made by Setup.exe from GitHub." }
    } else { $a.message = "The release $($rel.tag) is not among GitHub's tags yet." }
    if ($c.error) { $a.message = (("$($c.error); the answer is from " + $(if ($a.checked) { $a.checked } else { "an earlier start" }) + ". " + $a.message)).Trim() }
    $a
}

function Write-Status($a, $notes) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $sb = New-Object System.Text.StringBuilder
    foreach ($k in $a.Keys) { [void]$sb.Append("$k=$(([string]$a[$k]) -replace '[\r\n]+', ' ')`n") }
    if ($a.state -eq "available") { foreach ($n in $notes) { [void]$sb.Append("notes=$n`n") } }
    $tmp = "$StatusFile.tmp"
    [IO.File]::WriteAllText($tmp, $sb.ToString(), $Utf8)
    # the game may be reading the old one this very moment (a few microseconds every 2 s): try again
    for ($i = 0; ; $i++) {
        try {
            if (Test-Path -LiteralPath $StatusFile) { [IO.File]::Replace($tmp, $StatusFile, [NullString]::Value) } else { [IO.File]::Move($tmp, $StatusFile) }
            break
        } catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 100 }
    }
}

function Invoke-Check {
    $inst = Get-InstalledPort
    if (-not $inst) {
        Write-Status ([ordered]@{ state = "off"; installed = ""; message = "This folder has no port-version.txt: there is no installed version to compare." }) @()
        return
    }
    $c = Read-Cache
    $now = (Get-Date).ToUniversalTime()
    function Age([string]$t) { if (-not $t) { return [double]::MaxValue }; try { ($now - [DateTime]::Parse($t, $null, 'RoundtripKind').ToUniversalTime()).TotalHours } catch { [double]::MaxValue } }
    $waitOver = -not $c.retry_after -or (Age $c.retry_after) -ge 0
    # asked again when the kept answer is old, or is about another commit (this copy was just updated)
    $ask = $Force -or (-not $CacheOnly -and $waitOver -and ((Age $c.last_ok) -ge $MaxAgeHours -or $c.installed -ne $inst.Sha))
    if ($ask) { Update-FromGitHub $c; if (-not $c.error) { $c.installed = $inst.Sha } }
    $a = Get-Answer $c $inst ($ask -and -not $c.error)
    if ($CacheOnly -and -not $Force) { $a.message = ("Automatic checks are off (Check now asks GitHub). " + $a.message).Trim() }
    try { Save-Cache $c } catch {}
    $notes = if ($c.release) { ConvertTo-PlainNotes $c.release.body } else { @() }
    Write-Status $a $notes
}

# ---------------------------------------------------------------- the update itself

# A command-line argument quoted the way Windows splits them again.
function ConvertTo-Arg([string]$s) {
    if ($s.Length -and $s.IndexOfAny([char[]]" `t`"") -lt 0) { return $s }
    $b = New-Object System.Text.StringBuilder('"')
    $bs = 0
    foreach ($ch in $s.ToCharArray()) {
        if ($ch -eq '\') { $bs++; continue }
        if ($ch -eq '"') { [void]$b.Append('\', $bs * 2 + 1).Append('"'); $bs = 0; continue }
        [void]$b.Append('\', $bs).Append($ch); $bs = 0
    }
    [void]$b.Append('\', $bs * 2).Append('"')
    $b.ToString()
}

# A Setup.exe that knows --update: built from this copy's own tools\setup_exe (the C# compiler that comes
# with Windows, as tools\setup_exe\build.cmd does; again only when those files change), else the folder's
# Setup.exe when it is new enough. The folder's Setup.exe is the one the player first downloaded: an
# update never replaces it, so it may be older than --update.
function Get-UpdateSetup {
    $src = Join-Path $Root "tools\setup_exe"
    $setupDir = Join-Path $Dir "setup"
    $exe = Join-Path $setupDir "Setup.exe"
    $stamp = Join-Path $setupDir "source.sha256"
    if (Test-Path -LiteralPath (Join-Path $src "build.cmd")) {
        $ico = Join-Path $Root "tools\setup.ico"
        $files = @(Get-ChildItem -LiteralPath $src -File -Recurse | Sort-Object FullName) + @(Get-Item -LiteralPath $ico -ErrorAction SilentlyContinue)
        $h = ($files | ForEach-Object { $_.Name + "=" + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }) -join ";"
        if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $stamp) -and ([IO.File]::ReadAllText($stamp).Trim() -eq $h)) { return $exe }
        # build.cmd writes ..\..\Setup.exe and reads ..\setup.ico: a copy laid out the same way
        $b = Join-Path $setupDir "src"
        if (Test-Path -LiteralPath $b) { cmd /c rd /s /q "$b" | Out-Null }
        New-Item -ItemType Directory -Force (Join-Path $b "tools\setup_exe") | Out-Null
        Get-ChildItem -LiteralPath $src | Copy-Item -Destination (Join-Path $b "tools\setup_exe") -Recurse -Force
        if (Test-Path -LiteralPath $ico) { Copy-Item -LiteralPath $ico -Destination (Join-Path $b "tools") }
        Write-Host "  building Setup.exe from tools\setup_exe (the C# compiler that comes with Windows)"
        $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        try { cmd /c "`"$(Join-Path $b 'tools\setup_exe\build.cmd')`"" 2>&1 | ForEach-Object { Write-Host "    $_" } } finally { $ErrorActionPreference = $eap }
        $built = Join-Path $b "Setup.exe"
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $built)) {
            Move-Item -LiteralPath $built -Destination $exe -Force
            [IO.File]::WriteAllText($stamp, $h, $Utf8)
            cmd /c rd /s /q "$b" | Out-Null
            return $exe
        }
        Write-Host "  could not build Setup.exe (exit code $LASTEXITCODE)"
    }
    $own = Join-Path $Root "Setup.exe"
    if ((Test-Path -LiteralPath $own) -and [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($own)).Contains("--update")) { return $own }
    $null
}

function Start-PortUpdate {
    $latest = ""
    if (Test-Path -LiteralPath $StatusFile) {
        foreach ($l in [IO.File]::ReadAllLines($StatusFile)) { if ($l -match '^latest=(.+)$') { $latest = $Matches[1].Trim() } }
    }
    $exe = Get-UpdateSetup
    if (-not $exe) {
        $own = Join-Path $Root "Setup.exe"
        "No Setup.exe here knows the update switch. Run Setup.exe in $Root and choose 'Update to the latest version'."
        if (-not $DryRun -and (Test-Path -LiteralPath $own)) { Start-Process -FilePath $own -WorkingDirectory $Root }
        return
    }
    $a = @("--update", "--folder", $Root)
    if ($latest) { $a += "--to", $latest }
    # the game folder Setup linked (game\disc) and RPCS3 (config.local.json): nothing to ask
    $link = Get-Item -LiteralPath (Join-Path $Root "game\disc") -Force -ErrorAction SilentlyContinue
    if ($link -and $link.LinkType -eq "Junction") {
        $t = [string]@($link.Target)[0]
        if ($t -and (Test-Path -LiteralPath (Join-Path $t "PS3_GAME\PARAM.SFO"))) { $a += "--disc", $t }
    }
    try {
        $cfg = [IO.File]::ReadAllText((Join-Path $Root "config.local.json")) | ConvertFrom-Json
        if ($cfg.rpcs3 -and (Test-Path -LiteralPath (Join-Path $cfg.rpcs3 "rpcs3.exe"))) { $a += "--rpcs3", [string]$cfg.rpcs3 }
    } catch {}
    $line = ($a | ForEach-Object { ConvertTo-Arg $_ }) -join " "
    if ($DryRun) { "dry run: would start $exe $line"; return }
    "Starting the update$(if ($latest) { " to $latest" }): Setup.exe shows how far it is, then Play."
    Start-Process -FilePath $exe -ArgumentList $line -WorkingDirectory $Root
}

if ($StartUpdate) { Start-PortUpdate }
else {
    Invoke-Check
    [IO.File]::ReadAllText($StatusFile)
}
