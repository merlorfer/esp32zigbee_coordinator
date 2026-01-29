@echo off
echo ========================================
echo Clean Build with SPIFFS refresh
echo ========================================
echo.

REM Set ESP-IDF environment
call D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat

REM Go to project directory
cd /d D:\Programing\esp-idf\projects\AiAgent\CLCode01

REM Clean SPIFFS image specifically
echo Cleaning SPIFFS image...
if exist build\storage.bin del /f build\storage.bin
echo.

REM Clean build directory
echo Cleaning build directory...
if exist build rmdir /s /q build
echo.

REM Build
echo Starting clean build...
idf.py build

echo.
echo ========================================
if %ERRORLEVEL% EQU 0 (
    echo BUILD SUCCESS!
    echo.
    echo Flash command:
    echo   idf.py -p COM3 flash
) else (
    echo BUILD FAILED!
    echo Check errors above.
)
echo ========================================
pause
