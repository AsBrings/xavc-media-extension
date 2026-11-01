# Uninstall-XavcDecoder.ps1 - 卸载 XAVC S / S-I 解码插件 / removes the XAVC S / S-I decoder.
#
# 先撤销当前用户的设置（恢复安装前的值），再以管理员身份删除程序文件和组件注册。
# 本机其他用户残留的个人设置是无害的：组件删除后 Media Foundation 会自动改用系统自带的处理方式。
#
#   -NoPause   结束时不等待回车
#   -Machine   （内部使用）以管理员身份执行的机器部分

param(
    [switch]$NoPause,
    [switch]$Machine
)
$ErrorActionPreference = 'Stop'

$Product   = 'XavcDecoder'
$DestDir   = Join-Path $env:ProgramFiles $Product
$Dll       = Join-Path $DestDir 'xavc_decoder.dll'
$UninstKey = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$Product"
$ActiveKey = 'SOFTWARE\Microsoft\Active Setup\Installed Components\{7F9A3C2F-5E44-4B0A-9C1D-2A6B8E3D4F10}'

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

function Uninstall-Machine {
    if (Test-Path $Dll) {
        $p = Start-Process regsvr32.exe -ArgumentList "/s /u `"$Dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Host "regsvr32 /u exit $($p.ExitCode)" }
    }
    $hklm = [Microsoft.Win32.Registry]::LocalMachine
    $hklm.DeleteSubKeyTree($ActiveKey, $false)
    $hklm.DeleteSubKeyTree($UninstKey, $false)
    $hklm.DeleteSubKeyTree("SOFTWARE\$Product", $false)
    if (Test-Path $DestDir) {
        # Files still loaded by a running program are deleted at the next restart.
        foreach ($f in Get-ChildItem $DestDir -Recurse -File) {
            try { Remove-Item $f.FullName -Force }
            catch { [void][XavcSetup.Native]::MoveFileEx($f.FullName, [NullString]::Value, 4); $script:NeedsRestartNote = $true }
        }
        Get-ChildItem $DestDir -Recurse -Directory | Sort-Object FullName -Descending |
            ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        Remove-Item $DestDir -Force -ErrorAction SilentlyContinue
        if (Test-Path $DestDir) { [void][XavcSetup.Native]::MoveFileEx($DestDir, [NullString]::Value, 4) }
    }
    Remove-Item (Join-Path $env:ProgramData $Product) -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Machine) {
    try { Uninstall-Machine } catch { Write-Host "ERROR: $_"; exit 1 }
    if ($script:NeedsRestartNote) { exit 10 }
    exit 0
}

try {
    Say '正在卸载 XAVC S / S-I 解码插件 ...' 'Removing the XAVC S / S-I decoder ...'
    # Per-user part first (needs the DLL, which the machine part deletes).
    if (Test-Path $Dll) {
        $p = Start-Process regsvr32.exe -ArgumentList "/s /u /n /i:user `"$Dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Host "per-user removal exit $($p.ExitCode)" }
    }
    if (Test-Admin) {
        Uninstall-Machine
    } else {
        Say '需要管理员权限，请在弹出的窗口中确认。' 'Administrator rights are needed; please confirm the prompt.'
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Machine')
        try { $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argv -Wait -PassThru }
        catch { Say '已取消（当前用户的设置已撤销）。' 'Cancelled (your per-user settings were removed).'; Finish 2 }
        if ($p.ExitCode -eq 10) { $script:NeedsRestartNote = $true }
        elseif ($p.ExitCode -ne 0) { throw "machine part failed (exit $($p.ExitCode))" }
    }
    Say '卸载完成。' 'Removed.'
    if ($script:NeedsRestartNote) {
        Say '部分文件正在被使用，重启电脑后会自动删除。' 'Some files were in use; they are deleted at the next restart.'
    }
    Finish 0
} catch {
    Write-Host "ERROR: $_"
    Say '卸载失败。' 'Uninstall failed.'
    Finish 1
}
