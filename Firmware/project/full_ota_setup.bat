@echo off
title=Full OTA Setup - Bootloader + User App + OTA Seed
set WORK_PATH=%~dp0
set CURR_PATH=%cd%
cd %WORK_PATH%
:start
echo,
echo      ========================================
echo      Full OTA Setup Download
echo      ========================================
echo      1. Bootloader -> 0x12010000
echo      2. User App    -> 0x12020000
echo      3. OTA Seed    -> 0x12320000
echo      4. Flash Table -> 0x12000000
echo      ========================================
echo,
set /p input=please input the serial port num:
goto download
:download
echo com%input%
echo.
echo Flashing all components...
sftool -p COM%input% -c SF32LB52 -m nor write_flash "build_sf32lb52-vibekey_hcpu\bootloader\bootloader.bin@0x12010000" "build_sf32lb52-vibekey_hcpu\main.bin@0x12020000" "build_sf32lb52-vibekey_hcpu\main.bin@0x12320000" "build_sf32lb52-vibekey_hcpu\ftab\ftab.bin@0x12000000"
echo.
echo Done!
if "%ENV_ROOT%"=="" pause
