<#
.SYNOPSIS
  Remove the PebbleUtaProxy Windows Service and its firewall rule.
.EXAMPLE
  .\uninstall-service.ps1
#>
[CmdletBinding()]
param(
  [string] $ServiceName = 'PebbleUtaProxy',
  [int]    $Port = 8080
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
      ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated PowerShell.'
}

if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
  Write-Host "Stopping and deleting '$ServiceName'..."
  & sc.exe stop $ServiceName | Out-Null
  Start-Sleep -Seconds 2
  & sc.exe delete $ServiceName | Out-Null
} else {
  Write-Host "Service '$ServiceName' not installed."
}

$ruleName = "PebbleUtaProxy TCP $Port"
Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue |
  Remove-NetFirewallRule
Write-Host "Done."
