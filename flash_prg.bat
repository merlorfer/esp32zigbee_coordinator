@echo off
echo =======================
echo Flash program & monitor
echo =======================
echo.

REM Set ESP-IDF environment
call D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat

REM Set default COM port if not specified
set "SERIAL_PORT=%1"
if "%SERIAL_PORT%" == "" (
    echo No COM port specified, using default COM3.
    set "SERIAL_PORT=COM9"
) else (
    echo Using specified port: %SERIAL_PORT%
)

echo.
idf.py -p %SERIAL_PORT% flash monitor