@echo off
title=Flash User App + OTA Seed
set WORK_PATH=%~dp0
set CURR_PATH=%cd%
cd %WORK_PATH%
:start
echo,
echo      Flash User App + OTA Seed
echo,
set /p input=please input the serial port num:
goto download
:download
echo com%input%
sftool -p COM%input% -c SF32LB52 -m nor write_flash "build_sf32lb52-vibekey_hcpu\main.bin@0x12020000" "build_sf32lb52-vibekey_hcpu\main.bin@0x12320000" "build_sf32lb52-vibekey_hcpu\ftab\ftab.bin@0x12000000"
if "%ENV_ROOT%"=="" pause
