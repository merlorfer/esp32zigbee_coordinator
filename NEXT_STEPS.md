# Next Steps to Complete BLE Implementation

## Quick Start (5 Steps)

### Step 1: Update script.js (CRITICAL)
Open `web/script.js` and make these changes:

**1.1 Add BLE globals at the top (after line 13):**
```javascript
let bleGateway = null;
let bleConnected = false;
let useBluetoothMode = false;
```

**1.2 Add BLE functions (copy from `web/ble-integration.js`):**
- Copy the entire `connectBLE()` function
- Copy the entire `disconnectBLE()` function
- Copy the entire `apiRequest()` function
- Copy the entire `httpRequest()` function
- Copy the entire `bleRequest()` function
- Copy the entire `endpointToCommand()` function
- Copy the entire `extractIeeeFromEndpoint()` function

**1.3 Replace ALL fetch() calls with apiRequest():**

Find and replace:
```javascript
// BEFORE:
const response = await fetch('/api/status');
const data = await response.json();

// AFTER:
const data = await apiRequest('/api/status');
```

```javascript
// BEFORE:
await fetch('/api/rtc/set', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ datetime: datetimeString })
});

// AFTER:
await apiRequest('/api/rtc/set', 'POST', { datetime: datetimeString });
```

Functions to update:
- `loadStatus()` - Line ~62
- `setRtc()` - Line ~99
- `syncRtc()` - Line ~116
- `loadDevices()` - Line ~138
- `sendControl()` - Line ~280
- `saveDeviceConfig()` - Line ~450
- `deleteDevice()` - Line ~510
- `loadGlobalConfig()` - Line ~560
- `saveGlobalConfig()` - Line ~580
- `factoryReset()` - Line ~600

**1.4 Add BLE disconnect handler at the end:**
```javascript
window.addEventListener('ble-disconnected', () => {
    showMessage('Bluetooth kapcsolat megszakadt', 'warning');
    disconnectBLE();
});
```

---

### Step 2: Create PWA Icons

**Option A: Quick placeholder icons (for testing):**
```bash
# Install ImageMagick if not installed
# Windows: choco install imagemagick
# Or download from: https://imagemagick.org/

cd D:\Programing\esp-idf\projects\AiAgent\CLCode01\web

# Create simple blue square icons
magick -size 192x192 xc:#3498db icon-192.png
magick -size 512x512 xc:#3498db icon-512.png
```

**Option B: Proper icons:**
1. Go to https://realfavicongenerator.net/
2. Upload a logo or create one
3. Generate 192x192 and 512x512 PNG files
4. Save as `icon-192.png` and `icon-512.png` in the `web/` folder

---

### Step 3: Configure NimBLE in menuconfig

```bash
cd D:\Programing\esp-idf\projects\AiAgent\CLCode01
idf.py menuconfig
```

Navigate and enable:
1. `Component config → Bluetooth → [*] Bluetooth`
2. `Component config → Bluetooth → Host`
   - Select: `(*) NimBLE - BLE only`
3. `Component config → Bluetooth → Controller`
   - `[*] Controller Only`
4. `Component config → Bluetooth → Bluedroid`
   - Make sure this is `[ ]` DISABLED
5. `Component config → Wireless Coexistence`
   - `[*] Software controls WiFi/Bluetooth coexistence`

Save and exit (S, then Enter, then Q)

---

### Step 4: Build and Flash

```bash
idf.py build
idf.py -p COM3 flash monitor
```

(Replace COM3 with your actual port)

---

### Step 5: Test BLE Connection

**5.1 Test with nRF Connect app (Android/iOS):**
1. Download "nRF Connect" from app store
2. Scan for devices
3. Connect to "ESP32C6_Gateway"
4. Browse services → Find 0xFFF0 service
5. Click on characteristic 0xFFF1 (Command Request)
6. Write value (enable notifications on 0xFFF2 first):
   ```json
   {"cmd":"get_status","params":{}}
   ```
7. Check notification on 0xFFF2 for response

**5.2 Test with Web Bluetooth (PWA):**
1. Boot ESP32 → WiFi AP starts
2. Connect to WiFi: SSID = "ESP32C6_AI_Test", Password = "12345678"
3. Open browser: http://192.168.4.1
4. Install PWA: Click "Add to Home Screen" or browser menu
5. Press button on ESP32 (exits setup mode → Zigbee starts)
6. Wait 8 seconds for Zigbee network formation
7. Press button again (BLE mode activates, LED slow blink)
8. Open installed PWA (works offline now)
9. Click "Csatlakozas Bluetooth-on"
10. Browser shows device picker → Select "ESP32C6_Gateway"
11. Test commands:
    - Set RTC time
    - View devices
    - Control device ON/OFF

---

## Troubleshooting

### Build Errors

