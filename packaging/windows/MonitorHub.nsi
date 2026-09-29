Unicode True
RequestExecutionLevel user

!ifndef VERSION
  !error "VERSION define is required"
!endif
!ifndef STAGE_DIR
  !error "STAGE_DIR define is required"
!endif
!ifndef OUT_FILE
  !error "OUT_FILE define is required"
!endif

!include "MUI2.nsh"

Name "Monitor Hub ${VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Monitor Hub"
InstallDirRegKey HKCU "Software\Monitor\MonitorHubInstaller" "InstallDir"
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUnInstDetails show

!define MUI_ABORTWARNING
!define MUI_ICON "${STAGE_DIR}\resources\monitor_hub.ico"
!define MUI_UNICON "${STAGE_DIR}\resources\monitor_hub.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "Monitor Hub" SEC_MAIN
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"

  WriteRegStr HKCU "Software\Monitor\MonitorHubInstaller" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "DisplayName" "Monitor Hub"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "Publisher" "Monitor"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "DisplayIcon" "$INSTDIR\bin\monitor_hub_qt.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub" "NoRepair" 1

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\Monitor Hub"
  CreateShortcut "$SMPROGRAMS\Monitor Hub\Monitor Hub.lnk" "$INSTDIR\bin\monitor_hub_qt.exe"
  CreateShortcut "$SMPROGRAMS\Monitor Hub\Uninstall Monitor Hub.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Monitor Hub"
  RMDir /r "$SMPROGRAMS\Monitor Hub"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MonitorHub"
  DeleteRegKey HKCU "Software\Monitor\MonitorHubInstaller"
  RMDir /r "$INSTDIR"
SectionEnd
