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

!define MUI_WELCOMEPAGE_TITLE "Monitor Hub ${VERSION} · 安装向导"
!define MUI_WELCOMEPAGE_TEXT "Monitor Hub 是面向本地计算、HPC 与 Agent 工作流的 Windows 监控控制台。$$
$$
推荐保持默认安装目录。本安装仅作用于当前用户，不需要管理员权限，也不会删除、移动或覆盖你的项目/计算数据。$$
$$
Monitor Hub 支持托盘与后台运行；关闭主窗口时，如果启用了关闭到托盘，程序仍会继续运行。"
!define MUI_FINISHPAGE_TITLE "Monitor Hub ${VERSION} 已安装完成"
!define MUI_FINISHPAGE_TEXT "建议首次启动后检查：监控项目路径、Claude CLI 接入、通知设置和自动处理权限。自动恢复始终受项目 recovery policy 与 L1/L2/L3 权限边界约束。"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\monitor_hub_qt.exe"
!define MUI_FINISHPAGE_RUN_TEXT "安装完成后启动 Monitor Hub"

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
