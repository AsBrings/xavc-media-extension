# merit-matrix.ps1 - does a self-assigned MFT_CODEC_MERIT_Attribute move our decoder
# ahead of the Preferred in-box H.264 decoder? (ASCII-only; self-elevates once)
#
# For each merit value: re-register the installed DLL via DllInstall("merit=N"),
# then record MFTEnumEx order, a Source Reader decode of the 4:2:2 clip and of the
# 4:2:0 control clip, and an Explorer thumbnail of a fresh copy of the 4:2:2 clip.
# Ends by re-registering with no merit (the normal state).
#
#   powershell -ExecutionPolicy Bypass -File tools\experiments\merit-matrix.ps1

param(
    [string]$Repo     = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$TestData = 'E:\TQN_Projects\xavc-testdata',
    # ';'-separated: "-File" mode cannot pass arrays, and "0,8,1000" parses as 81000.
    [string]$Merits   = '0;8;1000'
)
$ErrorActionPreference = 'Continue'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
              '-Repo', "`"$Repo`"", '-TestData', "`"$TestData`"", '-Merits', "`"$Merits`"")
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argv -Wait -PassThru
    exit $p.ExitCode
}

$out  = Join-Path $TestData 'merit_matrix.txt'
$m    = Join-Path $Repo 'build\out\mftest.exe'
$dll  = Join-Path $env:ProgramFiles 'XavcDecoder\xavc_decoder.dll'
$si   = Join-Path $TestData 'si_4k24p_422_10bit.mp4'
$ctl  = Join-Path $TestData 's_4k60p_420_8bit_control.mp4'

"merit matrix  $(Get-Date -Format s)" | Out-File $out -Encoding ascii
Copy-Item (Join-Path $Repo 'build\out\xavc_decoder.dll') $dll -Force
"installed DLL: $((Get-Item $dll).LastWriteTime)" | Out-File $out -Append -Encoding ascii

function Register([int]$merit) {
    $p = Start-Process regsvr32 -ArgumentList "/s /n /i:`"merit=$merit`" `"$dll`"" -Wait -PassThru
    "regsvr32 /i:merit=$merit exit=$($p.ExitCode)" | Out-File $out -Append -Encoding ascii
}

foreach ($merit in ($Merits -split ';' | ForEach-Object { [int]$_ })) {
    "`n==================== merit=$merit ====================" | Out-File $out -Append -Encoding ascii
    Register $merit
    reg query "HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\7f9a3c21-5e44-4b0a-9c1d-2a6b8e3d4f10" | Out-File $out -Append -Encoding ascii
    "--- enum ---" | Out-File $out -Append -Encoding ascii
    & $m enum 2>&1 | Select-Object -First 4 | Out-File $out -Append -Encoding ascii
    "--- read 4:2:2 S-I ---" | Out-File $out -Append -Encoding ascii
    & $m read $si 2>&1 | Select-String 'pid=|SetCurrentMediaType|ReadSample|transform|decoded' | Out-File $out -Append -Encoding ascii
    "--- read 4:2:0 control ---" | Out-File $out -Append -Encoding ascii
    & $m read $ctl 2>&1 | Select-String 'pid=|SetCurrentMediaType|ReadSample #0|transform|decoded' | Out-File $out -Append -Encoding ascii
    "--- thumbnail (fresh copy) ---" | Out-File $out -Append -Encoding ascii
    $copy = Join-Path $TestData "si_thumb_m$merit.mp4"
    Copy-Item $si $copy -Force
    & $m thumb $copy (Join-Path $TestData "thumb_m$merit.bmp") 2>&1 | Out-File $out -Append -Encoding ascii
}

"`n==================== restore: merit=0 ====================" | Out-File $out -Append -Encoding ascii
Register 0
"done $(Get-Date -Format s)" | Out-File $out -Append -Encoding ascii
