# ESP32-C6 BLE Configuration System

## 🎯 Overview

Complete Bluetooth Low Energy (BLE) configuration system for ESP32-C6 Zigbee Gateway, replacing WiFi-based configuration to resolve WiFi+Zigbee coexistence issues.

## ✨ Key Features

- **Dual-Mode Setup**: WiFi AP (first boot) + BLE (ongoing configuration)
- **Progressive Web App**: Installable, works offline via Web Bluetooth API
- **Smart State Machine**: Button-controlled mode switching
- **JSON-over-BLE Protocol**: All HTTP endpoints mirrored for BLE
- **No Cloud Required**: Everything runs locally

## 🏗️ Architecture

```
┌─────────────────────────────────────────┐
│           ESP32-C6 Gateway              │
├─────────────────────────────────────────┤
│                                         │
│  Boot: WiFi AP + BLE (Setup Mode)      │
│        LED: Solid ON                    │
│                                         │
│  ↓ Button Press #1                     │
│                                         │
│  Operational: Zigbee Only               │
│               LED: 1 sec blink          │
│                                         │
│  ↓ Button Press #2                     │
│                                         │
│  Config Mode: Zigbee + BLE              │
│               LED: 2 sec slow blink     │
│                                         │
└─────────────────────────────────────────┘
         ↑                    ↑
         │                    │
    WiFi (boot)          BLE (anytime)
         │                    │
         ↓                    ↓
    ┌─────────┐        ┌──────────┐
    │ Browser │        │   PWA    │
    │ Install │        │ Offline  │
    └─────────┘        └──────────┘
```

## 📁 File Structure

### ESP32 Implementation (Phase 1)
```
main/
├── ble_task.c/h          # BLE lifecycle (NimBLE)
├── ble_service.c/h       # GATT server (5 characteristics)
├── ble_handlers.c/h      # JSON command processing
├── main.c                # Updated state machine
├── led_task.c            # BLE LED state support
└── CMakeLists.txt        # Build config with 'bt' component
```

### Web Frontend (Phase 2)
```
web/
├── ble-service.js        # Web Bluetooth API wrapper
├── service-worker.js     # PWA offline cache
├── manifest.json         # PWA metadata
├── icon-192.png          # PWA icon (generated)
├── icon-512.png          # PWA icon (generated)
├── index.html            # Updated with BLE UI
├── style.css             # BLE connection styles
└── script.js             # Fully BLE-integrated
```

## 🔌 BLE GATT Service

**Service UUID**: `0xFFF0`

| UUID   | Type           | Purpose                    |
|--------|----------------|----------------------------|
| 0xFFF1 | Write          | Command Request (JSON)     |
| 0xFFF2 | Read + Notify  | Command Response (JSON)    |
| 0xFFF3 | Read + Notify  | Device List Updates        |
| 0xFFF4 | Read + Notify  | System Status Updates      |
| 0xFFF5 | Write          | Large Transfer (chunked)   |

## 📡 Supported BLE Commands

All commands use JSON format:

```json
{"cmd": "get_status", "params": {}}
{"cmd": "get_devices", "params": {}}
{"cmd": "set_rtc", "params": {"year": 2025, "month": 1, ...}}
{"cmd": "set_device_config", "params": {"ieee_addr": "0x...", ...}}
{"cmd": "delete_device", "params": {"ieee_addr": "0x..."}}
{"cmd": "control_device", "params": {"ieee_addr": "0x...", "cmd": "on"}}
{"cmd": "permit_join", "params": {"duration": 60}}
{"cmd": "set_global_settings", "params": {"wifi_on_behavior": true}}
{"cmd": "get_global_settings", "params": {}}
{"cmd": "factory_reset", "params": {}}
```

## 🚀 Quick Start

### 1. Configure NimBLE
```bash
configure_nimble.bat  # Windows
# OR
./configure_nimble.sh # Linux/Mac
```

### 2. Build and Flash
```bash
idf.py build
idf.py -p COM3 flash monitor
```

### 3. Test BLE
- Use **nRF Connect** app (Android/iOS)
- Scan for "ESP32C6_Gateway"
- Connect and verify service 0xFFF0

### 4. Install PWA
- Connect to WiFi: "ESP32C6_AI_Test" / "12345678"
- Open: http://192.168.4.1
- Install PWA: "Add to Home Screen"

### 5. Use BLE
- Press button (exit setup → Zigbee mode)
- Press button again (enable BLE)
- Open PWA offline
- Click "Csatlakozas Bluetooth-on"
- Configure devices via BLE!

## 🎮 Button Controls

| Action       | Mode Change                  | LED Pattern        |
|--------------|-----------------------------|--------------------|
| Boot         | WiFi + BLE (Setup)          | Solid ON           |
| Press #1     | WiFi OFF, BLE OFF, Zigbee ON| 1 sec blink        |
| Press #2     | BLE ON (Zigbee continues)   | 2 sec slow blink   |
| Press #3     | BLE OFF                     | 1 sec blink        |
| Long Press   | Zigbee Pairing Mode         | 0.25 sec fast blink|

