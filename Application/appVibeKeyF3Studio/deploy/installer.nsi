; =====================================================================
;  VibeKey-F3 Studio NSIS installer script
;
;  LOCATION: deploy\installer.nsi  (same folder as build.bat / build_all.ps1)
;
;  ** Build through the single entry point (do NOT call makensis by hand): **
;        deploy\build.bat            (package)
;        deploy\build.bat rebuild    (recompile Release first)
;    build_all.ps1 reads the version from src\AppVersion.h, invokes makensis
;    with /DVERSION= and /DOUTFILE=, then deletes the staging dir.
;
;  ** Manual invocation is for debugging only - you MUST pass the version,
;     otherwise VERSION falls back to 0.0.0 and the output is named v0.0.0,
;     and makensis MUST run with the project root as CWD: **
;        cd <project root>
;        makensis /DVERSION=1.1.4 /DOUTFILE=deploy\VibeKey-F3_Studio_Setup_v1.1.4.exe deploy\installer.nsi
;
;  IMPORTANT - how makensis resolves the relative paths below (${SRC}, the
;             !system probes). Verified by experiment on 10-03, do not change:
;               * WITHOUT /NOCD  makensis first CHDIRs into THIS FILE's folder
;                                (deploy\), so ${SRC} would resolve to
;                                deploy\deploy\... and fail "no files found".
;               * WITH    /NOCD  the caller's CWD is kept, so ${SRC} resolves
;                                relative to the project root.
;             build_all.ps1 therefore invokes:
;               makensis /NOCD /DVERSION=.. /DOUTFILE=.. deploy\installer.nsi
;               with -WorkingDirectory <project root>
;             If you ever run makensis by hand you MUST do the same, otherwise
;             you get: File: "deploy\VibeKey-F3-Studio_Green\*.*" -> no files found.
;
;  Prereq  : deploy\VibeKey-F3-Studio_Green\  (staged by build_all.ps1 step 2)
;  Optional: drop vc_redist.x64.exe into that directory - it is then
;            installed silently on the target PC during setup.
;  Output  : deploy\VibeKey-F3_Studio_Setup_v<version>.exe
; =====================================================================
!include "MUI2.nsh"
!include "LogicLib.nsh"

; ----- 编译期开关：是否打包了 VC++ 运行库 -----
!system 'echo. > "_vcr.nsh"'
!system 'if exist "deploy\VibeKey-F3-Studio_Green\vc_redist.x64.exe" echo !define HAS_VCREDIST >> "_vcr.nsh"'
!include "_vcr.nsh"
!system 'del /q "_vcr.nsh" >nul 2>&1'

!define APPNAME   "VibeKey-F3 Studio"
!define EXENAME   "VibeKey-F3 Studio.exe"
!ifndef VERSION
  !define VERSION "0.0.0"          ; 正常由 build_all.ps1 从 src/AppVersion.h 读取 -DVERSION=
!endif
!define PUBLISHER "VibeKey-F3"
!define SRC       "deploy\VibeKey-F3-Studio_Green"

Name    "${APPNAME}"
!ifndef OUTFILE
  !define OUTFILE "VibeKey-F3_Studio_Setup_v${VERSION}.exe"
!endif
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin        ; 需要写 HKLM 并静默装 VC 运行库
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey  "ProductName"    "${APPNAME}"
VIAddVersionKey  "FileVersion"    "${VERSION}"
VIAddVersionKey  "LegalCopyright" "${PUBLISHER}"
VIAddVersionKey  "FileDescription" "${APPNAME} Installer"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
; 若 NSIS 未带 SimpChinese 语言包，改为 "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Section "Main" SEC01
  ; 自更新: 覆盖前先结束正在运行的旧版(否则 exe 被占用无法覆盖); GUI 重装同理
  nsExec::ExecToLog 'cmd /c taskkill /F /IM "${EXENAME}" >nul 2>&1'

  SetOutPath "$INSTDIR"
  ; 递归打包整个绿色版目录（含 Qt 依赖、python\、4 个 worker .py）
  File /r "${SRC}\*.*"

  ; 静默安装 VC++ 运行库（仅当 deploy 目录里带了 vc_redist.x64.exe）
  !ifdef HAS_VCREDIST
    ExecWait '"$INSTDIR\vc_redist.x64.exe" /install /quiet /norestart'
    Delete "$INSTDIR\vc_redist.x64.exe"
  !endif

  ; 快捷方式
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"

  ; 卸载程序
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; 控制面板 -> 添加/删除程序
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayName"     "${APPNAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "Publisher"       "${PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayVersion"  "${VERSION}"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoRepair" 1
  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"

  ; 自更新(/S)结束时自动启动新版 —— 上位机静默升级后无缝重开
  ${If} ${Silent}
    Exec '"$INSTDIR\${EXENAME}"'
  ${EndIf}
SectionEnd

Section "Uninstall"
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
  DeleteRegKey HKLM "Software\${APPNAME}"
SectionEnd
