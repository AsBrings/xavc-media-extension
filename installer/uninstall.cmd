@echo off
rem Removes the XAVC S / S-I decoder.
rem Always 64-bit PowerShell, also when started from a 32-bit program.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-XavcDecoder.ps1" %*
