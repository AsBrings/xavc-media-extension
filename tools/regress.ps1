# regress.ps1 - frame-exact regression test of the installed decoder (ASCII-only).
#
# Decodes through Media Foundation (mftest decode: Source Reader + the installed XAVC
# decoder) and compares every frame's luma hash and time with plain libavcodec
# (build\out\ref\avtest.exe, built with -FFmpegRef).
#
#   powershell -ExecutionPolicy Bypass -File tools\regress.ps1 -SI <XAVC S-I clip> -LongGop <XAVC S clip>
#
# Checks: all four output formats on the S-I clip, B-frame reordering on the Long GOP clip,
# decoding after a seek, and the last frames of the S-I clip (drain at end of stream).
# The Windows MP4 source ignores edit lists, so Long GOP times are compared after removing
# the constant offset seen on the first frame.

param(
    [Parameter(Mandatory = $true)] [string]$SI,
    [Parameter(Mandatory = $true)] [string]$LongGop,
    [int]$Frames = 30
)
$ErrorActionPreference = 'Stop'
$repo   = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out    = Join-Path $repo 'build\out'
$mftest = Join-Path $out 'mftest.exe'
$avtest = Join-Path $out 'ref\avtest.exe'
$tmp    = Join-Path $out 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$script:failures = 0

function Get-Reference([string]$file, [int]$n) {
    $cache = Join-Path $tmp ("ref_{0}_{1}.txt" -f [IO.Path]::GetFileNameWithoutExtension($file), $n)
    if (-not (Test-Path $cache)) {
        if (-not (Test-Path $avtest)) { throw "reference decoder missing: build with -FFmpegRef" }
        # (FFmpeg warnings on stderr must not count as errors in Windows PowerShell 5.1)
        $ErrorActionPreference = 'Continue'
        & $avtest $file hash $n 2>$null | Set-Content $cache
        $ErrorActionPreference = 'Stop'
    }
    $list = New-Object System.Collections.ArrayList
    foreach ($l in Get-Content $cache) {
        if ($l -match 't=(-?\d+) h10=(\w+) h8=(\w+)') {
            [void]$list.Add([pscustomobject]@{ t = [long]$matches[1]; h10 = $matches[2]; h8 = $matches[3] })
        }
    }
    return ,$list
}

function Get-Decoded([string[]]$argv) {
    $list = New-Object System.Collections.ArrayList
    foreach ($l in (& $mftest @argv)) {
        if ($l -match '^\s*\d+ t=(-?\d+) dur=\d+ (h10|h8)=(\w+)') {
            [void]$list.Add([pscustomobject]@{ t = [long]$matches[1]; kind = $matches[2]; h = $matches[3] })
        }
    }
    return ,$list
}

# Match decoded frames to reference frames by time (after removing a constant offset).
function Compare-Frames([string]$name, $ref, $dec, [bool]$alignOffset) {
    if ($dec.Count -eq 0) { Write-Host "FAIL $name : no frames decoded"; $script:failures++; return }
    $offset = 0
    if ($alignOffset) {
        $first = $ref | Where-Object { $_.h10 -eq $dec[0].h -or $_.h8 -eq $dec[0].h } | Select-Object -First 1
        if ($first) { $offset = $dec[0].t - $first.t }
    }
    $byTime = @{}
    foreach ($r in $ref) { $byTime[[long][Math]::Round(($r.t + $offset) / 100.0)] = $r }
    $ok = 0; $bad = 0; $missing = 0; $firstBad = ''
    foreach ($d in $dec) {
        $key = [long][Math]::Round($d.t / 100.0)
        $r = $byTime[$key]
        if ($null -eq $r) { $r = $byTime[$key - 1] }
        if ($null -eq $r) { $r = $byTime[$key + 1] }
        if ($null -eq $r) { $missing++; continue }
        $want = if ($d.kind -eq 'h10') { $r.h10 } else { $r.h8 }
        if ($want -eq $d.h) { $ok++ } else { $bad++; if (-not $firstBad) { $firstBad = "t=$($d.t)" } }
    }
    $status = if ($bad -eq 0 -and $missing -eq 0) { 'PASS' } else { $script:failures++; 'FAIL' }
    Write-Host ("{0} {1,-28} {2,4} frames: {3} match, {4} differ, {5} not in reference {6}" -f $status, $name, $dec.Count, $ok, $bad, $missing, $firstBad)
}

$refSI = Get-Reference $SI 100000
foreach ($fmt in 'P010', 'Y210', 'NV12', 'YUY2') {
    Compare-Frames "S-I $fmt" $refSI (Get-Decoded @('decode', $SI, $fmt, "$Frames")) $false
}
$lastT = $refSI[$refSI.Count - 1].t
$tail = Get-Decoded @('decode', $SI, 'Y210', '1000', '-', ('{0:F3}' -f ($lastT / 1e7 - 1.5)))
Compare-Frames 'S-I end of stream (Y210)' $refSI $tail $false
if ($tail.Count -and [Math]::Abs($tail[$tail.Count - 1].t - $lastT) -gt 2) {
    Write-Host "FAIL last frame is t=$($tail[$tail.Count - 1].t), expected t=$lastT"; $script:failures++
}

$refLG = Get-Reference $LongGop 760
Compare-Frames 'Long GOP P010' $refLG (Get-Decoded @('decode', $LongGop, 'P010', '90')) $true
Compare-Frames 'Long GOP seek 10.5 s (Y210)' $refLG (Get-Decoded @('decode', $LongGop, 'Y210', '60', '-', '10.5')) $true

if ($script:failures) { Write-Host "$script:failures check(s) FAILED"; exit 1 }
Write-Host 'all checks passed'
