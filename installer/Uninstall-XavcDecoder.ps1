# Uninstall-XavcDecoder.ps1  (ASCII-only)
# Unregisters and removes the diagnostic XAVC decoder MFT. Self-elevates.
#
#   -NoPause   Do not wait for Enter at the end (for scripted runs).

param([switch]$NoPause)
$ErrorActionPreference = 'SilentlyContinue'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($NoPause) { $argv += '-NoPause' }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argv -Wait -PassThru
    exit $p.ExitCode
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
if (-not $NoPause) { Read-Host "Enter to exit" | Out-Null }
