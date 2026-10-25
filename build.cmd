@echo off
rem Build svpn.exe (static, zero runtime deps). ASCII only.
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

echo [i] compiler: %CXX%
"%CXX%" -std=c++20 -O2 -Wall -Wextra -municode -mconsole -static -s ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    %SRC% -o svpn.exe ^
    -lws2_32 -lcrypt32 -lsecur32 -lwinhttp -ladvapi32 -luser32 -lshell32 -liphlpapi -lole32 -luuid -ldnsapi -lwininet
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo [x] build failed: %RC%
    popd
    exit /b %RC%
)

for %%f in (svpn.exe) do echo [+] built %%~ff  %%~zf bytes
popd
exit /b 0
