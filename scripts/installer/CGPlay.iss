#define AppName "CGPlay"
#ifndef AppVersion
  #define AppVersion "1.0.5"
#endif
#ifndef PackageMode
  #define PackageMode "full"
#endif
#ifndef PackageSource
  #define PackageSource "..\..\build_win_full\package\full\CGPlay"
#endif
#ifndef InstallerOutputDir
  #define InstallerOutputDir "..\..\build_win_full\installer\full"
#endif

[Setup]
AppId={{D30DF6D2-7F6F-4A36-9B2E-4D5A5B716E4D}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=CGPlay Team
AppPublisherURL=https://github.com/xty-luoye/CGPlay
AppSupportURL=https://github.com/xty-luoye/CGPlay
AppUpdatesURL=https://github.com/xty-luoye/CGPlay/releases
DefaultDirName={localappdata}\Programs\CGPlay
DefaultGroupName=CGPlay
AllowNoIcons=yes
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE.txt
OutputDir={#InstallerOutputDir}
#if PackageMode == "lite"
OutputBaseFilename=CGPlay_Setup_{#AppVersion}_lite
#else
OutputBaseFilename=CGPlay_Setup_{#AppVersion}_full
#endif
SetupIconFile=..\..\resources\CGPlay.ico
UninstallDisplayIcon={app}\CGPlay.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousAppDir=yes
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=CGPlay Team
VersionInfoDescription=CGPlay Installer
VersionInfoProductName=CGPlay
VersionInfoProductVersion={#AppVersion}
VersionInfoCopyright=Copyright 2026 CGPlay Team

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "simpchinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "quicklookautostart"; Description: "Start CGPlay QuickLook with Windows"; Flags: unchecked

[Dirs]
Name: "{app}\plugins"
Name: "{app}\runtime"
Name: "{app}\runtime\python"
Name: "{app}\tools"
Name: "{app}\tools\cgplay"
Name: "{app}\resources"
Name: "{app}\presets"
Name: "{app}\translations"
Name: "{app}\components"

[Files]
Source: "{#PackageSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\CGPlay\CGPlay"; Filename: "{app}\CGPlay.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\CGPlay\CGPlay QuickLook"; Filename: "{app}\CGPlayQuickLook.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\CGPlay\Uninstall CGPlay"; Filename: "{uninstallexe}"
Name: "{autodesktop}\CGPlay"; Filename: "{app}\CGPlay.exe"; Tasks: desktopicon; WorkingDir: "{app}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CGPlayQuickLook"; ValueData: """{app}\CGPlayQuickLook.exe"""; Flags: uninsdeletevalue; Tasks: quicklookautostart
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\CGPlay.exe"; ValueType: string; ValueData: "{app}\CGPlay.exe"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\CGPlay.exe"; ValueType: string; ValueName: "Path"; ValueData: "{app}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\CGPlay.exe"; Description: "Launch CGPlay"; Flags: nowait postinstall skipifsilent
Filename: "{app}\CGPlayQuickLook.exe"; Description: "Start QuickLook preview service"; Flags: nowait postinstall skipifsilent unchecked; Tasks: quicklookautostart

[Code]
const
  CGPlayThumbnailClsid = '{B71A2E3C-9D47-4B1C-8D4D-2D4C04E1D9A7}';
  CGPlayThumbnailShellEx = '{E357FCCD-A995-4576-B01F-234630154E96}';
  SupportedVideoExtensions: array[0..30] of String = (
    '3g2', '3gp', 'asf', 'avi', 'divx', 'dv', 'f4v', 'flv', 'ivf', 'm1v',
    'm2ts', 'm2v', 'm4v', 'mj2', 'mkv', 'mov', 'mp4', 'mpeg', 'mpg', 'mts',
    'mxf', 'ogv', 'prores', 'rm', 'rmvb', 'ts', 'vob', 'webm', 'wmv', 'wtv',
    'y4m');

procedure RegisterVideoExtension(const Ext: String);
begin
  RegWriteStringValue(HKCU, 'Software\Classes\.' + Ext, '', 'CGPlay.Video');
  RegWriteStringValue(HKCU, 'Software\Classes\.' + Ext, 'PerceivedType', 'video');
  RegWriteStringValue(HKCU, 'Software\Classes\.' + Ext, 'Content Type', 'video/' + Ext);
  RegWriteStringValue(HKCU, 'Software\Classes\.' + Ext + '\OpenWithProgids', 'CGPlay.Video', '');
  RegWriteStringValue(HKCU, 'Software\Classes\.' + Ext + '\ShellEx\' + CGPlayThumbnailShellEx, '', CGPlayThumbnailClsid);
  RegWriteStringValue(HKCU, 'Software\Classes\Applications\CGPlay.exe\SupportedTypes', '.' + Ext, '');
  RegWriteStringValue(HKCU, 'Software\CGPlay\Capabilities\FileAssociations', '.' + Ext, 'CGPlay.Video');
end;

procedure UnregisterVideoExtension(const Ext: String);
var
  Current: String;
begin
  RegDeleteValue(HKCU, 'Software\Classes\.' + Ext + '\OpenWithProgids', 'CGPlay.Video');
  RegDeleteValue(HKCU, 'Software\Classes\.' + Ext + '\ShellEx\' + CGPlayThumbnailShellEx, '');
  RegDeleteValue(HKCU, 'Software\Classes\Applications\CGPlay.exe\SupportedTypes', '.' + Ext);
  RegDeleteValue(HKCU, 'Software\CGPlay\Capabilities\FileAssociations', '.' + Ext);
  RegDeleteValue(HKCU, 'Software\Classes\.' + Ext, 'PerceivedType');
  RegDeleteValue(HKCU, 'Software\Classes\.' + Ext, 'Content Type');
  if RegQueryStringValue(HKCU, 'Software\Classes\.' + Ext, '', Current) and
     (CompareText(Current, 'CGPlay.Video') = 0) then
    RegDeleteValue(HKCU, 'Software\Classes\.' + Ext, '');
end;

procedure RegisterCGPlayAssociations;
var
  I: Integer;
begin
  RegWriteStringValue(HKCU, 'Software\Classes\CGPlay.Video', '', 'CGPlay 视频');
  RegWriteStringValue(HKCU, 'Software\Classes\CGPlay.Video\DefaultIcon', '', ExpandConstant('{app}\CGPlay.exe,0'));
  RegWriteStringValue(HKCU, 'Software\Classes\CGPlay.Video\shell\open\command', '',
    '"' + ExpandConstant('{app}\CGPlay.exe') + '" "%1"');
  RegWriteStringValue(HKCU, 'Software\Classes\CGPlay.Video\ShellEx\' + CGPlayThumbnailShellEx, '', CGPlayThumbnailClsid);
  RegWriteStringValue(HKCU, 'Software\Classes\CLSID\' + CGPlayThumbnailClsid + '\InprocServer32', '',
    ExpandConstant('{app}\CGPlayThumbnailProvider.dll'));
  RegWriteStringValue(HKCU, 'Software\Classes\CLSID\' + CGPlayThumbnailClsid + '\InprocServer32', 'ThreadingModel', 'Apartment');
  // The existing file-initialized provider needs the Shell's file-handler mode.
  RegWriteDWordValue(HKCU, 'Software\Classes\CLSID\' + CGPlayThumbnailClsid, 'DisableProcessIsolation', 1);
  RegWriteStringValue(HKCU, 'Software\Classes\Applications\CGPlay.exe\shell\open\command', '',
    '"' + ExpandConstant('{app}\CGPlay.exe') + '" "%1"');
  RegWriteStringValue(HKCU, 'Software\CGPlay\Capabilities', 'ApplicationName', 'CGPlay');
  RegWriteStringValue(HKCU, 'Software\CGPlay\Capabilities', 'ApplicationDescription', 'CGPlay 视频播放器与审片工具');
  RegWriteStringValue(HKCU, 'Software\CGPlay\Capabilities', 'ApplicationIcon', ExpandConstant('{app}\CGPlay.exe,0'));
  RegWriteStringValue(HKCU, 'Software\RegisteredApplications', 'CGPlay', 'Software\CGPlay\Capabilities');
  for I := 0 to GetArrayLength(SupportedVideoExtensions) - 1 do
    RegisterVideoExtension(SupportedVideoExtensions[I]);
end;

procedure UnregisterCGPlayAssociations;
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(SupportedVideoExtensions) - 1 do
    UnregisterVideoExtension(SupportedVideoExtensions[I]);
  RegDeleteValue(HKCU, 'Software\RegisteredApplications', 'CGPlay');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\CGPlay.Video');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\CLSID\' + CGPlayThumbnailClsid);
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Applications\CGPlay.exe\SupportedTypes');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Applications\CGPlay.exe\shell');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\CGPlay\Capabilities');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RegisterCGPlayAssociations;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    UnregisterCGPlayAssociations;
end;
