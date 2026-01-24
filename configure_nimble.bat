@echo off
REM BLE Configuration Script for ESP32-C6 Zigbee Gateway
REM This script adds necessary NimBLE configuration to sdkconfig

echo Configuring NimBLE for ESP32-C6 Zigbee Gateway...
echo.

REM Backup existing sdkconfig
if exist sdkconfig (
    copy sdkconfig sdkconfig.backup >nul
    echo Backed up existing sdkconfig to sdkconfig.backup
)

REM Add NimBLE configuration
echo. >> sdkconfig
echo # >> sdkconfig
echo # BLE Configuration (added by configure_nimble.bat) >> sdkconfig
echo # >> sdkconfig
echo CONFIG_BT_ENABLED=y >> sdkconfig
echo CONFIG_BT_CONTROLLER_ENABLED=y >> sdkconfig
echo CONFIG_BT_NIMBLE_ENABLED=y >> sdkconfig
echo CONFIG_BT_BLUEDROID_ENABLED=n >> sdkconfig
echo. >> sdkconfig
echo # NimBLE Host >> sdkconfig
echo CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096 >> sdkconfig
echo CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1 >> sdkconfig
echo CONFIG_BT_NIMBLE_MAX_BONDS=3 >> sdkconfig
echo CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=512 >> sdkconfig
echo CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME="ESP32C6_Gateway" >> sdkconfig
echo. >> sdkconfig
echo # Bluetooth Controller >> sdkconfig
echo CONFIG_BT_CTRL_BLE_MAX_ACT=10 >> sdkconfig
echo CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=4 >> sdkconfig
echo CONFIG_BT_CTRL_PINNED_TO_CORE_0=y >> sdkconfig
echo. >> sdkconfig
echo # Wireless Coexistence >> sdkconfig
echo CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y >> sdkconfig
echo CONFIG_ESP_WIFI_SW_COEXIST_ENABLE=y >> sdkconfig

echo.
echo NimBLE configuration added to sdkconfig
echo.
echo IMPORTANT: You must now run the following commands:
echo   1. idf.py menuconfig
echo   2. Navigate to: Component config -^> Bluetooth
echo   3. Verify settings:
echo      - [*] Bluetooth
echo      - Host: (*) NimBLE - BLE only
echo      - Controller: [*] Controller Only
echo      - Bluedroid: [ ] DISABLED
echo   4. Save and exit (S, Enter, Q)
echo   5. idf.py build
echo.
echo For detailed instructions, see NEXT_STEPS.md
echo.
pause
