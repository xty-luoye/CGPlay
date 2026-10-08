!define PRODUCT_NAME "CGPlay"
!ifndef APP_VERSION
  !define APP_VERSION "1.0.7.14"
!endif
!define PRODUCT_VERSION "${APP_VERSION}"
!ifndef APP_FILE_VERSION
  !define APP_FILE_VERSION "1.0.7.14"
!endif
!ifndef PACKAGE_MODE
  !define PACKAGE_MODE "full"
!endif
!ifndef PACKAGE_SOURCE_DIR
  !define PACKAGE_SOURCE_DIR "..\build_win_full\package\full\CGPlay"
!endif
!ifndef INSTALLER_OUTPUT_DIR
  !define INSTALLER_OUTPUT_DIR "..\build_win_full\installer"
!endif
!define PRODUCT_PUBLISHER "CGPlay Team"
!define PRODUCT_WEB_SITE "https://github.com/xty-luoye/CGPlay"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKCU"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\CGPlay.exe"
!define PRODUCT_RUN_REGKEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define PRODUCT_RUN_VALUE "CGPlayQuickLook"
!define CGPLAY_THUMBNAIL_CLSID "{B71A2E3C-9D47-4B1C-8D4D-2D4C04E1D9A7}"
!define CGPLAY_THUMBNAIL_SHELLEX "{E357FCCD-A995-4576-B01F-234630154E96}"

Unicode True
!if "${PACKAGE_MODE}" == "full"
SetCompressor zlib
!else
SetCompressor /SOLID lzma
SetCompressorDictSize 64
!endif

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!insertmacro GetSize

!define MUI_ABORTWARNING
!define MUI_ICON "..\resources\CGPlay.ico"
!define MUI_UNICON "..\resources\CGPlay.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${NSISDIR}\Contrib\Graphics\Wizard\orange.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${NSISDIR}\Contrib\Graphics\Header\orange.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_FINISHPAGE_RUN "$INSTDIR\CGPlay.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch CGPlay now"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\CGPlayQuickLook.exe"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Also start Explorer QuickLook preview"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

LangString PRODUCT_DESC ${LANG_SIMPCHINESE} "Install CGPlay with QuickLook preview and component bootstrapper."
LangString PRODUCT_DESC ${LANG_ENGLISH} "Install CGPlay with QuickLook preview and component bootstrapper."
LangString DESC_SecMain ${LANG_SIMPCHINESE} "Install CGPlay, QuickLook, export runtime, and all dependencies."
LangString DESC_SecMain ${LANG_ENGLISH} "Install CGPlay, QuickLook, export runtime, and all dependencies."
LangString DESC_SecStart ${LANG_SIMPCHINESE} "Create Start Menu shortcuts."
LangString DESC_SecStart ${LANG_ENGLISH} "Create Start Menu shortcuts."
LangString DESC_SecDesktop ${LANG_SIMPCHINESE} "Create Desktop shortcut."
LangString DESC_SecDesktop ${LANG_ENGLISH} "Create Desktop shortcut."
LangString DESC_SecQuickLook ${LANG_SIMPCHINESE} "Start QuickLook preview service automatically with Windows."
LangString DESC_SecQuickLook ${LANG_ENGLISH} "Start QuickLook preview service automatically with Windows."

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
!if "${PACKAGE_MODE}" == "lite"
OutFile "${INSTALLER_OUTPUT_DIR}\CGPlay_Setup_${PRODUCT_VERSION}_lite.exe"
!else
OutFile "${INSTALLER_OUTPUT_DIR}\CGPlay_Setup_${PRODUCT_VERSION}_full.exe"
!endif
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT_NAME}"
InstallDirRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation"
RequestExecutionLevel user

VIProductVersion "${APP_FILE_VERSION}"
VIAddVersionKey "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey "FileDescription" "CGPlay Setup"
VIAddVersionKey "LegalCopyright" "Copyright 2026 ${PRODUCT_PUBLISHER}"
VIAddVersionKey "FileVersion" "${PRODUCT_VERSION}"

