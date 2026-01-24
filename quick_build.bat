@echo off
echo ========================================
echo Quick Build - BLE Gateway
echo ========================================
echo.

REM Set ESP-IDF environment
call D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat

REM Go to project directory
cd /d D:\Programing\esp-idf\projects\AiAgent\CLCode01

REM Clean build
echo Cleaning previous build...
if exist build rmdir /s /q build
echo.

REM Build
echo Starting build...
idf.py build

echo.
echo ========================================
if %ERRORLEVEL% EQU 0 (
    echo BUILD SUCCESS!
    echo.
    echo Flash command:
    echo   idf.py -p COM3 flash monitor
) else (
    echo BUILD FAILED!
    echo Check errors above.
)
echo ========================================
pause
