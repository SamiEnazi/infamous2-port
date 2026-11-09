# Where this PC keeps what the game needs (the PowerShell side of tools\port_paths.py).
#   . "$PSScriptRoot\port_paths.ps1";  Get-PortPath rpcs3
# Order: environment variable (PORT_RPCS3 ...), then config.local.json in the project
# folder (written by Setup.cmd, not in git).
function Get-PortPath([string]$Key, [switch]$Optional) {
    $envName = @{ rpcs3 = "PORT_RPCS3"; vcvars = "PORT_VCVARS"; ninja = "PORT_NINJA"; llvm = "PORT_LLVM" }[$Key]
    $v = if ($envName) { [Environment]::GetEnvironmentVariable($envName) } else { $null }
    if (-not $v) {
        $cfg = Join-Path (Split-Path -Parent $PSScriptRoot) "config.local.json"
        if (Test-Path $cfg) { $v = (Get-Content $cfg -Raw | ConvertFrom-Json).$Key }
    }
    if (-not $v -and -not $Optional) {
        throw "no '$Key' path set: run Setup.cmd (or set $envName, or `"$Key`" in config.local.json)"
    }
    $v
}
