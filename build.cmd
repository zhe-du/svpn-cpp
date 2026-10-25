@echo off
rem Build svpn.exe (console) and svpn-gui.exe (windowed). Static, zero runtime deps. ASCII only.
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "CXX=D:\LE\AI\tools\mingw64\bin\g++.exe"
if not exist "%CXX%" set "CXX=g++"

pushd "%ROOT%" || exit /b 1
if not exist build mkdir build

set "SRC="
for %%f in (src\*.cpp) do set "SRC=!SRC! %%f"
if "%SRC%"=="" (
    echo [!] no sources under src\
    exit /b 2
)

set "LIBS=-lws2_32 -lcrypt32 -lsecur32 -lwinhttp -ladvapi32 -luser32 -lshell32 -liphlpapi -lole32 -luuid -ldnsapi -lwininet -lcomctl32 -lgdi32"
set "FLAGS=-std=c++20 -O2 -Wall -Wextra -municode -static -s -finput-charset=UTF-8 -fexec-charset=UTF-8"

echo [i] compiler: %CXX%
echo [1/2] svpn.exe  ^(console, CLI + "svpn gui"^)
"%CXX%" %FLAGS% -mconsole %SRC% -o svpn.exe %LIBS%
if errorlevel 1 (
    echo [x] console build failed
    popd
    exit /b 1
)

echo [2/2] svpn-gui.exe  ^(windowed, no console flash^)
"%CXX%" %FLAGS% -mwindows -DSVPN_GUI_SUBSYSTEM %SRC% -o svpn-gui.exe %LIBS%
if errorlevel 1 (
    echo [x] gui build failed
    popd
    exit /b 1
)

for %%f in (svpn.exe svpn-gui.exe) do echo [+] built %%~ff  %%~zf bytes
popd
exit /b 0
