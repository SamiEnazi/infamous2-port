# Setup.exe's checks: Get-SetupChecks of tools\setup_common.ps1 as JSON on stdout, in UTF-8.
#   tools\setup_checks.ps1 -Disc <game folder | .iso | .pkg> [-Rpcs3 <RPCS3 folder>] [-DiscKey <file>] [-Rap <file>]
param([string]$Disc, [string]$Rpcs3, [string]$DiscKey, [string]$Rap)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
. (Join-Path $PSScriptRoot "setup_common.ps1")
Get-SetupChecks -Disc $Disc -Rpcs3 $Rpcs3 -DiscKey $DiscKey -Rap $Rap | ConvertTo-Json -Depth 3
