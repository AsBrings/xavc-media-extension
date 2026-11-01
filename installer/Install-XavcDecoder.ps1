# Install-XavcDecoder.ps1 - 安装 XAVC S / S-I 解码插件 / installs the XAVC S / S-I decoder.
#
# 双击 install.cmd 即可。Windows 会请求一次管理员权限（安装到 Program Files 并注册组件），
# 然后为当前用户启用（不需要管理员权限）。
#
#   -LogLevel <0|1|2>   写诊断日志到 %ProgramData%\XavcDecoder\xavc.log（默认 0 = 关闭）
#   -NoAllUsers         只为当前用户启用（默认：本机其他用户下次登录时自动启用）
#   -ResetThumbnails    清空当前用户的缩略图缓存（会重启资源管理器），让已有的 XAVC 缩略图重新生成
#   -NoPause            结束时不等待回车
#   -Machine, -ErrorLog （内部使用）以管理员身份执行的机器部分

param(
    [int]$LogLevel = 0,
    [switch]$NoAllUsers,
    [switch]$ResetThumbnails,
    [switch]$NoPause,
    [switch]$Machine,
    [string]$ErrorLog = ''
)
$ErrorActionPreference = 'Stop'

# A 32-bit host (e.g. a 32-bit file manager) starts 32-bit PowerShell, which would install to
# Program Files (x86) and the WOW6432Node registry view: run again as 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $ps64 = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    foreach ($k in $PSBoundParameters.Keys) {
        $v = $PSBoundParameters[$k]
        if ($v -is [System.Management.Automation.SwitchParameter]) { if ($v.IsPresent) { $argList += "-$k" } }
        else { $argList += "-$k"; $argList += "$v" }
    }
    & $ps64 @argList
    exit $LASTEXITCODE
}

$Product    = 'XavcDecoder'
$Version    = '0.1.0'
$ProgFiles  = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$DestDir    = Join-Path $ProgFiles $Product
$DllName    = 'xavc_decoder.dll'
$SrcDir     = $PSScriptRoot
$BinDir     = if (Test-Path -LiteralPath (Join-Path $SrcDir "bin\$DllName")) { Join-Path $SrcDir 'bin' } else { $SrcDir }
$UninstKey  = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$Product"
$ActiveKey  = 'SOFTWARE\Microsoft\Active Setup\Installed Components\{7F9A3C2F-5E44-4B0A-9C1D-2A6B8E3D4F10}'
$PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$Regsvr32   = Join-Path $env:SystemRoot 'System32\regsvr32.exe'