Var StartMenuFolder

!macro RegisterVideoExtension EXT
  WriteRegStr HKCU "Software\Classes\.${EXT}" "" "CGPlay.Video"
  WriteRegStr HKCU "Software\Classes\.${EXT}" "PerceivedType" "video"
  WriteRegStr HKCU "Software\Classes\.${EXT}" "Content Type" "video/${EXT}"
  WriteRegStr HKCU "Software\Classes\.${EXT}\OpenWithProgids" "CGPlay.Video" ""
  WriteRegStr HKCU "Software\Classes\.${EXT}\ShellEx\${CGPLAY_THUMBNAIL_SHELLEX}" "" "${CGPLAY_THUMBNAIL_CLSID}"
  WriteRegStr HKCU "Software\Classes\Applications\CGPlay.exe\SupportedTypes" ".${EXT}" ""
  WriteRegStr HKCU "Software\CGPlay\Capabilities\FileAssociations" ".${EXT}" "CGPlay.Video"
!macroend

!macro UnregisterVideoExtension EXT
  DeleteRegValue HKCU "Software\Classes\.${EXT}\OpenWithProgids" "CGPlay.Video"
  DeleteRegValue HKCU "Software\Classes\.${EXT}\ShellEx\${CGPLAY_THUMBNAIL_SHELLEX}" ""
  DeleteRegValue HKCU "Software\Classes\Applications\CGPlay.exe\SupportedTypes" ".${EXT}"
  DeleteRegValue HKCU "Software\CGPlay\Capabilities\FileAssociations" ".${EXT}"
  DeleteRegValue HKCU "Software\Classes\.${EXT}" "PerceivedType"
  DeleteRegValue HKCU "Software\Classes\.${EXT}" "Content Type"
  ReadRegStr $0 HKCU "Software\Classes\.${EXT}" ""
  StrCmp $0 "CGPlay.Video" 0 +2
    DeleteRegValue HKCU "Software\Classes\.${EXT}" ""
!macroend

Function CloseCGPlayProcesses
  DetailPrint "Closing running CGPlay processes..."
  ClearErrors
  ExecWait '"$SYSDIR\taskkill.exe" /F /T /IM CGPlayQuickLook.exe' $0
  ClearErrors
  ExecWait '"$SYSDIR\taskkill.exe" /F /T /IM CGPlay.exe' $1
  Sleep 1200
FunctionEnd

Function un.CloseCGPlayProcesses
  DetailPrint "Closing running CGPlay processes..."
  ClearErrors
  ExecWait '"$SYSDIR\taskkill.exe" /F /T /IM CGPlayQuickLook.exe' $0
  ClearErrors
  ExecWait '"$SYSDIR\taskkill.exe" /F /T /IM CGPlay.exe' $1
  Sleep 1200
FunctionEnd

Function .onInit
  ; CGPlay and its Explorer thumbnail DLL are 64-bit. COM registration must
  ; reach the same registry view as 64-bit Explorer, even with an x86 installer.
  SetRegView 64
  Call CloseCGPlayProcesses
FunctionEnd

Function un.onInit
  SetRegView 64
  Call un.CloseCGPlayProcesses
FunctionEnd

