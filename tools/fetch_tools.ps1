# The build tools, downloaded into tools-cache\ (portable: no installer, no admin, no PATH changes).
# Setup deletes tools-cache\ after a successful build (tools\setup_cleanup.ps1).
#   1. tools-cache\python   CPython 3.12.16 (astral-sh/python-build-standalone), unpacked with
#                           Windows' own tar.exe                                        ~47 MB
#   2. tools\fetch_tools.py with that Python: CMake, Ninja, LLVM, and Microsoft's MSVC + Windows SDK
#      (those two only with -AcceptMsvcLicense: https://go.microsoft.com/fwlink/?LinkId=2179911)
#   tools\fetch_tools.ps1 [-AcceptMsvcLicense]
param([switch]$AcceptMsvcLicense)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "setup_common.ps1")
$root = Split-Path -Parent $PSScriptRoot
$cache = Join-Path $root "tools-cache"
$dl = Join-Path $cache "downloads"
New-Item -ItemType Directory -Force $dl | Out-Null

$py = Get-PortTool python
if (-not $py) {
    $name = "cpython-3.12.16+20261026-x86_64-pc-windows-msvc-install_only.tar.gz"
    $tgz = Join-Path $dl $name
    Save-Url "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/$($name.Replace('+', '%2B'))" `
        $tgz "Python 3.12" "590DFCBE3A34C07301E6332E13E2188B98655CD072B08AFA45A7998DC4C22BC5" 47113364
    $tar = Join-Path $env:WINDIR "System32\tar.exe"
    if (-not (Test-Path -LiteralPath $tar)) { throw "Windows' tar.exe is missing (it comes with Windows 10 1803 and newer)" }
    $dir = Join-Path $cache "python"
    if (Test-Path -LiteralPath $dir) { cmd /c rd /s /q "$dir" | Out-Null }
    "  unpacking Python 3.12"
    & $tar -xzf $tgz -C $cache                 # the archive's top folder is python\
    if ($LASTEXITCODE -or -not (Test-Path -LiteralPath (Join-Path $dir "python.exe"))) { throw "unpacking Python failed (tar exit code $LASTEXITCODE)" }
    Remove-Item -LiteralPath $tgz
    Set-Content -LiteralPath (Join-Path $dir ".ready") -Value "CPython 3.12.16"
    $py = Join-Path $dir "python.exe"
    "  Python 3.12: tools-cache\python"
}

# Python writes progress and notes; run it under "Continue" so a line on stderr is not a failure
$pyArgs = @("-I", (Join-Path $PSScriptRoot "fetch_tools.py"))
if ($AcceptMsvcLicense) { $pyArgs += "--accept-msvc-license" }
$eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
$why = $null
try { & $py @pyArgs 2>&1 | ForEach-Object { $s = "$_"; if ($s.Trim() -and $s -notmatch '^\s*progress:') { $why = $s.Trim() }; $s } }
finally { $ErrorActionPreference = $eap }
if ($LASTEXITCODE) { throw "getting the build tools failed: $why" }
