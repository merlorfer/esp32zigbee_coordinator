#!/bin/bash
#
# BLE Configuration Script for ESP32-C6 Zigbee Gateway
# This script adds necessary NimBLE configuration to sdkconfig
#

echo "Configuring NimBLE for ESP32-C6 Zigbee Gateway..."

# Backup existing sdkconfig
if [ -f sdkconfig ]; then
    cp sdkconfig sdkconfig.backup
    echo "Backed up existing sdkconfig to sdkconfig.backup"
fi

# Add or update NimBLE configuration
cat >> sdkconfig << 'EOF'

#
# BLE Configuration (added by configure_nimble.sh)
#
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=n

# NimBLE Host
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_MAX_BONDS=3
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=512
CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME="ESP32C6_Gateway"

# Bluetooth Controller
CONFIG_BT_CTRL_BLE_MAX_ACT=10
CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=4
CONFIG_BT_CTRL_PINNED_TO_CORE_0=y

# Wireless Coexistence
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
CONFIG_ESP_WIFI_SW_COEXIST_ENABLE=y

EOF

echo ""
echo "NimBLE configuration added to sdkconfig"
echo ""
echo "IMPORTANT: You must now run the following commands:"
echo "  1. idf.py menuconfig"
echo "  2. Navigate to: Component config → Bluetooth"
echo "  3. Verify settings:"
echo "     - [*] Bluetooth"
echo "     - Host: (*) NimBLE - BLE only"
echo "     - Controller: [*] Controller Only"
echo "     - Bluedroid: [ ] DISABLED"
echo "  4. Save and exit (S, Enter, Q)"
echo "  5. idf.py build"
echo ""
echo "For detailed instructions, see NEXT_STEPS.md"
