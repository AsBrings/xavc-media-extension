# build-win.ps1 - build xavc_decoder.dll natively on Windows with MinGW-w64 g++.
#
#   powershell -ExecutionPolicy Bypass -File build\build-win.ps1 -MinGW E:\path\to\mingw64
#
# -MinGW defaults to $env:XAVC_MINGW, then to the g++ found on PATH.
# NOTE: GNU ld cannot open paths containing non-ASCII characters (e.g. a Chinese
# user name in C:\Users\...). If the toolchain lives under such a path, create an
# ASCII junction to it and pass that instead:
#   New-Item -ItemType Junction -Path E:\tools\mingw64 -Target <real mingw64 dir>
# gcc resolves the junction back to the real path for its built-in search dirs, so
# the link step also passes explicit -B/-L dirs under -MinGW.

param(
    [string]$MinGW = $env:XAVC_MINGW,
    # Optional: an FFmpeg "shared" dev build (include\, lib\, bin\) to also build tools\avtest.
    [string]$FFmpeg = $env:XAVC_FFMPEG
)
$ErrorActionPreference = 'Stop'

if (-not $MinGW) {
    $gpp = (Get-Command x86_64-w64-mingw32-g++ -ErrorAction SilentlyContinue).Source
    if (-not $gpp) { throw "MinGW-w64 g++ not found. Pass -MinGW <dir>." }
    $MinGW = Split-Path (Split-Path $gpp)
}
if ($MinGW -match '[^\x00-\x7F]') {
    throw "Toolchain path contains non-ASCII characters ($MinGW); ld will fail. Use an ASCII junction (see header)."
}
$cxx = Join-Path $MinGW 'bin\x86_64-w64-mingw32-g++.exe'
if (-not (Test-Path $cxx)) { throw "g++ not found at $cxx" }

$tc     = $MinGW -replace '\\', '/'
$gccVer = (Get-ChildItem (Join-Path $MinGW 'lib\gcc\x86_64-w64-mingw32') -Directory | Select-Object -First 1).Name
$searchDirs = @("-B$tc/lib/gcc/x86_64-w64-mingw32/$gccVer/", "-B$tc/x86_64-w64-mingw32/lib/",
                "-L$tc/lib/gcc/x86_64-w64-mingw32/$gccVer", "-L$tc/x86_64-w64-mingw32/lib", "-L$tc/lib")

$root = Split-Path $PSScriptRoot
$src  = Join-Path $root 'src'
$out  = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
# gcc/as write intermediates to %TEMP%; keep them on an ASCII path too.
$tmp = Join-Path $out 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp

$flags = @('-O2', '-fno-exceptions', '-fno-rtti', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00',
           '-Wall', '-Wno-misleading-indentation')

function Invoke-Step([string]$name, [string[]]$argv) {
    Write-Host $name
    & $cxx @argv
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
}

if (-not $FFmpeg) { throw 'Pass -FFmpeg <FFmpeg shared build dir> (headers for the decoder, DLLs to ship).' }
$ffInc = Join-Path $FFmpeg 'include'
if (-not (Test-Path (Join-Path $ffInc 'libavcodec\avcodec.h'))) { throw "FFmpeg headers not found under $ffInc" }

# The decoder loads avcodec/avutil at run time (LoadLibraryEx from its own directory), so
# only the headers are needed here; the matching DLLs are copied next to it below.
$units = @(
    @{ src = 'guids.cpp';        obj = 'guids.o';   extra = @() },
    @{ src = 'xavc_util.cpp';    obj = 'util.o';    extra = @() },
    @{ src = 'xavc_h264.cpp';    obj = 'h264.o';    extra = @() },
    @{ src = 'xavc_convert.cpp'; obj = 'convert.o'; extra = @('-O3') },
    @{ src = 'xavc_ffmpeg.cpp';  obj = 'ffmpeg.o';  extra = @("-I$ffInc") },
    @{ src = 'xavc_decoder.cpp'; obj = 'decoder.o'; extra = @("-I$ffInc") },
    @{ src = 'xavc_bsh.cpp';     obj = 'bsh.o';     extra = @() },
    @{ src = 'xavc_dll.cpp';     obj = 'dll.o';     extra = @("-I$ffInc") }
)
$objs = @()
$n = $units.Count + 1
$i = 0
foreach ($u in $units) {
    $i++
    Invoke-Step "[$i/$n] compile $($u.src)" ($flags + $u.extra + @('-c', "$src\$($u.src)", '-o', "$out\$($u.obj)"))
    $objs += "$out\$($u.obj)"
}
Invoke-Step "[$n/$n] link xavc_decoder.dll" (@('-shared', '-o', "$out\xavc_decoder.dll") + $objs +
    @("$src\xavc.def") + $searchDirs + @('-static-libgcc', '-static-libstdc++',
    '-lmfplat', '-lmfuuid', '-lole32', '-loleaut32', '-luuid'))
# Host processes only find system DLLs: refuse a build that imports MinGW runtime DLLs
# (e.g. libwinpthread-1.dll, pulled in by thread_local / emutls).
$objdump = Join-Path $MinGW 'bin\objdump.exe'
if (Test-Path $objdump) {
    $imports = & $objdump -p "$out\xavc_decoder.dll" | Select-String 'DLL Name: (.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
    $bad = $imports | Where-Object { $_ -match '^(lib|av|sw)' }
    if ($bad) { throw "xavc_decoder.dll must not import: $($bad -join ', ')" }
    Write-Host "imports: $($imports -join ', ')"
}
# FFmpeg runtime the decoder loads (avcodec also imports swresample).
foreach ($d in 'avcodec-*.dll', 'avutil-*.dll', 'swresample-*.dll') {
    Copy-Item (Join-Path $FFmpeg "bin\$d") $out -Force
}

Write-Host "OK -> $out\xavc_decoder.dll"

# Test harness (enumeration / Source Reader decode / thumbnail / app activation)
Invoke-Step '[harness] mftest.exe' (@('-O2', '-municode', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00',
    '-Wall', '-Wno-misleading-indentation', '-o', "$out\mftest.exe", "$root\tools\mftest\mftest.cpp") +
    $searchDirs + @('-static-libgcc', '-static-libstdc++',
    '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-loleaut32', '-luuid', '-lshell32', '-lgdi32', '-luser32',
    '-ld3d11', '-ldxgi'))
Write-Host "OK -> $out\mftest.exe"

# Reference decoder for tests (libavformat demuxing + libavcodec), linked normally.
Invoke-Step '[test] avtest.exe' (@('-O2', '-x', 'c', "-I$ffInc", '-o', "$out\avtest.exe",
    "$root\tools\avtest\avtest.c", '-x', 'none', "-L$FFmpeg\lib") + $searchDirs + @(
    '-static-libgcc', '-static-libstdc++', '-lavformat', '-lavcodec', '-lavutil', '-lswscale'))
Copy-Item (Join-Path $FFmpeg 'bin\*.dll') $out -Force
Write-Host "OK -> $out\avtest.exe"
