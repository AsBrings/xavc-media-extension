# hkcu-register.ps1 - register the decoder MFT for the CURRENT USER only (no admin).
# Writes the same keys MFTRegister would, under HKCU\Software\Classes, plus the COM
# InprocServer32 entry. Remove with -Remove. (ASCII-only)
#
#   powershell -ExecutionPolicy Bypass -File tools\experiments\hkcu-register.ps1 [-Dll <path>] [-Remove]

param(
    [string]$Dll = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\out\xavc_decoder.dll'),
    [string]$Clsid = '0b1e6d2a-7c3f-4a58-9e21-6f4d8c0a3b71',
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$cat  = 'd6c02d4b-6833-45b4-971a-05a4b04bab91'           # MFT_CATEGORY_VIDEO_DECODER
$root = 'HKCU:\Software\Classes'
$tKey = "$root\MediaFoundation\Transforms\$Clsid"
$cKey = "$root\MediaFoundation\Transforms\Categories\$cat\$Clsid"
$com  = "$root\CLSID\{$($Clsid.ToUpper())}"

if ($Remove) {
    foreach ($k in $cKey, $tKey, $com) { if (Test-Path $k) { Remove-Item $k -Recurse -Force; Write-Host "removed $k" } }
    exit 0
}

function Fcc([uint32]$f) { (New-Object Guid ([int]$f, [int16]0, [int16]0x10, [byte[]](0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71))).ToByteArray() }
$vids = (New-Object Guid '73646976-0000-0010-8000-00aa00389b71').ToByteArray()  # MFMediaType_Video
$in   = $vids + (Fcc 0x34363248)                                                     # H264
$out  = $vids + (Fcc 0x3231564e) + $vids + (Fcc 0x30313050) + $vids + (Fcc 0x30313259) # NV12 P010 Y210

New-Item -Path $tKey -Force | Out-Null
Set-ItemProperty -Path $tKey -Name '(default)' -Value 'XAVC H.264 4:2:2 Decoder (diagnostic, per-user)'
New-ItemProperty -Path $tKey -Name 'InputTypes'  -PropertyType Binary -Value ([byte[]]$in)  -Force | Out-Null
New-ItemProperty -Path $tKey -Name 'OutputTypes' -PropertyType Binary -Value ([byte[]]$out) -Force | Out-Null
New-ItemProperty -Path $tKey -Name 'MFTFlags'    -PropertyType DWord  -Value 1 -Force | Out-Null
New-Item -Path $cKey -Force | Out-Null
New-Item -Path "$com\InprocServer32" -Force | Out-Null
Set-ItemProperty -Path $com -Name '(default)' -Value 'XAVC H.264 4:2:2 Decoder (diagnostic, per-user)'
Set-ItemProperty -Path "$com\InprocServer32" -Name '(default)' -Value $Dll
New-ItemProperty -Path "$com\InprocServer32" -Name 'ThreadingModel' -Value 'Both' -Force | Out-Null
Write-Host "registered per-user: $Clsid -> $Dll"
