# mediaplayer-test.ps1 - open a file in the new Media Player (like a double-click), let it play,
# capture the player window, print this decoder's log lines for the player process, then
# close the player again (only if this script started it).  ASCII-only.
#
#   powershell -ExecutionPolicy Bypass -File tools\experiments\mediaplayer-test.ps1 -File <video> [-Seconds 12] [-Png out.png] [-Keep]

param(
    [Parameter(Mandatory = $true)] [string]$File,
    [int]$Seconds = 12,
    [string]$Png = '',
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
$repo    = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$mftest  = Join-Path $repo 'build\out\mftest.exe'
$log     = Join-Path $env:ProgramData 'XavcDecoder\xavc.log'
$titleRx = (-join ([char[]](0x5a92, 0x4f53, 0x64ad, 0x653e, 0x5668))) + '|Media Player'   # window title (zh-CN / en)

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class XavcWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  // Visible top-level UWP frame window whose title matches the pattern.
  public static IntPtr Find(string pattern) {
    IntPtr found = IntPtr.Zero;
    var rx = new System.Text.RegularExpressions.Regex(pattern);
    EnumWindows((h, l) => {
      var t = new StringBuilder(256); var c = new StringBuilder(256);
      GetWindowText(h, t, 256); GetClassName(h, c, 256);
      if (IsWindowVisible(h) && c.ToString() == "ApplicationFrameWindow" && rx.IsMatch(t.ToString())) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@
Add-Type -AssemblyName System.Drawing

$started = Get-Date
$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
$aumid = "$((Get-AppxPackage Microsoft.ZuneMusic).PackageFamilyName)!Microsoft.ZuneMusic"
$out = & $mftest open $aumid $File
$line = ($out | Select-String 'ActivateForFile').ToString()
$mp = [int]([regex]'pid=(\d+)').Match($line).Groups[1].Value
Write-Host "$line; playing for $Seconds s"
Start-Sleep -Seconds $Seconds

$h = [XavcWin]::Find($titleRx)
if ($h -eq [IntPtr]::Zero) { Write-Host 'Media Player window not found' }
elseif ($Png) {
    $r = New-Object XavcWin+RECT
    [XavcWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [XavcWin]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
    $sw = [Math]::Min(960, $w)
    $sm = New-Object System.Drawing.Bitmap $sw, ([int]($hh * $sw / $w))
    $g2 = [System.Drawing.Graphics]::FromImage($sm); $g2.DrawImage($bmp, 0, 0, $sm.Width, $sm.Height); $g2.Dispose()
    $sm.Save($Png, [System.Drawing.Imaging.ImageFormat]::Png); $sm.Dispose(); $bmp.Dispose()
    Write-Host "window captured -> $Png"
}
$mods = (tasklist /m /fi "PID eq $mp" /fo list 2>$null | Out-String)
$dec = (($mods -split '[,\s]+') | Where-Object { $_ -match '(?i)^(msmpeg2vdec|xavc_decoder|avcodec)' } | Sort-Object -Unique) -join ', '
Write-Host "decoder modules in the player: $dec"
if (Test-Path $log) { Get-Content $log | Select-Object -Skip $before | Where-Object { $_ -match "pid=$mp " } }

if (-not $Keep) {
    $p = Get-Process -Id $mp -ErrorAction SilentlyContinue
    if ($p -and $p.StartTime -ge $started.AddSeconds(-2)) { Stop-Process -Id $mp -Force; Write-Host 'player closed' }
    elseif ($p) { Write-Host 'player was already running before this test; left open' }
}
