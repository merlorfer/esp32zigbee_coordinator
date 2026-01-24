@echo off
REM Build script for BLE-enabled ESP32-C6 Zigbee Gateway
REM This ensures clean build with proper NimBLE configuration

echo ========================================
echo ESP32-C6 BLE Gateway Build Script
echo ========================================
echo.

REM Check if we're in the right directory
if not exist "main\ble_task.c" (
    echo ERROR: Please run this script from the project root directory!
    pause
    exit /b 1
)

echo Step 1: Cleaning previous build...
if exist build (
    rmdir /s /q build
    echo Build directory removed.
)
if exist sdkconfig.old (
    del /q sdkconfig.old
    echo Old config removed.
)
echo.

echo Step 2: Checking NimBLE configuration in sdkconfig...
findstr /C:"CONFIG_BT_NIMBLE_ENABLED=y" sdkconfig >nul
if errorlevel 1 (
    echo WARNING: NimBLE not enabled in sdkconfig!
    echo.
    echo Please run configure_nimble.bat first, then:
    echo   idf.py menuconfig
    echo   Component config -^> Bluetooth -^> Verify NimBLE enabled
    echo.
    pause
    exit /b 1
)
echo NimBLE is enabled in sdkconfig.
echo.

echo Step 3: Building project...
echo This may take several minutes...
echo.
idf.py build

if errorlevel 1 (
    echo.
    echo ========================================
    echo BUILD FAILED!
    echo ========================================
    echo.
    echo Common issues:
    echo   1. NimBLE not properly configured
    echo      Solution: Run configure_nimble.bat
    echo.
    echo   2. ESP-IDF environment not set up
    echo      Solution: Run export.bat from ESP-IDF directory
    echo.
    echo   3. SPIFFS partition too small
    echo      Solution: Already fixed in partitions.csv
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCESSFUL!
echo ========================================
echo.
echo Next steps:
echo   1. Connect ESP32-C6 via USB
echo   2. Flash: idf.py -p COM3 flash monitor
echo      (Replace COM3 with your port)
echo.
echo Expected behavior:
echo   - Boot message: "Starting in setup mode (WiFi AP + BLE)"
echo   - BLE advertising: "ESP32C6_Gateway"
echo   - LED: Solid ON (setup mode)
echo.
pause
