@echo off
rem svpn launcher: create config if missing, start proxy in background, show status.
rem Keep every comment ASCII and keep one blank line after any non-ASCII line.
chcp 65001 >nul
cd /d "%~dp0"

if not exist "config.json" (
    echo [i] config.json not found, running: svpn.exe init
    svpn.exe init
    if errorlevel 1 goto failed
)

echo [i] starting svpn in background ...
svpn.exe up -b
if errorlevel 1 goto failed

timeout /t 2 >nul

svpn.exe status
echo.
echo [i] point your browser or launcher to socks5 127.0.0.1:7899 or http 127.0.0.1:7900
echo [i] to take over the system proxy:  svpn.exe system on
echo [i] to stop everything:            svpn.exe down
echo.
pause
exit /b 0

:failed
echo.
echo [!] start failed. see runtime\svpn.log for details.
echo.
pause
exit /b 1