Section "!CGPlay" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${PACKAGE_SOURCE_DIR}\*.*"
  CreateDirectory "$INSTDIR\plugins"
  CreateDirectory "$INSTDIR\runtime"
  CreateDirectory "$INSTDIR\runtime\python"
  CreateDirectory "$INSTDIR\tools"
  CreateDirectory "$INSTDIR\tools\cgplay"
  CreateDirectory "$INSTDIR\resources"
  CreateDirectory "$INSTDIR\presets"
  CreateDirectory "$INSTDIR\translations"
  CreateDirectory "$INSTDIR\components"

  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\CGPlay.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoRepair" 1

  WriteRegStr HKCU "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\CGPlay.exe"
  WriteRegStr HKCU "${PRODUCT_DIR_REGKEY}" "Path" "$INSTDIR"

  ; Register a per-user ProgID and Default apps capabilities. Windows keeps
  ; an explicit UserChoice intact, while unclaimed extensions use CGPlay.
  WriteRegStr HKCU "Software\Classes\CGPlay.Video" "" "CGPlay 视频"
  WriteRegStr HKCU "Software\Classes\CGPlay.Video\DefaultIcon" "" "$INSTDIR\CGPlay.exe,0"
  WriteRegStr HKCU "Software\Classes\CGPlay.Video\shell\open\command" "" '"$INSTDIR\CGPlay.exe" "%1"'
  WriteRegStr HKCU "Software\Classes\CGPlay.Video\ShellEx\${CGPLAY_THUMBNAIL_SHELLEX}" "" "${CGPLAY_THUMBNAIL_CLSID}"
  WriteRegStr HKCU "Software\Classes\CLSID\${CGPLAY_THUMBNAIL_CLSID}\InprocServer32" "" "$INSTDIR\CGPlayThumbnailProvider.dll"
  WriteRegStr HKCU "Software\Classes\CLSID\${CGPLAY_THUMBNAIL_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  ; The existing provider initializes with a file path for its FFmpeg worker.
  ; The isolated Shell host only supports IInitializeWithStream.
  WriteRegDWORD HKCU "Software\Classes\CLSID\${CGPLAY_THUMBNAIL_CLSID}" "DisableProcessIsolation" 1
  WriteRegStr HKCU "Software\Classes\Applications\CGPlay.exe\shell\open\command" "" '"$INSTDIR\CGPlay.exe" "%1"'
  WriteRegStr HKCU "Software\CGPlay\Capabilities" "ApplicationName" "CGPlay"
  WriteRegStr HKCU "Software\CGPlay\Capabilities" "ApplicationDescription" "CGPlay 视频播放器与审片工具"
  WriteRegStr HKCU "Software\CGPlay\Capabilities" "ApplicationIcon" "$INSTDIR\CGPlay.exe,0"
  WriteRegStr HKCU "Software\RegisteredApplications" "CGPlay" "Software\CGPlay\Capabilities"

  !insertmacro RegisterVideoExtension 3g2
  !insertmacro RegisterVideoExtension 3gp
  !insertmacro RegisterVideoExtension asf
  !insertmacro RegisterVideoExtension avi
  !insertmacro RegisterVideoExtension divx
  !insertmacro RegisterVideoExtension dv
  !insertmacro RegisterVideoExtension f4v
  !insertmacro RegisterVideoExtension flv
  !insertmacro RegisterVideoExtension ivf
  !insertmacro RegisterVideoExtension m1v
  !insertmacro RegisterVideoExtension m2ts
  !insertmacro RegisterVideoExtension m2v
  !insertmacro RegisterVideoExtension m4v
  !insertmacro RegisterVideoExtension mj2
  !insertmacro RegisterVideoExtension mkv
  !insertmacro RegisterVideoExtension mov
  !insertmacro RegisterVideoExtension mp4
  !insertmacro RegisterVideoExtension mpeg
  !insertmacro RegisterVideoExtension mpg
  !insertmacro RegisterVideoExtension mts
  !insertmacro RegisterVideoExtension mxf
  !insertmacro RegisterVideoExtension ogv
  !insertmacro RegisterVideoExtension prores
  !insertmacro RegisterVideoExtension rm
  !insertmacro RegisterVideoExtension rmvb
  !insertmacro RegisterVideoExtension ts
  !insertmacro RegisterVideoExtension vob
  !insertmacro RegisterVideoExtension webm
  !insertmacro RegisterVideoExtension wmv
  !insertmacro RegisterVideoExtension wtv
  !insertmacro RegisterVideoExtension y4m

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "EstimatedSize" "$0"

  WriteUninstaller "$INSTDIR\uninst.exe"
SectionEnd

