# Install-XavcDecoder.ps1 - 安装 XAVC S / S-I 解码插件 / installs the XAVC S / S-I decoder.
#
# 双击 install.cmd 即可。Windows 会请求一次管理员权限（安装到 Program Files 并注册组件），
# 然后为当前用户启用（不需要管理员权限）。
#
#   -LogLevel <0|1|2>   写诊断日志到 %ProgramData%\XavcDecoder\xavc.log（默认 0 = 关闭）
#   -NoAllUsers         只为当前用户启用（默认：本机其他用户下次登录时自动启用）
#   -ResetThumbnails    清空当前用户的缩略图缓存（会重启资源管理器），让已有的 XAVC 缩略图重新生成
#   -NoPause            结束时不等待回车
#   -Machine            （内部使用）以管理员身份执行的机器部分

param(
    [int]$LogLevel = 0,
    [switch]$NoAllUsers,
    [switch]$ResetThumbnails,
    [switch]$NoPause,
    [switch]$Machine
)
$ErrorActionPreference = 'Stop'

$Product    = 'XavcDecoder'
$Version    = '0.1.0'
$DestDir    = Join-Path $env:ProgramFiles $Product
$DllName    = 'xavc_decoder.dll'
$SrcDir     = $PSScriptRoot
$BinDir     = if (Test-Path (Join-Path $SrcDir "bin\$DllName")) { Join-Path $SrcDir 'bin' } else { $SrcDir }
$UninstKey  = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$Product"
$ActiveKey  = 'SOFTWARE\Microsoft\Active Setup\Installed Components\{7F9A3C2F-5E44-4B0A-9C1D-2A6B8E3D4F10}'

function Say([string]$zh, [string]$en) { Write-Host "$zh  /  $en" }
function Finish([int]$code) {
    if (-not $NoPause -and -not $Machine) { Read-Host '按回车退出 / Press Enter to exit' | Out-Null }
    exit $code
}
function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

Add-Type -Namespace XavcSetup -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool MoveFileEx(string existing, string newName, int flags);
'@

# Copy a file over one that may be loaded by a running program (Media Player, Explorer's
# thumbnail host, ...): a loaded DLL cannot be overwritten, but it can be renamed; the old
# copy is deleted at the next restart.
function Copy-Replacing([string]$from, [string]$to) {
    try {
        Copy-Item $from $to -Force
    } catch {
        $old = "$to.old-$([DateTime]::Now.ToString('yyyyMMddHHmmss'))"
        Move-Item $to $old -Force
        [void][XavcSetup.Native]::MoveFileEx($old, [NullString]::Value, 4)   # MOVEFILE_DELAY_UNTIL_REBOOT
        Copy-Item $from $to -Force
        $script:NeedsRestartNote = $true
    }
}

