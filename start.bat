@echo off
REM StreaMonitor Startup Script with Crash Protection
REM This script will restart the application automatically if it crashes

title StreaMonitor Launcher

echo =====================================
echo  StreaMonitor Crash-Protected Startup
echo =====================================
echo.

REM Set working directory
cd /d "%~dp0"

REM Check if watchdog exists
if not exist "watchdog.py" (
    echo WARNING: watchdog.py not found, running without crash protection
    if exist "StreaMonitor.exe" (
        echo Starting StreaMonitor.exe...
        StreaMonitor.exe %*
    ) else if exist "dist\StreaMonitor.exe" (
        echo Starting dist\StreaMonitor.exe...
        dist\StreaMonitor.exe %*
    ) else if exist "build\Release\StreaMonitor.exe" (
        echo Starting build\Release\StreaMonitor.exe...
        build\Release\StreaMonitor.exe %*
    ) else (
        echo ERROR: Could not find StreaMonitor.exe
        pause
        exit /b 1
    )
    goto end
)

REM Check if Python is available
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo WARNING: Python not found, running without crash protection
    goto run_direct
)

REM Run with watchdog protection
echo Starting StreaMonitor with crash protection...
echo Press Ctrl+C to stop
echo.
python watchdog.py %*
goto end

:run_direct
if exist "StreaMonitor.exe" (
    echo Starting StreaMonitor.exe...
    StreaMonitor.exe --no-duplicate-check %*
) else if exist "dist\StreaMonitor.exe" (
    echo Starting dist\StreaMonitor.exe...
    dist\StreaMonitor.exe --no-duplicate-check %*
) else if exist "build\Release\StreaMonitor.exe" (
    echo Starting build\Release\StreaMonitor.exe...
    build\Release\StreaMonitor.exe --no-duplicate-check %*
) else (
    echo ERROR: Could not find StreaMonitor.exe
    pause
    exit /b 1
)

:end
echo.
echo StreaMonitor has exited.
pause