## 💡 LED States

| State               | Pattern            | Meaning              |
|---------------------|-------------------|----------------------|
| LED_STATE_WIFI_ACTIVE | Solid ON         | Setup mode (WiFi+BLE)|
| LED_STATE_NORMAL    | 1 sec blink       | Automation mode      |
| LED_STATE_BLE_ACTIVE| 2 sec slow blink  | BLE config mode      |
| LED_STATE_PAIRING   | 0.25 sec fast blink| Zigbee pairing     |
| LED_STATE_ERROR     | 3x fast, pause    | Error occurred       |

## 🔍 Monitoring

### ESP32 Logs
```bash
idf.py monitor

# Expected output:
# [BLE_TASK] BLE host synced
# [BLE_TASK] BLE advertising started: ESP32C6_Gateway
# [BLE_SERVICE] Connection handle set to: 0x0000
# [BLE_HANDLERS] Received command: {"cmd":"get_status","params":{}}
# [BLE_SERVICE] Sent response notification: len=125
```

### Browser Console
```javascript
// Expected output:
// Service Worker registered: ...
// BLE connection established
// Sending command: {"cmd":"get_status","params":{}}
// Received response: {"status":"ok","rtc_initialized":true,...}
```

## 📊 Implementation Status

### ✅ Completed
- [x] ESP32 BLE Foundation (Phase 1)
- [x] Web Frontend PWA (Phase 2)
- [x] BLE GATT Service (5 characteristics)
- [x] All HTTP endpoints → BLE commands
- [x] State machine integration
- [x] LED support for BLE mode
- [x] PWA manifest & service worker
- [x] Web Bluetooth API integration
- [x] Icon generation
- [x] WiFi task serves PWA files

### 📝 Remaining
- [ ] Configure NimBLE in sdkconfig (run configure_nimble.bat)
- [ ] Build and flash (idf.py build flash)
- [ ] Test BLE connection (nRF Connect app)
- [ ] Test PWA installation (Chrome/Edge)
- [ ] Test end-to-end workflow

## 📚 Documentation

- **[FINAL_STEPS.md](FINAL_STEPS.md)** - Complete build & test guide
- **[BLE_IMPLEMENTATION_STATUS.md](BLE_IMPLEMENTATION_STATUS.md)** - Detailed status
- **[NEXT_STEPS.md](NEXT_STEPS.md)** - Original integration guide
- **[web/ICONS_NEEDED.md](web/ICONS_NEEDED.md)** - Icon creation guide

## 🐛 Common Issues

### "bt component not found"
```bash
idf.py menuconfig
# Component config → Bluetooth → [*] Bluetooth
```

### "Web Bluetooth not supported"
- Use Chrome or Edge browser
- Firefox and Safari don't support Web Bluetooth API

### BLE not advertising
```bash
# Check logs for:
# "BLE host synced" ✓
# "BLE advertising started" ✓
```

### PWA won't install
- Check manifest.json is served
- Check icons exist (icon-192.png, icon-512.png)
- Check Service Worker registered

## 🎓 Technical Details

**NimBLE Stack**: Lightweight BLE-only host (vs Bluedroid)
- RAM usage: ~50KB (vs ~120KB for Bluedroid)
- Better ESP32-C6 support
- Simpler API for BLE-only applications

**Web Bluetooth API**: Native browser BLE support
- No app store required
- Works on Chrome/Edge
- Requires HTTPS or localhost (WiFi AP counts as localhost)

**Coexistence Strategy**:
- BLE has **higher priority** than Zigbee
- BLE is **periodic** (not continuous like WiFi)
- Result: Minimal interference

## 🔧 Build Configuration

Required in sdkconfig:
```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=n
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

## 🎯 Use Cases

1. **Initial Setup**: Configure RTC and devices via WiFi on first boot
2. **Daily Configuration**: Adjust device settings via BLE (offline)
3. **Automation Mode**: Devices run autonomously (Zigbee only)
4. **Quick Config**: Press button → enable BLE → adjust → press button → disable BLE

## 🏆 Advantages

### vs WiFi-Only
- ✅ No WiFi+Zigbee coexistence issues
- ✅ Lower power consumption (BLE intermittent)
- ✅ Better Zigbee reliability

### vs Cloud-Based
- ✅ Fully local (no internet required)
- ✅ No privacy concerns
- ✅ Instant response (no latency)

### vs App-Based
- ✅ No app store required
- ✅ PWA works on any platform
- ✅ Easy updates (just reload)

## 📞 Support

For issues or questions:
1. Check **[FINAL_STEPS.md](FINAL_STEPS.md)** troubleshooting section
2. Review ESP32 logs via `idf.py monitor`
3. Check browser console for errors
4. Verify NimBLE configuration in menuconfig

---

**Status**: ✅ Implementation Complete - Ready to Build!

**Last Updated**: 2025-01-24