**Error: "bt component not found"**
- Solution: Run `idf.py menuconfig` and enable Bluetooth

**Error: "nimble/nimble_port.h: No such file"**
- Solution: Enable NimBLE in menuconfig, disable Bluedroid

**Error: "multiple definition of ble_hs_cfg"**
- Solution: Only one file should define ble_hs_cfg (it's in ble_task.c)

### Runtime Errors

**BLE not advertising:**
- Check logs: `ble_on_sync()` should be called
- Verify `ble_task_start()` was called
- Check `s_ble_active` flag is true

**Web Bluetooth can't find device:**
- Ensure Chrome/Edge browser (Firefox/Safari don't support Web Bluetooth)
- BLE must be active (press button to enable BLE mode)
- Check ESP32 logs for "BLE advertising started"
- Try from another device

**Connection fails immediately:**
- MTU negotiation issue
- Check BLE stack is initialized: `ble_hs_synced()` returns true
- Verify GATT service registered successfully

**Commands don't respond:**
- Check characteristic handles are correct
- Verify `s_conn_handle` is set when connected
- Check JSON parsing in `ble_handlers_process_command()`
- Monitor ESP32 logs for errors

### Testing Checklist

- [ ] Build completes without errors
- [ ] ESP32 boots and shows: "Starting in setup mode (WiFi AP + BLE)"
- [ ] nRF Connect can discover "ESP32C6_Gateway"
- [ ] nRF Connect can connect and see service 0xFFF0
- [ ] Can write to characteristic 0xFFF1 and receive notification on 0xFFF2
- [ ] Button press exits setup mode (WiFi stops, BLE stops, Zigbee starts)
- [ ] Button press toggles BLE mode (LED slow blink when BLE active)
- [ ] PWA installs from WiFi webpage
- [ ] PWA works offline after installation
- [ ] PWA can connect via Web Bluetooth
- [ ] Can set RTC time via BLE
- [ ] Can view devices via BLE
- [ ] Can control devices via BLE
- [ ] Can configure devices via BLE

---

## File Locations Reference

```
D:\Programing\esp-idf\projects\AiAgent\CLCode01\
├── main/
│   ├── ble_task.c          ✅ Created - BLE lifecycle
│   ├── ble_task.h          ✅ Created
│   ├── ble_service.c       ✅ Created - GATT server
│   ├── ble_service.h       ✅ Created
│   ├── ble_handlers.c      ✅ Created - Command processing
│   ├── ble_handlers.h      ✅ Created
│   ├── common.h            ✅ Modified - BLE event bits
│   ├── main.c              ✅ Modified - State machine
│   ├── led_task.c          ✅ Modified - BLE LED state
│   └── CMakeLists.txt      ✅ Modified - BLE components
├── web/
│   ├── ble-service.js      ✅ Created - Web Bluetooth API
│   ├── service-worker.js   ✅ Created - PWA caching
│   ├── manifest.json       ✅ Created - PWA metadata
│   ├── ble-integration.js  ✅ Created - Integration helpers
│   ├── index.html          ✅ Modified - BLE UI
│   ├── style.css           ✅ Modified - BLE styles
│   ├── script.js           ⚠️  NEEDS UPDATE - See Step 1
│   ├── icon-192.png        ⚠️  NEEDS CREATION - See Step 2
│   └── icon-512.png        ⚠️  NEEDS CREATION - See Step 2
└── sdkconfig               ⚠️  NEEDS CONFIGURATION - See Step 3
```

---

## Expected Behavior After Completion

### Boot Sequence:
```
1. ESP32 powers on
2. WiFi AP starts (SSID: ESP32C6_AI_Test)
3. BLE starts advertising (Name: ESP32C6_Gateway)
4. LED: Solid ON (setup mode indicator)
5. User can:
   - Connect to WiFi → Download PWA
   - Connect via BLE → Configure immediately
```

### Button Behavior:
```
First Press:  WiFi OFF → BLE OFF → Zigbee ON (LED: 1 sec blink)
Second Press: BLE ON (LED: 2 sec slow blink)
Third Press:  BLE OFF (LED: 1 sec blink)
Long Press:   Zigbee Pairing Mode (LED: 0.25 sec fast blink)
```

### PWA Behavior:
```
1. Install PWA from WiFi webpage
2. Exit setup mode (button press)
3. Open PWA offline
4. Press button to enable BLE
5. Click "Csatlakozas Bluetooth-on" in PWA
6. Browser shows device picker
7. Select "ESP32C6_Gateway"
8. Connected! Can now control devices via BLE
```

---

## Support

For detailed implementation status, see: `BLE_IMPLEMENTATION_STATUS.md`
For icon creation help, see: `web/ICONS_NEEDED.md`
For BLE integration code, see: `web/ble-integration.js`
