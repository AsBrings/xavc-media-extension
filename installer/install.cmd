@echo off
rem Installs the XAVC S / S-I decoder (asks for administrator rights once).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-XavcDecoder.ps1" %*
