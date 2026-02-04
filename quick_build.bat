@echo off
echo ========================================
echo Quick Build - BLE Gateway for esp32c6
echo ========================================
echo.

REM Set ESP-IDF environment
call D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat

REM Go to project directory
REM cd /d D:\Programing\esp-idf\projects\AiAgent\CLCode01

REM Set the target to esp32c6 unless -noset is specified
if /I NOT "%1" == "-noset" (
    echo Setting target to esp32c6...
    idf.py set-target esp32c6
) else (
    echo Skipping set-target as requested by %1 argument.
)

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
