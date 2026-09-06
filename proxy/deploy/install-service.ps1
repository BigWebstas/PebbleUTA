<#
.SYNOPSIS
  Install PebbleUtaProxy as a Windows Service.

.DESCRIPTION
  Registers the published PebbleUtaProxy.exe as an auto-start service that
  restarts on crash, opens the listen port in Windows Firewall, and starts it.
  Run from an elevated PowerShell.

.EXAMPLE
  .\install-service.ps1 -BinPath 'C:\Services\PebbleUtaProxy' -Port 8080
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string] $BinPath,
  [string] $ServiceName = 'PebbleUtaProxy',
  [string] $DisplayName = 'PebbleUTA Proxy',
  [int]    $Port = 8080,
  # LocalSystem "just works"; switch to 'NT AUTHORITY\LocalService' for least
  # privilege once it is running.
  [string] $Account = 'LocalSystem'
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
      ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated PowerShell.'
}

$exe = Join-Path $BinPath 'PebbleUtaProxy.exe'
if (-not (Test-Path $exe)) {
  throw "PebbleUtaProxy.exe not found in $BinPath. Publish first (see proxy\README.md)."
}

if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
  Write-Host "Removing existing service '$ServiceName'..."
  & sc.exe stop $ServiceName | Out-Null
  Start-Sleep -Seconds 3
  & sc.exe delete $ServiceName | Out-Null
  for ($i = 0; $i -lt 10 -and (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue); $i++) {
    Start-Sleep -Seconds 1
  }
}

# Sanity run: 'selftest' loads the feeds and exits, printing the real error
# (missing runtime, no network, bad config) that Start-Service hides behind
# the generic "cannot start service".
Write-Host "Smoke-testing the binary..."
& $exe selftest 40.76 -111.89 2>&1 | Select-Object -Last 6
if ($LASTEXITCODE -ne 0) {
  throw "The binary itself failed (exit $LASTEXITCODE). Fix that before installing the service. " +
        "Most common: install the ASP.NET Core Runtime 10 (x64) from https://dotnet.microsoft.com/download"
}
Write-Host "(binary OK)`n"

Write-Host "Creating service '$ServiceName' -> $exe"
& sc.exe create $ServiceName binPath= "`"$exe`"" start= auto obj= "$Account" DisplayName= "$DisplayName" | Out-Null
& sc.exe description $ServiceName 'Joins UTA GTFS + realtime into a nearby-departures API for the PebbleUTA watch app.' | Out-Null
# Restart on crash: 5s, 5s, 15s; reset the counter after 1 day.
& sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/15000 | Out-Null

# Listen URL. appsettings.json's "Urls" wins over env vars for this app, so
# set the port there.
$cfgPath = Join-Path $BinPath 'appsettings.json'
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$cfg.Urls = "http://0.0.0.0:$Port"
$cfg | ConvertTo-Json -Depth 10 | Set-Content $cfgPath -Encoding UTF8
Write-Host "appsettings.json Urls -> http://0.0.0.0:$Port"

$ruleName = "PebbleUtaProxy TCP $Port"
if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
  New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Action Allow `
    -Protocol TCP -LocalPort $Port -Profile Any | Out-Null
  Write-Host "Firewall: opened inbound TCP $Port"
}

Write-Host "Starting service..."
try {
  Start-Service -Name $ServiceName -ErrorAction Stop
}
catch {
  $q = & sc.exe query $ServiceName
  Write-Warning "Service registered but did not start. Details:"
  $q | Where-Object { $_ -match 'EXIT_CODE|STATE' }
  Write-Host ""
  Write-Host "Check Windows Logs > Application in Event Viewer for '.NET Runtime' /"
  Write-Host "'Application Error' entries, or run the exe directly to see the error:"
  Write-Host "  $exe"
  throw
}
Start-Sleep -Seconds 3
Get-Service -Name $ServiceName | Format-Table -AutoSize

Write-Host ""
Write-Host "Health check (schedule load takes ~10s on a cold start):"
Write-Host "  curl http://localhost:$Port/healthz"
