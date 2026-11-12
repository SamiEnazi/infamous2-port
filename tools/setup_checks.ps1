# Setup.exe's checks: Get-SetupChecks of tools\setup_common.ps1 as JSON on stdout, in UTF-8.
#   tools\setup_checks.ps1 -Disc <folder holding PS3_GAME> -Rpcs3 <RPCS3 folder>
param([string]$Disc, [string]$Rpcs3)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
. (Join-Path $PSScriptRoot "setup_common.ps1")
Get-SetupChecks -Disc $Disc -Rpcs3 $Rpcs3 | ConvertTo-Json -Depth 3
