@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist svpn.exe (
    echo [!] svpn.exe not found next to this script.
    pause
    exit /b 1
)
if not exist config.json svpn.exe init
svpn.exe up -b
timeout /t 2 >nul
svpn.exe status
pause
