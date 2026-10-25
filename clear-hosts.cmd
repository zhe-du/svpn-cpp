@echo off
rem Remove svpn entries from the hosts file. Self-elevates if needed.
rem Keep every comment ASCII. CRLF only.
chcp 65001 >nul
cd /d "%~dp0"

if not exist svpn.exe (
    echo [!] svpn.exe not found next to this script.
    pause
    exit /b 1
)

net session >nul 2>&1
if errorlevel 1 goto elevate

svpn.exe hosts clear --no-elevate
echo.
svpn.exe status
echo.
pause
exit /b 0

:elevate
echo [i] requesting administrator rights ...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b 0