function Say([string]$zh, [string]$en) { Write-Host "$zh  /  $en" }
function Finish([int]$code) {
    if (-not $NoPause -and -not $Machine) { Read-Host '按回车退出 / Press Enter to exit' | Out-Null }
    exit $code
}
function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Get-OsArchitecture {
    try { return [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() } catch { }
    if ($env:PROCESSOR_ARCHITEW6432) { return $env:PROCESSOR_ARCHITEW6432 }
    return $env:PROCESSOR_ARCHITECTURE
}
# SID of the user signed in to this session (owner of its explorer.exe), $null if unknown.
function Get-SessionUserSid {
    try {
        $session = (Get-Process -Id $PID).SessionId
        $ex = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe' AND SessionId=$session")
        if ($ex.Count) { return (Invoke-CimMethod -InputObject $ex[0] -MethodName GetOwnerSid).Sid }
    } catch { }
    return $null
}

Add-Type -Namespace XavcSetup -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool MoveFileEx(string existing, string newName, int flags);
'@

# Copy over a file that may be loaded by a running program (Media Player, Explorer's thumbnail
# host, ...): a loaded DLL cannot be overwritten but can be renamed; the old copy is deleted at
# the next restart. If the new copy cannot be written either, the old file is put back.
function Copy-Replacing([string]$from, [string]$to) {
    try {
        Copy-Item -LiteralPath $from -Destination $to -Force
        return
    } catch {
        if (-not (Test-Path -LiteralPath $to)) { throw }
    }
    $old = "$to.old-$([DateTime]::Now.ToString('yyyyMMddHHmmssfff'))"
    Move-Item -LiteralPath $to -Destination $old -Force
    try {
        Copy-Item -LiteralPath $from -Destination $to -Force
    } catch {
        Move-Item -LiteralPath $old -Destination $to -Force
        throw
    }
    [void][XavcSetup.Native]::MoveFileEx($old, [NullString]::Value, 4)   # MOVEFILE_DELAY_UNTIL_REBOOT
    $script:NeedsRestartNote = $true
}

function Assert-Supported {
    if (-not [Environment]::Is64BitOperatingSystem) { throw '需要 64 位 Windows / 64-bit Windows required' }
    if ([Environment]::OSVersion.Version.Major -lt 10) { throw '需要 Windows 10 或 11 / Windows 10 or 11 required' }
    $arch = Get-OsArchitecture
    if ($arch -notin @('X64', 'AMD64')) { throw "不支持此处理器架构（$arch），需要 x64 / Unsupported architecture $arch (x64 required)" }
}

# ------------------------------------------------------------------ machine part (Administrator)
function Install-Machine {
    Assert-Supported
    $files = @(Get-ChildItem -LiteralPath $BinDir -Filter *.dll |
               Where-Object { $_.Name -match '^(xavc_decoder|(avcodec|avutil|swresample)(-xavc)?-\d+)\.dll$' })
    if (-not ($files | Where-Object Name -eq $DllName)) { throw "$DllName not found in $BinDir" }
    $dll = Join-Path $DestDir $DllName
    $fresh = -not (Test-Path -LiteralPath $dll)

    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    foreach ($f in $files) { Copy-Replacing $f.FullName (Join-Path $DestDir $f.Name) }
    # FFmpeg DLLs of an earlier version (e.g. a differently named build) are no longer used.
    $keep = @($files | ForEach-Object { $_.Name })
    Get-ChildItem -LiteralPath $DestDir -Filter *.dll |
        Where-Object { $_.Name -match '^(avcodec|avutil|swresample)' -and $keep -notcontains $_.Name } |
        ForEach-Object {
            try { Remove-Item -LiteralPath $_.FullName -Force }
            catch { [void][XavcSetup.Native]::MoveFileEx($_.FullName, [NullString]::Value, 4); $script:NeedsRestartNote = $true }
        }
    foreach ($n in 'Uninstall-XavcDecoder.ps1', 'uninstall.cmd', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt') {
        $p = Join-Path $SrcDir $n
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $DestDir -Force }
    }
    $lic = Join-Path $SrcDir 'licenses'
    if (Test-Path -LiteralPath $lic) { Copy-Item -LiteralPath $lic -Destination $DestDir -Recurse -Force }

    $p = Start-Process $Regsvr32 -ArgumentList "/s `"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        if ($fresh) { Remove-Item -LiteralPath $DestDir -Recurse -Force -ErrorAction SilentlyContinue }
        throw "regsvr32 failed (exit $($p.ExitCode))"
    }

    $cfg = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("SOFTWARE\$Product")
    $cfg.SetValue('Version', $Version)
    $cfg.SetValue('LogLevel', $LogLevel, [Microsoft.Win32.RegistryValueKind]::DWord)
    $cfg.Close()
    if ($LogLevel -gt 0) {
        # sandboxed hosts (Media Player) must be able to append to the log
        $logDir = Join-Path $env:ProgramData $Product
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
        $acl = Get-Acl -LiteralPath $logDir
        foreach ($sid in 'S-1-5-32-545', 'S-1-15-2-1') {   # Users, ALL APPLICATION PACKAGES
            $id = New-Object Security.Principal.SecurityIdentifier $sid
            $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($id, 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
        }
        Set-Acl -LiteralPath $logDir -AclObject $acl
    }

    # Settings -> Apps
    $size = [int](($files | Measure-Object Length -Sum).Sum / 1KB)
    $u = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($UninstKey)
    $u.SetValue('DisplayName', 'XAVC S / S-I Decoder (Media Foundation)')
    $u.SetValue('DisplayVersion', $Version)
    $u.SetValue('Publisher', 'xavc-media-extension')
    $u.SetValue('InstallLocation', $DestDir)
    $u.SetValue('UninstallString', "`"$PowerShell`" -NoProfile -ExecutionPolicy Bypass -File `"$DestDir\Uninstall-XavcDecoder.ps1`"")
    $u.SetValue('NoModify', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $u.SetValue('NoRepair', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $u.SetValue('EstimatedSize', $size, [Microsoft.Win32.RegistryValueKind]::DWord)
    $u.Close()

    # Other users of this PC: enable it for them at their next sign-in (Active Setup).
    if ($NoAllUsers) {
        [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($ActiveKey, $false)
    } else {
        $a = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($ActiveKey)
        $a.SetValue('', 'XAVC S / S-I Decoder')
        $a.SetValue('StubPath', "`"$Regsvr32`" /s /n /i:user `"$dll`"")
        $a.SetValue('Version', (($Version -split '\.') + @('0', '0', '0', '0'))[0..3] -join ',')
        $a.SetValue('IsInstalled', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
        $a.Close()
    }
}

if ($Machine) {
    try { Install-Machine }
    catch {
        # This window closes at once: hand the message to the window that started us.
        if ($ErrorLog) { try { Set-Content -LiteralPath $ErrorLog -Value "$_" -Encoding UTF8 } catch { } }
        Write-Host "ERROR: $_"
        exit 1
    }
    if ($script:NeedsRestartNote) { exit 10 }   # tells the caller to mention the restart
    exit 0
}

# ------------------------------------------------------------------ main (as the signed-in user)
$stage = $null
try {
    Say '正在安装 XAVC S / S-I 解码插件 ...' 'Installing the XAVC S / S-I decoder ...'
    Assert-Supported
    $otherAccount = $false
    if (Test-Admin) {
        # Started with "Run as administrator" as a different account: HKCU would be that account's.
        $sessionUser = Get-SessionUserSid
        $otherAccount = $sessionUser -and $sessionUser -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        Install-Machine
    } else {
        Say '需要管理员权限来安装到 Program Files，请在弹出的窗口中确认。' 'Administrator rights are needed for Program Files; please confirm the prompt.'
        # Run the elevated part from a local copy of the package: an elevated process does not
        # see the user's network or subst drives. (Files a user creates under ProgramData can be
        # read by administrators but not changed by other users.)
        $stage = Join-Path $env:ProgramData ('XavcDecoder-setup-' + [guid]::NewGuid().ToString('N'))
        Copy-Item -LiteralPath $SrcDir -Destination $stage -Recurse
        $script = Join-Path $stage (Split-Path -Leaf $PSCommandPath)
        $errLog = Join-Path $env:ProgramData ('XavcDecoder-setup-' + [guid]::NewGuid().ToString('N') + '.log')
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"", '-Machine',
                  '-LogLevel', "$LogLevel", '-ErrorLog', "`"$errLog`"")
        if ($NoAllUsers) { $argv += '-NoAllUsers' }
        try { $p = Start-Process $PowerShell -Verb RunAs -ArgumentList $argv -Wait -PassThru }
        catch { Say '已取消。' 'Cancelled.'; Finish 2 }
        $detail = ''
        if (Test-Path -LiteralPath $errLog) {
            $detail = (Get-Content -LiteralPath $errLog -Raw -ErrorAction SilentlyContinue)
            Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
        }
        if ($p.ExitCode -eq 10) { $script:NeedsRestartNote = $true }
        elseif ($p.ExitCode -ne 0) { throw "machine part failed (exit $($p.ExitCode)) $detail" }
    }

    if ($otherAccount) {
        Say '你是用另一个管理员账户运行的安装：系统部分已装好，但没有为当前登录的用户启用。' `
            'Installed for the computer, but it was run as another account, so it is not enabled for the signed-in user.'
        if ($NoAllUsers) {
            Say '请直接双击 install.cmd（不要选"以管理员身份运行"）为当前用户启用。' 'Double-click install.cmd (not "Run as administrator") to enable it for your account.'
        } else {
            Say '当前用户注销并重新登录后会自动启用；也可以直接双击 install.cmd 立即启用。' 'It is enabled at your next sign-in, or double-click install.cmd to enable it now.'
        }
        Finish 0
    }

    # Per-user part: Media Foundation reads these settings from the user's own registry hive.
    $dll = Join-Path $DestDir $DllName
    $p = Start-Process $Regsvr32 -ArgumentList "/s /n /i:user `"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "per-user registration failed (exit $($p.ExitCode))" }

    if ($ResetThumbnails -and (Test-Admin)) {
        # Restarting Explorer from an elevated window would give the user an elevated shell.
        Say '缩略图缓存未清空：请不要用"以管理员身份运行"，直接双击 install.cmd -ResetThumbnails。' `
            'Thumbnail cache not cleared: run install.cmd -ResetThumbnails without "Run as administrator".'
    } elseif ($ResetThumbnails) {
        Say '正在清空缩略图缓存（资源管理器会重启）...' 'Clearing the thumbnail cache (Explorer restarts) ...'
        $session = (Get-Process -Id $PID).SessionId
        Get-Process -Name explorer -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $session } |
            Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Get-ChildItem -LiteralPath "$env:LOCALAPPDATA\Microsoft\Windows\Explorer" -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue |
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
} finally {
    if ($stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
