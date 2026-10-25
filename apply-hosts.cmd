@echo off
rem Apply svpn CDN edge IPs into the hosts file. Self-elevates if needed.
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

echo [1/3] refreshing CDN edges ...
svpn.exe cdn --refresh --no-bench --timeout 8
echo.
echo [2/3] writing hosts ...
svpn.exe hosts apply --no-elevate
echo.
echo [3/3] status ...
svpn.exe status
echo.
echo [i] done. To undo, run clear-hosts.cmd
pause
exit /b 0

:elevate
echo [i] requesting administrator rights ...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b 0
