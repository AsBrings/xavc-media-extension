<#
    Probe-XavcEnv.ps1  --  XAVC Media Extension environment / sample probe

    Read-only. Does NOT modify the system, registry, or network.
    Collects hardware / OS info, installed media codec extensions, and parses
    your Sony XAVC files (resolution / fps / bitrate + H.264 avcC profile).

    NOTE: This file is intentionally ASCII-only so Windows PowerShell 5.1 can
    read it regardless of system locale.

    Usage (PowerShell, normal user is fine):
        powershell -ExecutionPolicy Bypass -File .\Probe-XavcEnv.ps1 -Sample "C:\path\clip.MP4"

    -Sample takes one or more of your Sony files (S / S-I / HS each is ideal).
    Copy everything between ===COPY START=== and ===COPY END=== back to me.
#>

[CmdletBinding()]
param(
    [string[]]$Sample
)

$ErrorActionPreference = 'Continue'
function W([string]$s) { Write-Host $s }

W "===COPY START==="
W ("XAVC-PROBE v2  {0}" -f (Get-Date -Format s))

# ---------- OS ----------
try {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    W ("OS         : {0} build {1}.{2} ({3})" -f $cv.ProductName, $cv.CurrentBuild, $cv.UBR, $cv.DisplayVersion)
} catch { W "OS         : <read failed>" }

# ---------- CPU ----------
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    W ("CPU        : {0} | {1}C/{2}T" -f $cpu.Name.Trim(), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors)
} catch { W "CPU        : <read failed>" }

# ---------- GPU ----------
try {
    foreach ($g in Get-CimInstance Win32_VideoController) {
        W ("GPU        : {0} | driver {1}" -f $g.Name, $g.DriverVersion)
    }
} catch { W "GPU        : <read failed>" }

# ---------- Media codec extensions installed ----------
W "--- installed media extensions ---"
$pkgs = 'Microsoft.HEVCVideoExtension','Microsoft.HEVCVideoExtensions','Microsoft.AV1VideoExtension',
        'Microsoft.VP9VideoExtensions','Microsoft.MPEG2VideoExtension','Microsoft.RawImageExtension',
        'Microsoft.WebMediaExtensions'
foreach ($p in $pkgs) {
    try {
        $a = Get-AppxPackage -Name $p -ErrorAction SilentlyContinue
        if ($a) { W ("  {0}  {1}" -f $a.Name, $a.Version) } else { W ("  {0}  <not installed>" -f $p) }
    } catch { W ("  {0}  <query failed>" -f $p) }
}

# ---------- Media Player app ----------
try {
    $mp = Get-AppxPackage -Name 'Microsoft.ZuneMusic' -ErrorAction SilentlyContinue
    if ($mp) { W ("MediaPlayer: Microsoft.ZuneMusic {0}" -f $mp.Version) } else { W "MediaPlayer: <ZuneMusic not found>" }
} catch { W "MediaPlayer: <query failed>" }

# ---------- sideload / dev mode state ----------
W "--- sideload / dev mode ---"
try {
    $ax = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue
    W ("  AllowAllTrustedApps             = {0}" -f ($ax.AllowAllTrustedApps))
    W ("  AllowDevelopmentWithoutDevLicense = {0}" -f ($ax.AllowDevelopmentWithoutDevLicense))
} catch { W "  <AppModelUnlock not set>" }

# ---------- hardware MFT hint from registry ----------
W "--- hardware MFT registry hint ---"
try {
    $hw = Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows Media Foundation\HardwareMFT' -ErrorAction SilentlyContinue
    if ($hw) { W ("  HardwareMFT key present ({0} entries)" -f $hw.Count) } else { W "  (no HardwareMFT key)" }
} catch { W "  <read failed>" }

# ================= sample file analysis =================
function Get-ShellProp($folderObj, $fileObj, [string]$name) {
    for ($i = 0; $i -lt 400; $i++) {
        $h = $folderObj.GetDetailsOf($null, $i)
        if ($h -eq $name) { return $folderObj.GetDetailsOf($fileObj, $i) }
    }
    return $null
}

function Find-CodecBox([byte[]]$buf, [string]$tag) {
    $t = [System.Text.Encoding]::ASCII.GetBytes($tag)
    for ($i = 0; $i -le $buf.Length - ($t.Length + 8); $i++) {
        if ($buf[$i] -eq $t[0] -and $buf[$i+1] -eq $t[1] -and $buf[$i+2] -eq $t[2] -and $buf[$i+3] -eq $t[3]) {
            return $i
        }
    }
    return -1
}

$h264Profiles = @{ 66='Baseline'; 77='Main'; 88='Extended'; 100='High'; 110='High 10';
                   122='High 4:2:2'; 244='High 4:4:4 Predictive'; 44='CAVLC 4:4:4' }

if ($Sample) {
    $shell = New-Object -ComObject Shell.Application
    foreach ($s in $Sample) {
        W "--- sample: $s ---"
        if (-not (Test-Path $s)) { W "  <file not found>"; continue }
        $fi = Get-Item $s
        W ("  size       : {0:N1} MB" -f ($fi.Length/1MB))

        try {
            $dir  = $shell.Namespace($fi.DirectoryName)
            $item = $dir.ParseName($fi.Name)
            foreach ($prop in 'Frame width','Frame height','Frame rate','Total bitrate','Data rate','Video compression','Length') {
                $v = Get-ShellProp $dir $item $prop
                if ($v) { W ("  {0,-18}: {1}" -f $prop, $v) }
            }
        } catch { W "  <shell props failed>" }

        # read up to first 8MB, then tail, to find avcC(H.264) / hvcC(HEVC)
        try {
            $fs = [System.IO.File]::OpenRead($s)
            $cfgLen = 0; $tag = ''; $prof = 0; $lvl = 0
            function Scan([System.IO.FileStream]$fs, [long]$pos, [int]$len) {
                $fs.Position = $pos
                $b = New-Object byte[] $len
                [void]$fs.Read($b, 0, $len)
                return $b
            }
            $buf = Scan $fs 0 ([Math]::Min(8MB, $fs.Length))
            $av = Find-CodecBox $buf 'avcC'
            $hv = Find-CodecBox $buf 'hvcC'
            if ($av -lt 0 -and $hv -lt 0 -and $fs.Length -gt 8MB) {
                # moov likely at tail
                $tlen = [int][Math]::Min(8MB, $fs.Length)
                $buf = Scan $fs ($fs.Length - $tlen) $tlen
                $av = Find-CodecBox $buf 'avcC'
                $hv = Find-CodecBox $buf 'hvcC'
            }
            $fs.Close()

            if ($av -ge 0) {
                $prof = $buf[$av + 5]
                $lvl  = $buf[$av + 7]
                $pn = $h264Profiles[[int]$prof]; if (-not $pn) { $pn = "profile_idc=$prof" }
                W ("  H.264 avcC : profile_idc={0} ({1}), level={2}" -f $prof, $pn, $lvl)
                if ($prof -eq 122 -or $prof -eq 110 -or $prof -eq 244) {
                    W "  >>> advanced profile the built-in decoder cannot handle (needs our decoder)"
                }
            }
            if ($hv -ge 0) { W "  HEVC hvcC  : present (XAVC HS / H.265)" }
            if ($av -lt 0 -and $hv -lt 0) { W "  <no avcC/hvcC found; MXF or unusual layout>" }
        } catch { W "  <codec box scan failed>" }
    }
}

W "===COPY END==="
W ""
W "Done. Copy everything between ===COPY START=== and ===COPY END=== back to me."
