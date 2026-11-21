!include "MUI2.nsh"
!include "LogicLib.nsh"
Unicode true
RequestExecutionLevel user
Name "CGPlay 1.0.8.02"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\CGPlay"
VIProductVersion "1.0.8.2"
VIAddVersionKey "ProductName" "CGPlay"
VIAddVersionKey "FileDescription" "CGPlay online installer"
VIAddVersionKey "FileVersion" "1.0.8.02"
VIAddVersionKey "ProductVersion" "1.0.8.02"
VIAddVersionKey "LegalCopyright" "CGPlay Team"
!define MUI_ICON "..\resources\CGPlay.ico"
!define MUI_WELCOMEPAGE_TEXT "此安装程序将 CGPlay 更新到 1.0.8.02。已有 1.0.8.01 时只下载约 45 MiB。首次安装或旧版需要另外下载基础运行组件（约 802 MiB）。请先保存工作并关闭播放器。"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
  SetRegView 64
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\CGPlay" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} $INSTDIR == "$LOCALAPPDATA\Programs\CGPlay"
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

Section
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "Apply-OnlineUpdate.ps1"
  SetCompress off
  File /oname=CGPlay_Update_1.0.8.01_to_1.0.8.02_win64.zip "${DELTA_ZIP}"
  SetCompress auto
  System::Call 'Kernel32::SetEnvironmentVariableW(w "CGPLAY_ONLINE_INSTALL_DIR", w "$INSTDIR")'
  System::Call 'Kernel32::SetEnvironmentVariableW(w "CGPLAY_ONLINE_SOURCE_DIR", w "$EXEDIR")'
  ; Use 64-bit Windows PowerShell to match the install and registry view.
  nsExec::ExecToStack '"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Apply-OnlineUpdate.ps1"'
  Pop $0
  Pop $1
  DetailPrint "$1"
  ${If} $0 != 0
    FileOpen $2 "$TEMP\CGPlay-update-1.0.8.02.log" w
    FileWrite $2 "Exit $0. $1"
    FileClose $2
    SetErrorLevel 1
    IfSilent +2
    MessageBox MB_ICONSTOP "更新未完成。请查看安装详情，关闭占用文件的程序后重试。"
    Abort
  ${EndIf}
SectionEnd
