@echo off
title=bootloader download
set WORK_PATH=%~dp0
set CURR_PATH=%cd%
cd %WORK_PATH%
:start
echo,
echo      Bootloader Download
echo,
set /p input=please input the serial port num:
goto download
:download
echo com%input%
sftool -p COM%input% -c SF32LB52 -m nor write_flash "ram_v2\output\bootloader.bin@0x12010000"

if "%ENV_ROOT%"=="" pause
