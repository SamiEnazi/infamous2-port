# The RecompLauncher zip of a release, inFAMOUS2-port-v<version>-Windows-x64.zip: the port the way Setup.exe
# downloads it (this repository at one commit, with the engine in engine\ and the port base in base\ since
# v0.5.0), with Setup.exe, port-version.txt and recomplauncher.json at the top. RecompLauncher installs the
# zip, runs "Setup.exe --prepare" (the build, from the player's own copy) and then "Setup.exe --play".
# Only committed files go in (git archive, LF line ends as in GitHub's own ZIP files). It never holds a game
# file or firmware (refused if one is found), nor the development repository's own files (PLAN.md,
# research\, patches\toolkit\: make the zip from the public repository's commit). Attach it to the GitHub
# release next to Setup.exe.
#   tools\make_release_zip.ps1 -Version 0.5.0 -SetupExe <Setup.exe built from the same commit> [-RequireSigned]
#       [-Repo <a clone of SamiEnazi/infamous2-port>] [-Commit HEAD] [-Out <folder>]
# Setup.exe: the signed one from the release, which .github/workflows/setup-exe.yml built and SignPath signed
# (gh release download v0.5.0 -R SamiEnazi/infamous2-port -p Setup.exe -D work\release\signed). An unsigned
# one gets a warning (-RequireSigned: refused); a broken or untrusted signature (SignPath's test-signing) is refused.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$SetupExe,
    [switch]$RequireSigned,
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [string]$Commit = "HEAD",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$Version = $Version.TrimStart('v')
$Repo = (Resolve-Path $Repo).Path
if (-not $Out) { $Out = Join-Path $Repo "work\release" }
$name = "inFAMOUS2-port-v$Version-Windows-x64"

function GitOut([string]$Dir, [string[]]$GitArgs) {
    $r = & git -C $Dir @GitArgs
    if ($LASTEXITCODE) { throw "git -C $Dir $($GitArgs -join ' ') failed" }
    $r
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
foreach ($f in "recomplauncher.json", "tools/setup_common.ps1", "tools/setup_checks.ps1", "tools/setup_build.ps1", "tools/play.ps1",
               "engine/CMakeLists.txt", "engine/LICENSE", "base/LICENSE", "base/infamous2-lle/CMakeLists.txt") {
    # Windows PowerShell turns a native program's stderr into an error under "Stop"
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & git -C $Repo cat-file -e "$($script:sha):$f" 2>$null } finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE) { throw "$f is not in $($script:sha)" }
}
$exeBytes = [System.IO.File]::ReadAllBytes((Resolve-Path $SetupExe).Path)
# (a UTF-16 string of the exe: at an even or an odd byte offset, as the compiler laid it out)
if ([System.Text.Encoding]::Unicode.GetString($exeBytes).IndexOf("--prepare") -lt 0 -and
    [System.Text.Encoding]::Unicode.GetString($exeBytes, 1, $exeBytes.Length - 1).IndexOf("--prepare") -lt 0) { throw "$SetupExe has no --prepare: build it from this commit (tools\setup_exe\build.cmd)" }
# its version information (Explorer > Properties > Details) should name this release
$fv = (Get-Item -LiteralPath (Resolve-Path $SetupExe).Path).VersionInfo.FileVersion
if ($fv -ne "$Version.0" -and $fv -ne $Version) { Write-Warning "Setup.exe says version $fv, not $($Version): set AssemblyVersion/FileVersion/InformationalVersion at the top of tools\setup_exe\SetupWizard.cs (and setup.manifest) and build it again" }
# the zip carries the same Setup.exe as the release: the signed one
$sig = Get-AuthenticodeSignature -LiteralPath (Resolve-Path $SetupExe).Path
if ($sig.Status -eq "Valid") { $signedBy = "  Setup.exe signed by $($sig.SignerCertificate.Subject)" }
elseif ($sig.Status -eq "NotSigned") {
    $m = "$SetupExe is not signed: take the release's signed one (gh release download v$Version -R SamiEnazi/infamous2-port -p Setup.exe -D work\release\signed)"
    if ($RequireSigned) { throw $m }
    Write-Warning $m; $signedBy = "  Setup.exe NOT signed"
}
else { throw "$SetupExe has a signature Windows does not accept ($($sig.Status): $($sig.StatusMessage)): not for a release" }

New-Item -ItemType Directory -Force $Out | Out-Null
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
"$name.zip"
$signedBy
Unpack $Repo $script:sha $stage "the port (with engine\ and base\)"
# the development repository's own files never go out (tools\publish_export.ps1 leaves them out of the public one)
foreach ($p in "PLAN.md", "research", "patches\toolkit", "tools\publish_export.ps1") {
    if (Test-Path -LiteralPath (Join-Path $stage $p)) { throw "$p is in $($script:sha): make the zip from the public repository's commit (-Repo <its clone>)" }
}
# upstream ps3recomp's fw_spu\ (decrypted PS3 firmware modules and code lifted from them) left the engine
# before v0.4.1 (nothing in this port's build read it); a release never carries firmware
if (Test-Path -LiteralPath (Join-Path $stage "engine\fw_spu")) { throw "engine\fw_spu (PS3 firmware modules) is in $($script:sha): not for a release" }
Copy-Item -LiteralPath (Resolve-Path $SetupExe).Path (Join-Path $stage "Setup.exe")
# the same lines Setup.exe writes after its own download (its update compares them with GitHub)
[System.IO.File]::WriteAllText((Join-Path $stage "port-version.txt"),
    "Release v$Version ($name.zip)`r`nSamiEnazi/infamous2-port $($script:sha)`r`n")

# never a game file, firmware or key (the repository holds none; this makes sure)
$game = '\.(elf|self|sprx|prx|pkg|rap|edat|sdat|pup|iso)$|\.psarc|(^|\\)(EBOOT\.BIN|PARAM\.SFO|TROPHY\.TRP|TROPUSR\.DAT|TROPCONF\.SFM)$|(^|\\)TROP_?\d*\.SFM$|(^|\\)recomp_achievements[^\\]*\.json$'
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
# Every entry dated 26 hours ago (GitHub #21/#22): a ZIP's times have no time zone, so this PC's local times came
# out hours in the FUTURE west of it, and ninja then re-ran CMake until it gave up ("build.ninja still dirty").
$stamp = [DateTimeOffset]::Now.AddHours(-26)
try {
    foreach ($f in Get-ChildItem -LiteralPath $stage -Recurse -File -Force) {
        $rel = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        $f.LastWriteTime = $stamp.LocalDateTime     # the staged copy (an entry takes its file's time)
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
foreach ($f in "engine/CMakeLists.txt", "base/infamous2-lle/CMakeLists.txt") { if ($names -notcontains $f) { throw "$f is not in $zip" } }
"  $($names.Count) files, $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, at the top: $($top -join ', ')"
"  sha256 $((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower())"
$zip
