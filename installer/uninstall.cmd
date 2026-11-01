@echo off
rem Removes the XAVC S / S-I decoder.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-XavcDecoder.ps1" %*
