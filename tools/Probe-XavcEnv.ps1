<#
    Probe-XavcEnv.ps1  --  XAVC Media Extension 环境/素材探测脚本

    作用：只读地收集做解码器插件需要的信息，并把你的 XAVC 文件规格解析出来。
    不修改系统、不写注册表、不联网。可以放心运行。

    用法（PowerShell，普通权限即可）：
        powershell -ExecutionPolicy Bypass -File .\Probe-XavcEnv.ps1 -Sample "D:\DCIM\xxxx.MP4"

    -Sample 可给一个或多个你的索尼视频文件（S / S-I / HS 各来一个最好）。
    把屏幕上 ===COPY START=== 到 ===COPY END=== 之间的全部内容贴回来给我即可。
#>

[CmdletBinding()]
param(
    [string[]]$Sample
)

$ErrorActionPreference = 'Continue'
$out = [System.Collections.Generic.List[string]]::new()
function W([string]$s) { $out.Add($s); Write-Host $s }

W "===COPY START==="
W "XAVC-PROBE v1  $(Get-Date -Format s)"

# ---------- OS ----------
try {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    W ("OS         : {0} build {1}.{2} ({3})" -f $cv.ProductName, $cv.CurrentBuild, $cv.UBR, $cv.DisplayVersion)
} catch { W "OS         : <read failed> $_" }

# ---------- CPU ----------
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    W ("CPU        : {0} | {1}C/{2}T" -f $cpu.Name.Trim(), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors)
} catch { W "CPU        : <read failed> $_" }

# ---------- GPU ----------
try {
    foreach ($g in Get-CimInstance Win32_VideoController) {
        W ("GPU        : {0} | driver {1} | {2} MB" -f $g.Name, $g.DriverVersion, [int]($g.AdapterRAM/1MB))
    }
} catch { W "GPU        : <read failed> $_" }

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
    W ("  AllowAllTrustedApps            = {0}" -f ($ax.AllowAllTrustedApps))
    W ("  AllowDevelopmentWithoutDevLic. = {0}" -f ($ax.AllowDevelopmentWithoutDevLicense))
} catch { W "  <AppModelUnlock not set>" }

# ---------- hardware MFT hint from registry ----------
W "--- registered hardware MFTs (name hints) ---"
try {
    $hw = Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows Media Foundation\HardwareMFT' -ErrorAction SilentlyContinue
    if ($hw) { W ("  HardwareMFT key present, {0} enabled entries" -f $hw.Count) } else { W "  (no HardwareMFT key)" }
} catch { W "  <read failed>" }

# ================= sample file analysis =================
function Get-ShellProp($folderObj, $fileObj, [string]$name) {
    for ($i = 0; $i -lt 400; $i++) {
        $h = $folderObj.GetDetailsOf($null, $i)
        if ($h -eq $name) { return $folderObj.GetDetailsOf($fileObj, $i) }
    }
    return $null
}

# 在文件前若干字节里找 4CC box，取其后的编解码配置
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
        } catch { W "  <shell props failed> $_" }

        # 读前 8MB 找 avcC(H.264) / hvcC(HEVC)
        try {
            $fs = [System.IO.File]::OpenRead($s)
            $len = [Math]::Min(8MB, $fs.Length)
            $buf = New-Object byte[] $len
            [void]$fs.Read($buf, 0, $len)
            $fs.Close()

            $av = Find-CodecBox $buf 'avcC'
            if ($av -ge 0) {
                $prof = $buf[$av + 5]      # configurationVersion(+4), AVCProfileIndication(+5)
                $lvl  = $buf[$av + 7]
                $pn = $h264Profiles[[int]$prof]; if (-not $pn) { $pn = "profile_idc=$prof" }
                W ("  H.264 avcC : profile_idc={0} ({1}), level={2}" -f $prof, $pn, $lvl)
                if ($prof -eq 122 -or $prof -eq 110 -or $prof -eq 244) { W "  >>> 这是内置解码器不支持的高级 profile（需要我们的解码器）" }
            }
            $hv = Find-CodecBox $buf 'hvcC'
            if ($hv -ge 0) { W "  HEVC hvcC : present (XAVC HS / H.265)" }
            if ($av -lt 0 -and $hv -lt 0) { W "  <未在前8MB找到 avcC/hvcC，可能是 MXF 或 moov 在文件尾部>" }
        } catch { W "  <codec box scan failed> $_" }
    }
}

W "===COPY END==="
W ""
W "完成。请把 ===COPY START=== 到 ===COPY END=== 之间的内容整段复制回来。"