# ------------------------------------------------------------------ machine part (Administrator)
function Install-Machine {
    if (-not [Environment]::Is64BitOperatingSystem) { throw '需要 64 位 Windows / 64-bit Windows required' }
    if ([Environment]::OSVersion.Version.Major -lt 10) { throw '需要 Windows 10 或 11 / Windows 10 or 11 required' }
    $files = @(Get-ChildItem $BinDir -Filter *.dll | Where-Object { $_.Name -match '^(xavc_decoder|(avcodec|avutil|swresample)(-xavc)?-\d+)\.dll$' })
    if (-not ($files | Where-Object Name -eq $DllName)) { throw "$DllName not found in $BinDir" }

    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    foreach ($f in $files) { Copy-Replacing $f.FullName (Join-Path $DestDir $f.Name) }
    # FFmpeg DLLs of an earlier version (e.g. a differently named build) are no longer used.
    $keep = @($files | ForEach-Object { $_.Name })
    Get-ChildItem $DestDir -Filter *.dll | Where-Object { $_.Name -match '^(avcodec|avutil|swresample)' -and $keep -notcontains $_.Name } |
        ForEach-Object {
            try { Remove-Item $_.FullName -Force }
            catch { [void][XavcSetup.Native]::MoveFileEx($_.FullName, [NullString]::Value, 4); $script:NeedsRestartNote = $true }
        }
    foreach ($n in 'Uninstall-XavcDecoder.ps1', 'uninstall.cmd', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt') {
        $p = Join-Path $SrcDir $n
        if (Test-Path $p) { Copy-Item $p $DestDir -Force }
    }
    $ff = Join-Path $SrcDir 'licenses'
    if (Test-Path $ff) { Copy-Item $ff $DestDir -Recurse -Force }

    $dll = Join-Path $DestDir $DllName
    $p = Start-Process regsvr32.exe -ArgumentList "/s `"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed (exit $($p.ExitCode))" }

    $cfg = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("SOFTWARE\$Product")
    $cfg.SetValue('Version', $Version)
    $cfg.SetValue('LogLevel', $LogLevel, [Microsoft.Win32.RegistryValueKind]::DWord)
    $cfg.Close()
    if ($LogLevel -gt 0) {
        # sandboxed hosts (Media Player) must be able to append to the log
        $logDir = Join-Path $env:ProgramData $Product
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
        $acl = Get-Acl $logDir
        foreach ($sid in 'S-1-5-32-545', 'S-1-15-2-1') {   # Users, ALL APPLICATION PACKAGES
            $id = New-Object Security.Principal.SecurityIdentifier $sid
            $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($id, 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
        }
        Set-Acl $logDir $acl
    }

    # Settings -> Apps
    $size = [int](($files | Measure-Object Length -Sum).Sum / 1KB)
    $u = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($UninstKey)
    $u.SetValue('DisplayName', 'XAVC S / S-I Decoder (Media Foundation)')
    $u.SetValue('DisplayVersion', $Version)
    $u.SetValue('Publisher', 'xavc-media-extension')
    $u.SetValue('InstallLocation', $DestDir)
    $u.SetValue('UninstallString', "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$DestDir\Uninstall-XavcDecoder.ps1`"")
    $u.SetValue('NoModify', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $u.SetValue('NoRepair', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $u.SetValue('EstimatedSize', $size, [Microsoft.Win32.RegistryValueKind]::DWord)
    $u.Close()

    # Other users of this PC: enable it for them at their next sign-in (Active Setup).
    if (-not $NoAllUsers) {
        $a = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($ActiveKey)
        $a.SetValue('', 'XAVC S / S-I Decoder')
        $a.SetValue('StubPath', "regsvr32.exe /s /n /i:user `"$dll`"")
        $a.SetValue('Version', ($Version -replace '\.', ',') + ',0')
        $a.SetValue('IsInstalled', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
        $a.Close()
    }
}

if ($Machine) {
    try { Install-Machine } catch { Write-Host "ERROR: $_"; exit 1 }
    if ($script:NeedsRestartNote) { exit 10 }   # tells the caller to mention the restart
    exit 0
}

# ------------------------------------------------------------------ main (as the signed-in user)
try {
    Say '正在安装 XAVC S / S-I 解码插件 ...' 'Installing the XAVC S / S-I decoder ...'
    if (Test-Admin) {
        Install-Machine
    } else {
        Say '需要管理员权限来安装到 Program Files，请在弹出的窗口中确认。' 'Administrator rights are needed for Program Files; please confirm the prompt.'
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Machine', '-LogLevel', "$LogLevel")
        if ($NoAllUsers) { $argv += '-NoAllUsers' }
        try { $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argv -Wait -PassThru }
        catch { Say '已取消。' 'Cancelled.'; Finish 2 }
        if ($p.ExitCode -eq 10) { $script:NeedsRestartNote = $true }
        elseif ($p.ExitCode -ne 0) { throw "machine part failed (exit $($p.ExitCode))" }
    }

    # Per-user part: Media Foundation reads these settings from the user's own registry hive.
    $dll = Join-Path $DestDir $DllName
    $p = Start-Process regsvr32.exe -ArgumentList "/s /n /i:user `"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "per-user registration failed (exit $($p.ExitCode))" }

    if ($ResetThumbnails) {
        Say '正在清空缩略图缓存（资源管理器会重启）...' 'Clearing the thumbnail cache (Explorer restarts) ...'
        Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Get-ChildItem "$env:LOCALAPPDATA\Microsoft\Windows\Explorer" -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
        Start-Process explorer.exe
    }

    Say '安装完成。用「媒体播放器」打开 XAVC S / S-I 视频即可播放；正在运行的媒体播放器需要先关闭再打开。' `
        'Done. Open XAVC S / S-I videos in Media Player (restart it if it is running).'
    if ($script:NeedsRestartNote) {
        Say '有旧版本文件正在被使用，重启电脑后会自动清理。' 'Some old files were in use; they are removed at the next restart.'
    }
    Finish 0
} catch {
    Write-Host "ERROR: $_"
    Say '安装失败。' 'Installation failed.'
    Finish 1
}
