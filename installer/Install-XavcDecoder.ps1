# Install-XavcDecoder.ps1  (ASCII-only; run by double-click or from PowerShell)
# Installs and registers the DIAGNOSTIC XAVC H.264 4:2:2 decoder MFT system-wide.
# Self-elevates to Administrator. Safe to uninstall with Uninstall-XavcDecoder.ps1.

$ErrorActionPreference = 'Stop'

# ---- self-elevate ----
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Elevating to Administrator..."
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    exit
}

$src = Join-Path $PSScriptRoot 'xavc_decoder.dll'
if (-not (Test-Path $src)) { Write-Host "ERROR: xavc_decoder.dll not found next to this script."; Read-Host "Enter to exit"; exit 1 }

$dest = Join-Path $env:ProgramFiles 'XavcDecoder'
$log  = Join-Path $env:ProgramData 'XavcDecoder'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
New-Item -ItemType Directory -Force -Path $log  | Out-Null
Copy-Item $src (Join-Path $dest 'xavc_decoder.dll') -Force

# Let the log folder be writable from inside the Media Player sandbox (AppContainer)
# so the MFT can record whether it was loaded.
cmd /c "icacls `"$log`" /grant *S-1-15-2-1:(OI)(CI)M"  | Out-Null   # ALL APPLICATION PACKAGES
cmd /c "icacls `"$log`" /grant *S-1-15-2-2:(OI)(CI)M"  | Out-Null   # ALL RESTRICTED APPLICATION PACKAGES
cmd /c "icacls `"$log`" /grant *S-1-1-0:(OI)(CI)M"     | Out-Null   # Everyone
Remove-Item (Join-Path $log 'xavc.log') -ErrorAction SilentlyContinue

$dll = Join-Path $dest 'xavc_decoder.dll'
Write-Host "Registering $dll ..."
$p = Start-Process regsvr32 -ArgumentList "/s `"$dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { Write-Host "regsvr32 FAILED (exit $($p.ExitCode))."; Read-Host "Enter to exit"; exit 1 }

Write-Host ""
Write-Host "Installed and registered OK."
Write-Host "DLL : $dll"
Write-Host "Log : $log\xavc.log"
Write-Host ""
Write-Host "NOW TEST:"
Write-Host "  1. Open an XAVC S / S-I file in the Media Player app."
Write-Host "  2. If you see a solid MAGENTA frame -> the player loaded our decoder (success)."
Write-Host "     If you still get 'unsupported' / black -> it does not use third-party decoders."
Write-Host "  3. Either way, send me the contents of:  $log\xavc.log"
Write-Host ""
Read-Host "Enter to exit"
