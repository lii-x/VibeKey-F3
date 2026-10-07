@echo off
rem ============================================================================
rem  build.bat - VibeKey-F3 Studio packaging entry point (double-click)
rem
rem    build.bat            -> recompile Release if needed, then package  (DEFAULT)
rem    build.bat rebuild    -> always force a full recompile first
rem    build.bat package    -> package only (use an existing Release build as-is)
rem
rem  All real work lives in build_all.ps1 (same directory). This file only
rem  picks a PowerShell host, forwards the switch, and keeps the window open.
rem
rem  2026-10-07: default mode changed from "package only" to "rebuild if needed"
rem  (forwards -AutoRebuild). Reason: build_all.ps1 now verifies that the exe
rem  embeds the VIBEKEY_STUDIO_VERSION from AppVersion.h (so an installer can
rem  never be named v1.1.6 while the payload still self-reports 1.1.5).
rem  A stale exe usually sits in build\release, so a plain double-click would
rem  just fail => "button does nothing" for the user. Now the script decides and
rem  recompiles on its own.
rem
rem  NOTE: comments here MUST stay ASCII. This file is UTF-8, but cmd.exe reads
rem  .bat using the ANSI code page (GBK on this machine) -> non-ASCII comment text
rem  turns into mojibake and some byte sequences get parsed as commands, which
rem  produced bogus "'xxx' is not recognized" errors. Keep this file ASCII-only.
rem
rem  NOTE: this file MUST stay CRLF. cmd.exe mis-parses LF-only line endings
rem  inside multi-line ( ... ) blocks, which silently swallows the code after
rem  them. Control flow is also deliberately flat (goto :label, no nested
rem  blocks) -- an if(...) whose body holds a for/f plus 2^>nul is what broke it.
rem ============================================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "PS1=%~dp0build_all.ps1"
set "PSEXE="
set "PSARGS="
set "RC=0"
set "NEWEST="
set "MODE=auto"

if /i "%~1"=="rebuild" set "PSARGS=-Rebuild"
if /i "%~1"=="rebuild" set "MODE=rebuild"
if /i "%~1"=="package" set "PSARGS=-PackageOnly"
if /i "%~1"=="package" set "MODE=package-only"
if not "%~1"=="" goto :have_args
rem no argument: default to -AutoRebuild (script recompiles only if needed)
set "PSARGS=-AutoRebuild"
:have_args

rem Prefer PowerShell 7 (pwsh): build_all.ps1 sets ErrorActionPreference=Stop, and
rem under Windows PowerShell 5.1 native stderr becomes a terminating
rem NativeCommandError. pwsh does not have that quirk.
where pwsh >nul 2>nul
if not errorlevel 1 set "PSEXE=pwsh"
if defined PSEXE goto :have_host
where powershell >nul 2>nul
if not errorlevel 1 set "PSEXE=powershell"
if defined PSEXE goto :have_host

echo [ERROR] Neither pwsh nor powershell was found on PATH.
echo         Install PowerShell 7 ^(winget install Microsoft.PowerShell^).
goto :fail

:have_host
echo ============================================================
echo  VibeKey-F3 Studio  -  build_all.ps1
echo  host      : !PSEXE!
echo  mode      : !MODE!  (!PSARGS!)
echo  started   : %DATE% %TIME%
echo ============================================================
echo.

!PSEXE! -NoProfile -ExecutionPolicy Bypass -File "!PS1!" !PSARGS!
set "RC=%errorlevel%"

if "%RC%"=="0" goto :success

echo.
echo [FAILED] build_all.ps1 exited with code %RC%.
call :newest_log build_all_*.log rebuild_all_*.log
if not defined NEWEST goto :no_log
echo.
echo ---- last 25 lines of %NEWEST% ----
!PSEXE! -NoProfile -Command "Get-Content -LiteralPath '%~dp0%NEWEST%' -Tail 25"
goto :fail

:no_log
echo.
echo   (no build log found in %~dp0)
goto :fail

:success
echo.
echo [DONE] build_all.ps1 finished successfully.
call :newest_log build_all_*.log rebuild_all_*.log
if defined NEWEST echo   log: %~dp0%NEWEST%
echo.
pause
exit /b 0

rem --- subroutine: set NEWEST to the most recently modified log matching masks
:newest_log
set "NEWEST="
for /f "delims=" %%L in ('dir /b /o-d %1 2^>nul') do call :take_first "%%L"
exit /b 0

:take_first
if defined NEWEST exit /b 0
set "NEWEST=%~1"
exit /b 0

:fail
echo.
pause
exit /b 1
