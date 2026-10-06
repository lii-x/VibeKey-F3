@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
set PATH=D:\Qt\Tools\QtCreator\bin\jom;D:\Qt\Tools\CMake_64\bin;%PATH%
cd /d "%~dp0"
rmdir /s /q build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug 2>nul
cmake -B build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug -S . -G "NMake Makefiles JOM" -DCMAKE_PREFIX_PATH="D:\Qt\6.11.1\msvc2022_64" -DCMAKE_BUILD_TYPE=Debug
cmake --build build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug --target all
