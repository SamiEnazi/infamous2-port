# The RecompLauncher zip of a release, inFAMOUS2-port-v<version>-Windows-x64.zip: the port the way Setup.exe
# downloads it (this repository at one commit, the toolkit and the port base at the commits it pins), with
# Setup.exe, port-version.txt and recomplauncher.json at the top. RecompLauncher installs the zip, runs
# "Setup.exe --prepare" (the build, from the player's own copy) and then "Setup.exe --play".
# Only committed files go in (git archive, LF line ends as in GitHub's own ZIP files). It never holds a game
# file or firmware: the toolkit's fw_spu\ (upstream's firmware modules) is left out, and the zip is refused
# if any other one is found. Attach it to the GitHub release next to Setup.exe.
#   tools\make_release_zip.ps1 -Version 0.4.1 -SetupExe <Setup.exe built from the same commit>
#       [-Repo <a clone of SamiEnazi/infamous2-port>] [-Commit HEAD] [-Out <folder>]
#       [-Toolkit <git repo holding the toolkit commit>] [-PortBase <git repo holding the port base commit>]
#       [-ToolkitSha <sha>] [-PortBaseSha <sha>]
# The pins come from -Repo's submodules at -Commit (give the shas when -Repo has none). -Toolkit and -PortBase
# default to -Repo's submodule folders.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$SetupExe,
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [string]$Commit = "HEAD",
    [string]$Out = "",
    [string]$Toolkit = "",
    [string]$PortBase = "",
    [string]$ToolkitSha = "",
    [string]$PortBaseSha = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$Version = $Version.TrimStart('v')
$Repo = (Resolve-Path $Repo).Path
if (-not $Out) { $Out = Join-Path $Repo "work\release" }
if (-not $Toolkit) { $Toolkit = Join-Path $Repo "toolkit" }
if (-not $PortBase) { $PortBase = Join-Path $Repo "community\erikomis-infamous2-recomp" }
$name = "inFAMOUS2-port-v$Version-Windows-x64"

function GitOut([string]$Dir, [string[]]$GitArgs) {
    $r = & git -C $Dir @GitArgs
    if ($LASTEXITCODE) { throw "git -C $Dir $($GitArgs -join ' ') failed" }
    $r
}
function Pin([string]$Path, [string]$Given) {
    if ($Given) { return $Given }
    $line = GitOut $Repo @("ls-tree", $script:sha, "--", $Path)
    if ($line -notmatch '^160000 commit ([0-9a-f]{40})\s') { throw "$Path is not a submodule at $($script:sha): give its commit (-ToolkitSha / -PortBaseSha)" }
    $Matches[1]
}
# git archive of one commit, unpacked into $Dst
function Unpack([string]$Dir, [string]$Rev, [string]$Dst, [string]$What) {
    $tmp = Join-Path $Out "$name.part.zip"
    Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
    & git -c core.autocrlf=false -c core.eol=lf -C $Dir archive --format=zip -o $tmp $Rev
    if ($LASTEXITCODE -or -not (Test-Path $tmp)) { throw "git archive of $What ($Rev in $Dir) failed: is that commit in this repository?" }
    New-Item -ItemType Directory -Force $Dst | Out-Null
    [System.IO.Compression.ZipFile]::ExtractToDirectory($tmp, $Dst)
    Remove-Item -LiteralPath $tmp
    "  $What`: $Rev"
}

$script:sha = (GitOut $Repo @("rev-parse", "$Commit^{commit}")).Trim()
$tk = Pin "toolkit" $ToolkitSha
$pb = Pin "community/erikomis-infamous2-recomp" $PortBaseSha
foreach ($f in "recomplauncher.json", "tools/setup_common.ps1", "tools/setup_build.ps1", "tools/play.ps1") {
    # Windows PowerShell turns a native program's stderr into an error under "Stop"
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & git -C $Repo cat-file -e "$($script:sha):$f" 2>$null } finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE) { throw "$f is not in $($script:sha)" }
}
$exeBytes = [System.IO.File]::ReadAllBytes((Resolve-Path $SetupExe).Path)
if ([System.Text.Encoding]::Unicode.GetString($exeBytes).IndexOf("--prepare") -lt 0) { throw "$SetupExe has no --prepare: build it from this commit (tools\setup_exe\build.cmd)" }

New-Item -ItemType Directory -Force $Out | Out-Null
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
"$name.zip"
Unpack $Repo $script:sha $stage "the port"
Unpack $Toolkit $tk (Join-Path $stage "toolkit") "the toolkit"
# The toolkit's fw_spu\ (from upstream ps3recomp) holds decrypted PS3 firmware modules (libsre.prx, libadec.prx,
# ...) and code lifted from them. Nothing in this port's build reads it (the toolkit's CMake globs libs\ and
# runtime\; the port lifts the player's own firmware from RPCS3 into work\port), and a release never carries
# firmware: it stays out.
$fw = Join-Path $stage "toolkit\fw_spu"
if (Test-Path -LiteralPath $fw) { Remove-Item -LiteralPath $fw -Recurse -Force; "  left out: toolkit\fw_spu (PS3 firmware modules from upstream; not built)" }
Unpack $PortBase $pb (Join-Path $stage "community\erikomis-infamous2-recomp") "the port base"
Copy-Item -LiteralPath (Resolve-Path $SetupExe).Path (Join-Path $stage "Setup.exe")
# the same lines Setup.exe writes after its own download (its update compares them with GitHub)
[System.IO.File]::WriteAllText((Join-Path $stage "port-version.txt"),
    "Release v$Version ($name.zip)`r`nSamiEnazi/infamous2-port $($script:sha)`r`ntoolkit SamiEnazi/ps3recomp $tk`r`n" +
    "community/erikomis-infamous2-recomp erikomis/infamous2-recomp $pb`r`n")

# never a game file, firmware or key (the repositories hold none; this makes sure)
$game = '\.(elf|self|sprx|prx|pkg|rap|edat|sdat|pup|iso)$|\.psarc|(^|\\)(EBOOT\.BIN|PARAM\.SFO)$'
$bad = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.FullName.Substring($stage.Length + 1) -match $game }
if ($bad) { $bad | ForEach-Object { "  GAME FILE: $($_.FullName.Substring($stage.Length + 1))" }; throw "game files in the zip: nothing made" }
$json = Get-Content -LiteralPath (Join-Path $stage "recomplauncher.json") -Raw | ConvertFrom-Json
foreach ($p in @($json.start.program) + @($json.prepare.program)) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $p))) { throw "recomplauncher.json starts $p, which is not in the zip" }
}

$zip = Join-Path $Out "$name.zip"
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
# Entry by entry with "/" in the names: Windows PowerShell's ZipFile.CreateFromDirectory writes "\", which the
# ZIP format does not allow (other unzippers make files named "tools\play.ps1")
$fs = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew)
$za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -LiteralPath $stage -Recurse -File -Force) {
        $rel = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose(); $fs.Dispose() }
Remove-Item -LiteralPath $stage -Recurse -Force
$z = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $names = @($z.Entries | ForEach-Object { $_.FullName })
    $top = @($names | Where-Object { $_ -notmatch '/' })
} finally { $z.Dispose() }
if ($names | Where-Object { $_ -match '\\' }) { throw "$zip has entries with a backslash" }
foreach ($f in "recomplauncher.json", "Setup.exe", "port-version.txt") { if ($top -notcontains $f) { throw "$f is not at the top of $zip" } }
"  $($names.Count) files, $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, at the top: $($top -join ', ')"
"  sha256 $((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower())"
$zip
