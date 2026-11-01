# bsh-probe.ps1 - install / remove the byte-stream-handler probe (ASCII-only).
#
# Verified on Windows 11 26300 (Media Player 11.2609): with this configuration the new
# Media Player, Explorer thumbnails and IMFSourceReader route Sony XAVC S / S-I (H.264
# High 4:2:2) through our decoder, while ordinary MP4 files keep using the in-box path.
#
# Machine part (Administrator):
#   - %ProgramFiles%\XavcDecoder\xavc_decoder.dll, COM classes for the decoder and the
#     byte stream handler, and the decoder MFT registered for the private 'XAVC' subtype
#     only. Do NOT give it a merit: CMFTransformActivate::InstantiateTransform verifies a
#     registered merit with MFGetMFTMerit (certificate-backed) and drops the instance if
#     that fails.
# Per-user part (no protected keys are touched):
#   - HKCU\Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\{.mp4,video/mp4}
#     adds our handler to the candidate list (mfplat reads HKCU before HKLM),
#   - HKCU\Software\Classes\MediaFoundation\MediaSources\Preferred\{.mp4,video/mp4} makes it
#     the preferred handler (HKCR merges per value, so only these two entries change); the
#     preferred list also counts as "approved" under MF_PLUGIN_CONTROL_POLICY_USE_APPROVED_PLUGINS,
#   - HKCU\Software\Classes\MediaFoundation\Transforms\Preferred\{XAVC subtype} -> decoder,
#     so the decoder is approved too (nothing else decodes that private subtype),
#   - ALL APPLICATION PACKAGES read access on the per-user Media Foundation key, so the
#     Media Player's AppContainer can read the handler registration.
#
#   powershell -ExecutionPolicy Bypass -File tools\experiments\bsh-probe.ps1 -Install
#   powershell -ExecutionPolicy Bypass -File tools\experiments\bsh-probe.ps1 -Remove

param(
    [switch]$Install,
    [switch]$Remove,
    [int]$LogLevel = 1,
    [string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)
$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated (Administrator) PowerShell.'
}

$bshClsid  = '{7F9A3C24-5E44-4B0A-9C1D-2A6B8E3D4F10}'
$decClsid  = '{0B1E6D2A-7C3F-4A58-9E21-6F4D8C0A3B71}'
$xavcSub   = '{43564158-0000-0010-8000-00AA00389B71}'
$wmfSub    = 'Software\Microsoft\Windows Media Foundation'
$bshSub    = "$wmfSub\ByteStreamHandlers"
$srcPref   = 'Software\Classes\MediaFoundation\MediaSources\Preferred'
$mftPref   = 'Software\Classes\MediaFoundation\Transforms\Preferred'
$selectors = @('.mp4', 'video/mp4')
$dest      = Join-Path $env:ProgramFiles 'XavcDecoder'
$dll       = Join-Path $dest 'xavc_decoder.dll'
$HKCU      = [Microsoft.Win32.Registry]::CurrentUser   # .NET API: '/' in MIME names is not a path separator

function Remove-UserValue([string]$sub, [string]$name, [string]$onlyIf) {
    $k = $HKCU.OpenSubKey($sub, $true)
    if (-not $k) { return }
    if ($k.GetValue($name) -eq $onlyIf) { $k.DeleteValue($name, $false); Write-Host "removed HKCU\$sub : $name" }
    $k.Close()
}

function Remove-UserConfig {
    foreach ($s in $selectors) {
        Remove-UserValue $srcPref $s $bshClsid
        $k = $HKCU.OpenSubKey("$bshSub\$s", $true)
        if ($k) {
            $k.DeleteValue($bshClsid, $false)
            $empty = ($k.ValueCount -eq 0 -and $k.SubKeyCount -eq 0)
            $k.Close()
            if ($empty) { $HKCU.OpenSubKey($bshSub, $true).DeleteSubKey($s, $false) }
            Write-Host "removed HKCU\$bshSub\$s"
        }
    }
    Remove-UserValue $mftPref $xavcSub $decClsid
}

function Unregister-Installed {
    if (Test-Path $dll) {
        $p = Start-Process regsvr32 -ArgumentList "/s /u `"$dll`"" -Wait -PassThru
        Write-Host "regsvr32 /u (installed DLL) exit=$($p.ExitCode)"
    }
}

if ($Remove) {
    Remove-UserConfig
    Unregister-Installed
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
    [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree('SOFTWARE\XavcDecoder', $false)
    Write-Host 'probe removed'
    exit 0
}
if (-not $Install) { throw 'Specify -Install or -Remove.' }

# 1. earlier diagnostic registrations
Unregister-Installed                                   # whatever build is installed knows its own CLSIDs
& (Join-Path $PSScriptRoot 'hkcu-register.ps1') -Remove | Out-Null
foreach ($t in '7f9a3c21-5e44-4b0a-9c1d-2a6b8e3d4f10') {
    if (Test-Path "HKLM:\SOFTWARE\Classes\MediaFoundation\Transforms\$t") { Write-Host "WARNING: stale MFT registration $t still present" }
}

# 2. machine part: install + register (no merit)
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item (Join-Path $Repo 'build\out\xavc_decoder.dll') $dll -Force
foreach ($d in 'avcodec-*.dll', 'avutil-*.dll', 'swresample-*.dll') {   # FFmpeg runtime (loaded on demand)
    Copy-Item (Join-Path $Repo "build\out\$d") $dest -Force
}
$cfg = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\XavcDecoder')
$cfg.SetValue('LogLevel', $LogLevel, [Microsoft.Win32.RegistryValueKind]::DWord)
$cfg.Close()
$p = Start-Process regsvr32 -ArgumentList "/s `"$dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed (exit $($p.ExitCode))" }
Write-Host "registered $dll"

# 3. per-user part
foreach ($s in $selectors) {
    $k = $HKCU.CreateSubKey("$bshSub\$s"); $k.SetValue($bshClsid, 'XAVC Byte Stream Handler'); $k.Close()
    $k = $HKCU.CreateSubKey($srcPref);     $k.SetValue($s, $bshClsid);                         $k.Close()
    Write-Host "handler registered + preferred for $s"
}
$k = $HKCU.CreateSubKey($mftPref); $k.SetValue($xavcSub, $decClsid); $k.Close()
Write-Host "decoder preferred for XAVC subtype"

$wmfPath = "HKCU:\$wmfSub"
$acl = Get-Acl $wmfPath
if (-not ($acl.Access | Where-Object { $_.IdentityReference -match 'ALL APPLICATION PACKAGES' })) {
    $sid  = New-Object Security.Principal.SecurityIdentifier 'S-1-15-2-1'
    $rule = New-Object Security.AccessControl.RegistryAccessRule($sid, 'ReadKey', 'ContainerInherit', 'None', 'Allow')
    $acl.AddAccessRule($rule)
    Set-Acl -Path $wmfPath -AclObject $acl
    Write-Host 'granted ALL APPLICATION PACKAGES read on the per-user Media Foundation key'
}
Write-Host 'probe installed'
