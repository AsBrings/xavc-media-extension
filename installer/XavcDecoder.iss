; XavcDecoder.iss - guided installer (Inno Setup 6.5+) for the XAVC S / S-I decoder.
;
; Built by build\package.ps1, which first assembles build\package\XavcDecoder-<ver>\ and then runs
;   ISCC /DAppVersion=<ver> /DPkgDir=<that folder> installer\XavcDecoder.iss
;
; What it does (same as installer\Install-XavcDecoder.ps1):
;   machine part (elevated): files in Program Files\XavcDecoder, COM + MFT registration (regserver),
;     HKLM\SOFTWARE\XavcDecoder, Active Setup for the PC's other users (task "allusers");
;   per-user part, run as the user who started Setup (not the elevated account):
;     regsvr32 /n /i:user  -> the HKCU settings that make Media Foundation use the handler.
; The DLLs must stay in Program Files: the Media Player is an AppContainer and can only load
; code from folders it may read.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef PkgDir
  #define PkgDir "..\build\package\XavcDecoder-" + AppVersion
#endif
#define ActiveSetupVersion StringChange(AppVersion, ".", ",") + ",0"
#define ActiveSetupKey "SOFTWARE\Microsoft\Active Setup\Installed Components\{{7F9A3C2F-5E44-4B0A-9C1D-2A6B8E3D4F10}"

[Setup]
AppId={{7F9A3C2E-5E44-4B0A-9C1D-2A6B8E3D4F10}
AppName=XAVC S / S-I Decoder
AppVersion={#AppVersion}
AppVerName=XAVC S / S-I Decoder {#AppVersion}
AppPublisher=xavc-media-extension
AppPublisherURL=https://github.com/AsBrings/xavc-media-extension
AppSupportURL=https://github.com/AsBrings/xavc-media-extension/issues
AppUpdatesURL=https://github.com/AsBrings/xavc-media-extension/releases
DefaultDirName={commonpf64}\XavcDecoder
DisableWelcomePage=no
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
WizardStyle=modern
ShowLanguageDialog=auto
LicenseFile=..\LICENSE
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
UninstallDisplayName=XAVC S / S-I Decoder (Media Foundation)
OutputDir=..\build\package
OutputBaseFilename=XavcDecoder-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=XAVC S / S-I Decoder
VersionInfoDescription=XAVC S / S-I Decoder Setup
VersionInfoCompany=xavc-media-extension

[Languages]
Name: "chinesesimplified"; MessagesFile: "inno\ChineseSimplified.isl"; InfoBeforeFile: "inno\info-zh.txt"
Name: "english"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "inno\info-en.txt"

[CustomMessages]
chinesesimplified.AllUsers=同时为本机其他用户启用（他们下次登录时自动生效）
english.AllUsers=Also enable it for the other users of this PC (at their next sign-in)
chinesesimplified.EnableForUser=正在为当前用户启用...
english.EnableForUser=Enabling it for the current user...
chinesesimplified.ResetThumbnails=清空缩略图缓存，让已有的 XAVC 视频重新生成缩略图（资源管理器会重启）
english.ResetThumbnails=Clear the thumbnail cache so existing XAVC videos get new thumbnails (restarts Explorer)
chinesesimplified.OpenReadme=查看说明
english.OpenReadme=Show the readme

[Tasks]
Name: "allusers"; Description: "{cm:AllUsers}"

[Files]
Source: "{#PkgDir}\bin\xavc_decoder.dll"; DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete
Source: "{#PkgDir}\bin\avcodec-xavc-*.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PkgDir}\bin\avutil-xavc-*.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PkgDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PkgDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PkgDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PkgDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion

[InstallDelete]
; left behind by the script-based installer or earlier builds
Type: files; Name: "{app}\Uninstall-XavcDecoder.ps1"
Type: files; Name: "{app}\uninstall.cmd"
Type: files; Name: "{app}\avcodec-63.dll"
Type: files; Name: "{app}\avutil-61.dll"
Type: files; Name: "{app}\swresample-7.dll"
Type: files; Name: "{app}\*.old-*"

[Registry]
Root: HKLM; Subkey: "SOFTWARE\XavcDecoder"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\XavcDecoder"; ValueType: dword; ValueName: "LogLevel"; ValueData: "0"; Flags: createvalueifdoesntexist
; the script-based installer's entry in Settings > Apps (this setup now manages the same files)
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\XavcDecoder"; Flags: deletekey
; Active Setup: run the per-user registration for every other user at their next sign-in
Root: HKLM; Subkey: "{#ActiveSetupKey}"; ValueType: string; ValueName: ""; ValueData: "XAVC S / S-I Decoder"; Tasks: allusers; Flags: uninsdeletekey
Root: HKLM; Subkey: "{#ActiveSetupKey}"; ValueType: string; ValueName: "StubPath"; ValueData: """{sys}\regsvr32.exe"" /s /n /i:user ""{app}\xavc_decoder.dll"""; Tasks: allusers
Root: HKLM; Subkey: "{#ActiveSetupKey}"; ValueType: string; ValueName: "Version"; ValueData: "{#ActiveSetupVersion}"; Tasks: allusers
Root: HKLM; Subkey: "{#ActiveSetupKey}"; ValueType: dword; ValueName: "IsInstalled"; ValueData: "1"; Tasks: allusers
Root: HKLM; Subkey: "{#ActiveSetupKey}"; Flags: deletekey; Tasks: not allusers

[Run]
; Per-user part, with the credentials of the user who started Setup (not the elevated account):
; Media Foundation reads these settings from that user's registry.
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /n /i:user ""{app}\xavc_decoder.dll"""; StatusMsg: "{cm:EnableForUser}"; Flags: runasoriginaluser waituntilterminated
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command ""$s = (Get-Process -Id $PID).SessionId; Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $s | Stop-Process -Force; Start-Sleep 2; Remove-Item ($env:LOCALAPPDATA + '\Microsoft\Windows\Explorer\thumbcache_*.db') -Force -ErrorAction SilentlyContinue; Start-Process explorer.exe"""; Description: "{cm:ResetThumbnails}"; Flags: postinstall unchecked runasoriginaluser runhidden waituntilterminated
Filename: "{app}\README.txt"; Description: "{cm:OpenReadme}"; Flags: postinstall unchecked shellexec skipifsilent

[UninstallRun]
; Per-user settings first (the DLL is needed for that); restores what was there before.
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u /n /i:user ""{app}\xavc_decoder.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "XavcUnregisterUser"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\XavcDecoder"
; folders an earlier script-based install created (Setup only removes folders it created itself)
Type: dirifempty; Name: "{app}\licenses"
Type: dirifempty; Name: "{app}"
