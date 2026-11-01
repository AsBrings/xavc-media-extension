# package.ps1 - assemble the distributable folder + zip from build\out (ASCII-only).
#
#   powershell -ExecutionPolicy Bypass -File build\package.ps1 -FFmpeg <FFmpeg shared build dir> [-Version 0.1.0]
#
# Layout:
#   XavcDecoder-<ver>\install.cmd, uninstall.cmd, Install-/Uninstall-XavcDecoder.ps1,
#   README.txt, LICENSE.txt, THIRD_PARTY_NOTICES.txt, licenses\FFmpeg-LICENSE.txt,
#   bin\xavc_decoder.dll + the FFmpeg runtime DLLs it loads.

param(
    [string]$FFmpeg = $env:XAVC_FFMPEG,
    [string]$Version = '0.1.0'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out  = Join-Path $PSScriptRoot 'out'
$pkg  = Join-Path $PSScriptRoot "package\XavcDecoder-$Version"
if (-not $FFmpeg) { throw 'Pass -FFmpeg <dir> (for its LICENSE file).' }

Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'bin'), (Join-Path $pkg 'licenses') | Out-Null

foreach ($d in 'xavc_decoder.dll', 'avcodec*.dll', 'avutil*.dll') {
    $f = @(Get-ChildItem (Join-Path $out $d))
    if (-not $f) { throw "missing build output: $d" }
    $f | Copy-Item -Destination (Join-Path $pkg 'bin')
}
Get-ChildItem (Join-Path $out 'swresample*.dll') -ErrorAction SilentlyContinue | Copy-Item -Destination (Join-Path $pkg 'bin')
foreach ($f in 'install.cmd', 'uninstall.cmd', 'Install-XavcDecoder.ps1', 'Uninstall-XavcDecoder.ps1', 'README.txt', 'THIRD_PARTY_NOTICES.txt') {
    Copy-Item (Join-Path $root "installer\$f") $pkg
}
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $pkg 'LICENSE.txt')
# FFmpeg licence: our minimal build (build\ffmpeg-minimal.sh) is LGPL 2.1+; a full BtbN build is LGPL 3.
$lic = @(@{ f = 'COPYING.LGPLv2.1'; to = 'FFmpeg-COPYING.LGPLv2.1.txt' }, @{ f = 'LICENSE.md'; to = 'FFmpeg-LICENSE.md' },
         @{ f = 'LICENSE.txt'; to = 'FFmpeg-LICENSE.txt' }, @{ f = 'FFMPEG_COMMIT.txt'; to = 'FFmpeg-source-commit.txt' })
$found = 0
foreach ($l in $lic) {
    $p = Join-Path $FFmpeg $l.f
    if (Test-Path $p) { Copy-Item $p (Join-Path $pkg "licenses\$($l.to)"); $found++ }
}
if (-not $found) { throw "no FFmpeg licence file in $FFmpeg" }
Copy-Item (Join-Path $PSScriptRoot 'ffmpeg-minimal.sh') (Join-Path $pkg 'licenses\ffmpeg-minimal.sh')

$zip = "$pkg.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $pkg -DestinationPath $zip
$size = [Math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "package: $pkg"
Write-Host "zip:     $zip ($size MB)"
