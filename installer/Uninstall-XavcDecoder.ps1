# Uninstall-XavcDecoder.ps1 - 卸载 XAVC S / S-I 解码插件 / removes the XAVC S / S-I decoder.
#
# 先撤销当前用户的设置（恢复安装前的值），再以管理员身份删除程序文件和组件注册。
# 本机其他用户残留的个人设置是无害的：组件删除后 Media Foundation 会自动改用系统自带的处理方式。
#
#   -NoPause            结束时不等待回车
#   -Machine, -ErrorLog （内部使用）以管理员身份执行的机器部分

param(
    [switch]$NoPause,
    [switch]$Machine,
    [string]$ErrorLog = ''
)
$ErrorActionPreference = 'Stop'

# 32-bit PowerShell would look in Program Files (x86) and the WOW6432Node registry view.
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
$ProgFiles  = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$DestDir    = Join-Path $ProgFiles $Product
$Dll        = Join-Path $DestDir 'xavc_decoder.dll'
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

function Uninstall-Machine {
    if (Test-Path -LiteralPath $Dll) {
        $p = Start-Process $Regsvr32 -ArgumentList "/s /u `"$Dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Host "regsvr32 /u exit $($p.ExitCode)" }
    }
    $hklm = [Microsoft.Win32.Registry]::LocalMachine
    $hklm.DeleteSubKeyTree($ActiveKey, $false)
    $hklm.DeleteSubKeyTree($UninstKey, $false)
    $hklm.DeleteSubKeyTree("SOFTWARE\$Product", $false)
    if (Test-Path -LiteralPath $DestDir) {
        # Files still loaded by a running program are deleted at the next restart.
        foreach ($f in Get-ChildItem -LiteralPath $DestDir -Recurse -File) {
            try { Remove-Item -LiteralPath $f.FullName -Force }
            catch { [void][XavcSetup.Native]::MoveFileEx($f.FullName, [NullString]::Value, 4); $script:NeedsRestartNote = $true }
        }
        Get-ChildItem -LiteralPath $DestDir -Recurse -Directory | Sort-Object FullName -Descending |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $DestDir -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $DestDir) { [void][XavcSetup.Native]::MoveFileEx($DestDir, [NullString]::Value, 4) }
    }
    Remove-Item -LiteralPath (Join-Path $env:ProgramData $Product) -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Machine) {
    try { Uninstall-Machine }
    catch {
        if ($ErrorLog) { try { Set-Content -LiteralPath $ErrorLog -Value "$_" -Encoding UTF8 } catch { } }
        Write-Host "ERROR: $_"
        exit 1
    }
    if ($script:NeedsRestartNote) { exit 10 }
    exit 0
}

try {
    Say '正在卸载 XAVC S / S-I 解码插件 ...' 'Removing the XAVC S / S-I decoder ...'
    $otherAccount = $false
    if (Test-Admin) {
        $sessionUser = Get-SessionUserSid
        $otherAccount = $sessionUser -and $sessionUser -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    }
    # Per-user part first (it needs the DLL, which the machine part deletes). When this runs as a
    # different administrator account, the signed-in user's leftover settings are harmless.
    if ((Test-Path -LiteralPath $Dll) -and -not $otherAccount) {
        $p = Start-Process $Regsvr32 -ArgumentList "/s /u /n /i:user `"$Dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Host "per-user removal exit $($p.ExitCode)" }
    }
    if (Test-Admin) {
        Uninstall-Machine
    } else {
        Say '需要管理员权限，请在弹出的窗口中确认。' 'Administrator rights are needed; please confirm the prompt.'
        # Elevate the installed copy (local), not a copy on a network drive the elevated process cannot see.
        $installed = Join-Path $DestDir 'Uninstall-XavcDecoder.ps1'
        $script = if (Test-Path -LiteralPath $installed) { $installed } else { $PSCommandPath }
        $errLog = Join-Path $env:ProgramData ('XavcDecoder-setup-' + [guid]::NewGuid().ToString('N') + '.log')
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"", '-Machine', '-ErrorLog', "`"$errLog`"")
        try { $p = Start-Process $PowerShell -Verb RunAs -ArgumentList $argv -Wait -PassThru }
        catch { Say '已取消（当前用户的设置已撤销）。' 'Cancelled (your per-user settings were removed).'; Finish 2 }
        $detail = ''
        if (Test-Path -LiteralPath $errLog) {
            $detail = (Get-Content -LiteralPath $errLog -Raw -ErrorAction SilentlyContinue)
            Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
        }
        if ($p.ExitCode -eq 10) { $script:NeedsRestartNote = $true }
        elseif ($p.ExitCode -ne 0) { throw "machine part failed (exit $($p.ExitCode)) $detail" }
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
