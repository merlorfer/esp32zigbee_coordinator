# BLE Configuration System Implementation Status

## Phase 1: ESP32 BLE Foundation ✅ COMPLETE

### Files Created
- ✅ `main/ble_task.h` - BLE lifecycle management header
- ✅ `main/ble_task.c` - BLE lifecycle implementation (NimBLE stack)
- ✅ `main/ble_service.h` - GATT service definitions
- ✅ `main/ble_service.c` - GATT server with 5 characteristics
- ✅ `main/ble_handlers.h` - Command processing declarations
- ✅ `main/ble_handlers.c` - JSON command handlers (mirrors HTTP API)

### Files Modified
- ✅ `main/common.h` - Added BLE event bit, LED state, task priority/stack
- ✅ `main/CMakeLists.txt` - Added BLE source files and `bt` component
- ✅ `main/led_task.c` - Added LED_STATE_BLE_ACTIVE (2 sec slow blink)
- ✅ `main/main.c` - Updated state machine for new workflow

### Implementation Details

**BLE GATT Service (UUID: 0xFFF0)**:
- `0xFFF1` - Command Request (Write) - Receives JSON commands
- `0xFFF2` - Command Response (Read + Notify) - Sends JSON responses
- `0xFFF3` - Device List (Read + Notify) - Device array updates
- `0xFFF4` - System Status (Read + Notify) - Status updates
- `0xFFF5` - Large Transfer (Write) - Chunked data support

**Supported Commands**:
- `get_status` - System status
- `get_devices` - Device list
- `set_rtc` - Time synchronization
- `set_device_config` - Device automation config
- `delete_device` - Remove device (triggers Zigbee leave)
- `control_device` - Send ON/OFF/TOGGLE
- `permit_join` - Enable pairing
- `set_global_settings` - Global automation settings

**New State Machine**:
1. **Boot**: WiFi AP + BLE both active (setup mode)
   - LED: Solid ON (WiFi indicator)
   - User can configure via WiFi webpage OR BLE immediately
2. **First Button Press**: WiFi stops → BLE stops → Zigbee starts
   - LED: 1 sec blink (automation mode)
3. **Subsequent Short Presses**: Toggle BLE on/off (Zigbee continues)
   - BLE ON: LED = 2 sec slow blink
   - BLE OFF: LED = 1 sec normal blink
4. **Long Press**: Zigbee pairing mode (60 sec)
   - LED: Fast 0.25 sec blink

---

## Phase 2: Web Frontend (PWA) ⚠️ PARTIALLY COMPLETE

### Files Created
- ✅ `web/ble-service.js` - Web Bluetooth API wrapper class
- ✅ `web/service-worker.js` - PWA offline caching
- ✅ `web/manifest.json` - PWA metadata
- ✅ `web/ble-integration.js` - Helper functions for BLE integration
- ✅ `web/ICONS_NEEDED.md` - Icon creation instructions

### Files Modified
- ✅ `web/index.html` - Added manifest, BLE section, service worker registration
- ✅ `web/style.css` - Added BLE connection UI styles

### Files Requiring Updates
- ⚠️ `web/script.js` - **NEEDS INTEGRATION** with BLE functions

---

## Remaining Work

### Critical (Required for BLE to work)

1. **Update `web/script.js`** (integration needed):
   ```javascript
   // Add at the top of script.js
   let bleGateway = null;
   let bleConnected = false;
   let useBluetoothMode = false;

   // Copy functions from ble-integration.js:
   //   - connectBLE()
   //   - disconnectBLE()
   //   - apiRequest()
   //   - httpRequest()
   //   - bleRequest()
   //   - endpointToCommand()

   // Replace all fetch() calls with apiRequest():
   // OLD: const response = await fetch('/api/status');
   //      const data = await response.json();
   // NEW: const data = await apiRequest('/api/status');

   // Update these functions:
   //   - loadStatus() → use apiRequest()
   //   - loadDevices() → use apiRequest()
   //   - setRtc() → use apiRequest()
   //   - syncRtc() → use apiRequest()
   //   - saveDeviceConfig() → use apiRequest()
   //   - deleteDevice() → use apiRequest()
   //   - sendControl() → use apiRequest()
   //   - loadGlobalConfig() → use apiRequest()
   //   - saveGlobalConfig() → use apiRequest()
   //   - factoryReset() → use apiRequest()
   ```

2. **Create PWA Icons**:
   - Create `web/icon-192.png` (192x192 pixels)
   - Create `web/icon-512.png` (512x512 pixels)
   - See `web/ICONS_NEEDED.md` for instructions

3. **Configure NimBLE in sdkconfig**:
   ```bash
   idf.py menuconfig
   ```
   Navigate to:
   - `Component config → Bluetooth → Host → NimBLE - BLE only` [*]
   - `Component config → Bluetooth → Controller → Enabled` [*]
   - `Component config → Bluetooth → Bluedroid → Disabled` [ ]
   - `Component config → Wireless Coexistence → Software controls ...` [*]

### Optional (Enhancements)

4. **Error Handling Improvements**:
   - Add BLE disconnection auto-reconnect in PWA
   - Add command queue overflow handling
   - Add chunking timeout handling

5. **Documentation**:
   - Update main README with new workflow
   - Add Web Bluetooth browser compatibility notes
   - Add BLE troubleshooting section

