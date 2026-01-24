@echo off
REM Diagnostic script to check BLE setup
echo ========================================
echo BLE Setup Diagnostic Tool
echo ========================================
echo.

echo Checking ESP-IDF environment...
where idf.py >nul 2>&1
if errorlevel 1 (
    echo [FAIL] idf.py not found in PATH
    echo        Run: D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat
    set /a errors+=1
) else (
    echo [OK] idf.py found
)
echo.

echo Checking project files...
if exist "main\ble_task.c" (
    echo [OK] ble_task.c exists
) else (
    echo [FAIL] ble_task.c missing
    set /a errors+=1
)

if exist "main\ble_service.c" (
    echo [OK] ble_service.c exists
) else (
    echo [FAIL] ble_service.c missing
    set /a errors+=1
)

if exist "main\ble_handlers.c" (
    echo [OK] ble_handlers.c exists
) else (
    echo [FAIL] ble_handlers.c missing
    set /a errors+=1
)
echo.

echo Checking web files...
if exist "web\ble-service.js" (
    echo [OK] ble-service.js exists
) else (
    echo [FAIL] ble-service.js missing
    set /a errors+=1
)

if exist "web\icon-192.png" (
    echo [OK] icon-192.png exists
) else (
    echo [FAIL] icon-192.png missing - Run: python web\generate_icons.py
    set /a errors+=1
)

if exist "web\icon-512.png" (
    echo [OK] icon-512.png exists
) else (
    echo [FAIL] icon-512.png missing - Run: python web\generate_icons.py
    set /a errors+=1
)
echo.

echo Checking sdkconfig...
if not exist "sdkconfig" (
    echo [WARN] sdkconfig not found - will be created on first build
) else (
    findstr /C:"CONFIG_BT_ENABLED=y" sdkconfig >nul
    if errorlevel 1 (
        echo [FAIL] Bluetooth not enabled in sdkconfig
        echo        Run: configure_nimble.bat
        set /a errors+=1
    ) else (
        echo [OK] Bluetooth enabled
    )

    findstr /C:"CONFIG_BT_NIMBLE_ENABLED=y" sdkconfig >nul
    if errorlevel 1 (
        echo [FAIL] NimBLE not enabled in sdkconfig
        echo        Run: configure_nimble.bat
        set /a errors+=1
    ) else (
        echo [OK] NimBLE enabled
    )

    findstr /C:"CONFIG_BT_BLUEDROID_ENABLED is not set" sdkconfig >nul
    if errorlevel 1 (
        echo [WARN] Bluedroid might be enabled (should be disabled)
    ) else (
        echo [OK] Bluedroid disabled
    )
)
echo.

echo Checking partitions...
if exist "partitions.csv" (
    findstr /C:"0x20000" partitions.csv >nul
    if errorlevel 1 (
        echo [WARN] SPIFFS partition might be too small
        echo        Should be 0x20000 (128KB)
    ) else (
        echo [OK] SPIFFS partition is 128KB
    )
) else (
    echo [FAIL] partitions.csv not found
    set /a errors+=1
)
echo.

echo ========================================
if defined errors (
    echo RESULT: %errors% issue^(s^) found
    echo.
    echo Please fix the issues above before building.
) else (
    echo RESULT: All checks passed!
    echo.
    echo You can now build the project:
    echo   build_ble.bat
    echo or:
    echo   idf.py build
)
echo ========================================
echo.
pause
