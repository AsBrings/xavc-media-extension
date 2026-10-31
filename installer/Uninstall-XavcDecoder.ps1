# Uninstall-XavcDecoder.ps1  (ASCII-only)
# Unregisters and removes the diagnostic XAVC decoder MFT. Self-elevates.

$ErrorActionPreference = 'SilentlyContinue'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    exit
}

$dest = Join-Path $env:ProgramFiles 'XavcDecoder'
$dll  = Join-Path $dest 'xavc_decoder.dll'
if (Test-Path $dll) {
    Write-Host "Unregistering $dll ..."
    Start-Process regsvr32 -ArgumentList "/u /s `"$dll`"" -Wait
}
Remove-Item -Recurse -Force $dest
Remove-Item -Recurse -Force (Join-Path $env:ProgramData 'XavcDecoder')
Write-Host "Removed. (A reboot or restart of Media Player clears any cached state.)"
Read-Host "Enter to exit"
