# Run in PowerShell as Administrator, from this folder.
$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "FamilyLocationServer.exe"
if (-not (Test-Path $exe)) {
    Write-Error "FamilyLocationServer.exe was not found next to this script."
}
$existing = Get-NetFirewallRule -DisplayName "Allow LocationServer 8000" -ErrorAction SilentlyContinue
if ($existing) {
    Remove-NetFirewallRule -DisplayName "Allow LocationServer 8000"
}
New-NetFirewallRule -DisplayName "Allow LocationServer 8000" -Direction Inbound -Protocol TCP -LocalPort 8000 -Program $exe -Action Allow -Profile Private | Out-Null
Write-Host "Port 8000 is allowed for FamilyLocationServer.exe on Private networks only."