---

## Testing Checklist

### Phase 1 Testing (ESP32)
- [ ] Build project: `idf.py build`
- [ ] Flash: `idf.py -p COMx flash monitor`
- [ ] Boot → WiFi AP + BLE both visible
- [ ] Use nRF Connect app to verify:
  - [ ] Device "ESP32C6_Gateway" discoverable
  - [ ] GATT service 0xFFF0 present
  - [ ] All 5 characteristics present
  - [ ] Can write to 0xFFF1
  - [ ] Receive notification on 0xFFF2
- [ ] Button press → WiFi stops, BLE stops, Zigbee starts
- [ ] LED shows correct states

### Phase 2 Testing (PWA)
- [ ] Connect to WiFi AP
- [ ] Load webpage: http://192.168.4.1
- [ ] Check console: Service Worker registered
- [ ] Check Application tab: Manifest valid
- [ ] Install PWA: "Add to Home Screen" appears
- [ ] After install, open PWA offline (no WiFi)
- [ ] Click "Csatlakozas Bluetooth-on"
- [ ] Browser shows device picker
- [ ] Select "ESP32C6_Gateway"
- [ ] Connection established
- [ ] Test commands:
  - [ ] Set RTC via BLE
  - [ ] Get devices via BLE
  - [ ] Control device ON/OFF via BLE
  - [ ] Configure device automation via BLE
- [ ] Disconnect BLE
- [ ] Reconnect BLE

### End-to-End Testing
- [ ] Boot → Configure via WiFi → Install PWA
- [ ] Button press → Switch to Zigbee mode
- [ ] Wait 8 seconds for Zigbee network formation
- [ ] Button press → BLE mode active
- [ ] Open installed PWA → Connect via BLE (offline)
- [ ] Set RTC time via BLE
- [ ] Add device automation via BLE
- [ ] Control device via BLE
- [ ] Button press → BLE off, Zigbee only
- [ ] Verify automations still run
- [ ] Complete workflow without ever using WiFi webpage again

---

## Build Instructions

### 1. Configure NimBLE
```bash
cd D:\Programing\esp-idf\projects\AiAgent\CLCode01
idf.py menuconfig
# Enable NimBLE, disable Bluedroid
# Save and exit
```

### 2. Create Icons
```bash
cd web
# Create icon-192.png and icon-512.png
# See ICONS_NEEDED.md for instructions
```

### 3. Integrate BLE into script.js
```bash
# Merge functions from ble-integration.js into script.js
# See "Remaining Work" section above
```

### 4. Build and Flash
```bash
idf.py build
idf.py -p COMx flash monitor
```

---

## Known Issues & Limitations

1. **Web Bluetooth Browser Support**:
   - Chrome/Edge: ✅ Full support
   - Firefox: ❌ No Web Bluetooth API
   - Safari: ❌ No Web Bluetooth API
   - Solution: Display clear error message for unsupported browsers

2. **MTU Negotiation**:
   - Default MTU: 23 bytes (20 bytes payload)
   - Large responses may require chunking
   - Current implementation handles up to 512 bytes

3. **BLE+Zigbee Coexistence**:
   - BLE has higher priority than Zigbee
   - Minimal interference expected due to BLE's periodic nature
   - Monitor RSSI/LQI during testing

4. **PWA Installation**:
   - Requires HTTPS or localhost (WiFi AP counts as localhost)
   - Icons required for install prompt to appear
   - Service Worker requires secure context

---

## Architecture Summary

```
┌─────────────────────────────────────────────────────────┐
│                        ESP32-C6                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐ │
│  │   WiFi   │  │   BLE    │  │       Zigbee         │ │
│  │   AP     │  │ NimBLE   │  │    Coordinator       │ │
│  └────┬─────┘  └────┬─────┘  └──────────┬───────────┘ │
│       │             │                    │             │
│  ┌────┴─────────────┴────────────────────┴───────────┐ │
│  │           Main State Machine                      │ │
│  │  (Setup → Zigbee → BLE Config Mode)               │ │
│  └───────────────────────────────────────────────────┘ │
│                                                         │
└─────────────────────────────────────────────────────────┘
                    │              │
        ┌───────────┘              └────────────┐
        │                                       │
        ▼                                       ▼
┌───────────────┐                    ┌─────────────────┐
│  WiFi Client  │                    │  BLE Client     │
│  (First Boot) │                    │  (PWA)          │
├───────────────┤                    ├─────────────────┤
│ • Download PWA│                    │ • Web Bluetooth │
│ • Install App │                    │ • Offline Mode  │
│ • Config RTC  │                    │ • JSON Commands │
└───────────────┘                    └─────────────────┘
```

---

## Next Steps

1. **Integrate BLE functions into script.js** (see Remaining Work section)
2. **Create PWA icons** (see web/ICONS_NEEDED.md)
3. **Configure NimBLE in menuconfig**
4. **Build and test** (see Testing Checklist)
5. **Document final workflow** in main README

---

## Questions?

- BLE GATT service implementation: See `main/ble_service.c`
- Command routing logic: See `main/ble_handlers.c`
- State machine details: See `main/main.c` (on_button_short_press)
- Web Bluetooth API usage: See `web/ble-service.js`
