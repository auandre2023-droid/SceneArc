; SceneArc installer. Built by package-mpv.cmd, which stages the files
; into dist\stage-mpv and passes AppVersion, AppCommit, StageDir, VCRedist and OutputDir.
;
; The player is portable by design: config.json and data\ live next to VideoPlayer.exe.
; It therefore installs per user under %LOCALAPPDATA%\Programs (no admin rights, always
; writable). config.json and data\ are never part of the payload, so upgrades keep them.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppVersionNum
  #define AppVersionNum AppVersion
#endif
#ifndef AppCommit
  #define AppCommit "unknown"
#endif
#ifndef StageDir
  #error StageDir is not defined (run package-mpv.cmd)
#endif
#ifndef VCRedist
  #error VCRedist is not defined (run package-mpv.cmd)
#endif
#ifndef OutputName
  #define OutputName "SceneArc-Setup-" + AppVersion + "-" + AppCommit
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "SceneArc"
#define AppExe "VideoPlayer.exe"
; Oldest VC++ runtime VideoPlayer.exe accepts: the redist of the toolset it was built with.
#ifndef MinCrtMajor
  #define MinCrtMajor 14
#endif
#ifndef MinCrtMinor
  #define MinCrtMinor 44
#endif

[Setup]
AppId={{6B1C2E7A-3F0D-4C59-9A47-5D2B8E0F71C3}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} ({#AppCommit})
AppPublisher=SceneArc
VersionInfoVersion={#AppVersionNum}
VersionInfoDescription={#AppName} 安裝程式
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
SetupIconFile=..\host\src\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=4
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=no

[Languages]
Name: "zhtw"; MessagesFile: "compiler:Default.isl,ChineseTraditional.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; The web UI is fully replaced on every install so stale scripts or styles never linger.
Type: filesandordirs; Name: "{app}\ui"
; License texts are replaced too, so a component dropped from the package leaves no stale license behind.
Type: filesandordirs; Name: "{app}\licenses"
; Shortcuts left by installs from before the rename to SceneArc (the AppId is unchanged, so an
; update keeps the old install folder with its config.json and data\).
Type: files; Name: "{autoprograms}\Video Player MPV.lnk"
Type: files; Name: "{autodesktop}\Video Player MPV.lnk"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#VCRedist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall; Check: NeedsVCRedist

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /passive /norestart"; StatusMsg: "正在安裝 Microsoft Visual C++ 執行階段..."; Flags: waituntilterminated; Check: NeedsVCRedist
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
const
  WebView2ClientKey = 'Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  WebView2DownloadUrl = 'https://go.microsoft.com/fwlink/p/?LinkId=2124703';

function CrtIsOlder(const FileName: String): Boolean;
var
  MS, LS: Cardinal;
begin
  if not GetVersionNumbers(FileName, MS, LS) then
    Result := True
  else
    Result := ((MS shr 16) < {#MinCrtMajor}) or
              (((MS shr 16) = {#MinCrtMajor}) and ((MS and $FFFF) < {#MinCrtMinor}));
end;

// True when the system VC++ runtime is missing or older than the toolset VideoPlayer.exe uses.
function NeedsVCRedist: Boolean;
begin
  Result := CrtIsOlder(ExpandConstant('{sys}\vcruntime140.dll')) or
            CrtIsOlder(ExpandConstant('{sys}\vcruntime140_1.dll')) or
            CrtIsOlder(ExpandConstant('{sys}\msvcp140.dll'));
end;

function WebView2Installed: Boolean;
var
  Version: String;
begin
  Result :=
    (RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\' + WebView2ClientKey, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0')) or
    (RegQueryStringValue(HKLM, 'SOFTWARE\' + WebView2ClientKey, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0')) or
    (RegQueryStringValue(HKCU, 'Software\' + WebView2ClientKey, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0'));
end;

function InitializeSetup: Boolean;
var
  ErrorCode: Integer;
begin
  Result := True;
  if not WebView2Installed and not WizardSilent then begin
    if MsgBox('這台電腦尚未安裝 Microsoft Edge WebView2 Runtime，播放器介面需要它才能顯示。' + #13#10#13#10 +
              '要現在開啟 Microsoft 的下載頁面嗎？（安裝會照常繼續，可以之後再安裝 WebView2。）',
              mbConfirmation, MB_YESNO) = IDYES then
      ShellExec('open', WebView2DownloadUrl, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
  end;
end;

// Uninstall removes the program files; settings, playback history and caches stay unless the
// user asks to remove them too.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  AppDir := ExpandConstant('{app}');
  if not DirExists(AppDir) then Exit;
  if UninstallSilent then Exit;
  if MsgBox('要一併刪除設定、播放進度、區網使用者與快取嗎？' + #13#10#13#10 +
            '（' + AppDir + ' 內的 config.json、data 資料夾與其他留下的檔案）' + #13#10#13#10 +
            '選「否」會保留它們，日後重新安裝可以沿用。',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    DelTree(AppDir, True, True, True);
end;