Section "$(DESC_SecStart)" SecStart
  StrCpy $StartMenuFolder "$SMPROGRAMS\${PRODUCT_NAME}"
  CreateDirectory "$StartMenuFolder"
  CreateShortCut "$StartMenuFolder\CGPlay.lnk" "$INSTDIR\CGPlay.exe"
  CreateShortCut "$StartMenuFolder\CGPlay QuickLook.lnk" "$INSTDIR\CGPlayQuickLook.exe"
  CreateShortCut "$StartMenuFolder\Uninstall CGPlay.lnk" "$INSTDIR\uninst.exe"
SectionEnd

Section "$(DESC_SecDesktop)" SecDesktop
  CreateShortCut "$DESKTOP\CGPlay.lnk" "$INSTDIR\CGPlay.exe"
SectionEnd

Section "$(DESC_SecQuickLook)" SecQuickLook
  WriteRegStr HKCU "${PRODUCT_RUN_REGKEY}" "${PRODUCT_RUN_VALUE}" '"$INSTDIR\CGPlayQuickLook.exe"'
  Exec '"$INSTDIR\CGPlayQuickLook.exe"'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_SecMain)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStart} $(DESC_SecStart)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecQuickLook} $(DESC_SecQuickLook)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  DeleteRegValue HKCU "${PRODUCT_RUN_REGKEY}" "${PRODUCT_RUN_VALUE}"
  DeleteRegKey HKCU "${PRODUCT_DIR_REGKEY}"
  DeleteRegValue HKCU "Software\RegisteredApplications" "CGPlay"
  DeleteRegKey HKCU "Software\Classes\CGPlay.Video"
  DeleteRegKey HKCU "Software\Classes\CLSID\${CGPLAY_THUMBNAIL_CLSID}"
  DeleteRegKey HKCU "Software\Classes\Applications\CGPlay.exe\SupportedTypes"
  DeleteRegKey HKCU "Software\Classes\Applications\CGPlay.exe\shell"
  DeleteRegKey HKCU "Software\CGPlay\Capabilities"
  DeleteRegKey HKCU "Software\CGPlay"

  !insertmacro UnregisterVideoExtension 3g2
  !insertmacro UnregisterVideoExtension 3gp
  !insertmacro UnregisterVideoExtension asf
  !insertmacro UnregisterVideoExtension avi
  !insertmacro UnregisterVideoExtension divx
  !insertmacro UnregisterVideoExtension dv
  !insertmacro UnregisterVideoExtension f4v
  !insertmacro UnregisterVideoExtension flv
  !insertmacro UnregisterVideoExtension ivf
  !insertmacro UnregisterVideoExtension m1v
  !insertmacro UnregisterVideoExtension m2ts
  !insertmacro UnregisterVideoExtension m2v
  !insertmacro UnregisterVideoExtension m4v
  !insertmacro UnregisterVideoExtension mj2
  !insertmacro UnregisterVideoExtension mkv
  !insertmacro UnregisterVideoExtension mov
  !insertmacro UnregisterVideoExtension mp4
  !insertmacro UnregisterVideoExtension mpeg
  !insertmacro UnregisterVideoExtension mpg
  !insertmacro UnregisterVideoExtension mts
  !insertmacro UnregisterVideoExtension mxf
  !insertmacro UnregisterVideoExtension ogv
  !insertmacro UnregisterVideoExtension prores
  !insertmacro UnregisterVideoExtension rm
  !insertmacro UnregisterVideoExtension rmvb
  !insertmacro UnregisterVideoExtension ts
  !insertmacro UnregisterVideoExtension vob
  !insertmacro UnregisterVideoExtension webm
  !insertmacro UnregisterVideoExtension wmv
  !insertmacro UnregisterVideoExtension wtv
  !insertmacro UnregisterVideoExtension y4m
  DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"

  Delete "$DESKTOP\CGPlay.lnk"
  RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"

  Delete "$INSTDIR\uninst.exe"
  RMDir /r "$INSTDIR"
SectionEnd
