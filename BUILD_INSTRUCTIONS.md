# BLE Build Instructions - Windows

## Quick Start (5 lépés)

### 1. ESP-IDF Környezet Beállítása

Nyiss meg egy **új Command Prompt**-ot (nem PowerShell, nem Cygwin!) és:

```cmd
cd D:\Programing\esp-idf\v5.5.1\esp-idf
export.bat
```

**Fontos**: Minden további parancsot ebben a terminálban futtass!

---

### 2. Projekt Könyvtárba Navigálás

```cmd
cd D:\Programing\esp-idf\projects\AiAgent\CLCode01
```

---

### 3. Setup Ellenőrzés (Opcionális)

```cmd
check_ble_setup.bat
```

Ha hibát jelez, kövesd az utasításokat.

---

### 4. Build

**Egyszerű mód** (ajánlott):
```cmd
build_ble.bat
```

**Manuális mód**:
```cmd
idf.py fullclean
idf.py build
```

---

### 5. Flash & Monitor

```cmd
idf.py -p COM3 flash monitor
```

(Cseréld a COM3-at a te portodra - nézd meg Device Manager-ben)

---

## Gyakori Hibák & Megoldások

### ❌ "idf.py: command not found"

**Ok**: ESP-IDF környezet nincs betöltve

**Megoldás**:
```cmd
cd D:\Programing\esp-idf\v5.5.1\esp-idf
export.bat
cd D:\Programing\esp-idf\projects\AiAgent\CLCode01
```

---

### ❌ "esp_nimble_hci.h: No such file"

**Ok**: NimBLE komponens nincs engedélyezve

**Megoldás 1** (Gyors):
```cmd
configure_nimble.bat
idf.py build
```

**Megoldás 2** (Manuális):
```cmd
idf.py menuconfig
```
Navigálj: `Component config → Bluetooth`
- [x] Bluetooth
- Host: (•) NimBLE - BLE only
- [x] Controller Enabled
- [ ] Bluedroid DISABLED

Mentés: `S` → `Enter` → `Q`

```cmd
idf.py build
```

---

### ❌ "SpiffsFullError: the image size has been exceeded"

**Ok**: Túl sok fájl van a web/ könyvtárban

**Megoldás**:
```cmd
cd web
del ble-integration.js generate_icons.py ICONS_NEEDED.md
cd ..
idf.py build
```

(Ezek már törölve lettek)

---

### ❌ Build sikeres, de Flash sikertelen

**Ok**: Rossz port vagy foglalt port

**Megoldás**:
1. Device Manager → Ports (COM & LPT)
2. Nézd meg melyik a helyes COM port (pl. COM5)
3. Zárd be a Serial Monitor programokat
4. Próbáld újra:
```cmd
idf.py -p COM5 flash monitor
```

---

## Ellenőrzési Lista

Build előtt futtasd le:

```cmd
check_ble_setup.bat
```

Minden sornak `[OK]`-nak kell lennie!

---

## Build Kimenet Ellenőrzése

### Sikeres Build

Így néz ki a sikeres build vége:

```
...
[1163/1163] Generating binary image from built executable
esptool.py v4.x
...
Project build complete. To flash, run:
  idf.py -p (PORT) flash
or idf.py -p (PORT) flash monitor
```

### Sikeres Flash

```
...
Hash of data verified.

Leaving...
Hard resetting via RTS pin...
```

### ESP32 Log (Monitor)

```
I (xxx) main_task: Started on CPU0
I (xxx) main_task: Calling app_main()
I (xxx) MAIN: ========================================
I (xxx) MAIN: ESP32-C6 Zigbee Gateway Started
I (xxx) MAIN: ========================================
I (xxx) MAIN: Starting in setup mode (WiFi AP + BLE)
I (xxx) BLE_TASK: BLE host synced
I (xxx) BLE_TASK: BLE advertising started: ESP32C6_Gateway
I (xxx) WIFI_TASK: Wi-Fi AP started. SSID:ESP32C6_AI_Test
```

---

## Tesztelés

### 1. nRF Connect App (Android/iOS)

1. Telepítsd az nRF Connect app-ot
2. Scan for devices
3. Látnod kell: **ESP32C6_Gateway**
4. Connect
5. Browse Services → **0xFFF0**
6. Lásd a 5 karakterisztikát: 0xFFF1-0xFFF5

### 2. PWA Telepítés

1. Csatlakozz WiFi-hez: `ESP32C6_AI_Test` / `12345678`
2. Böngésző: http://192.168.4.1
3. F12 → Console: `Service Worker registered` ✓
4. Böngésző menü → "Add to Home Screen"
5. Telepítsd

### 3. BLE Kapcsolat

1. Nyomd meg az ESP32 gombot (kilép setup módból)
2. Várj 8 másodpercet
3. Nyomd meg a gombot újra (BLE mód, LED: 2 mp villogás)
4. Nyisd meg a telepített PWA-t (offline!)
5. "Csatlakozás Bluetooth-on"
6. Válaszd: "ESP32C6_Gateway"
7. Látható: "Csatlakozva (BLE)" ✓

---

## Portok Megtalálása (Windows)

1. **Device Manager** megnyitása:
   - Win+X → Device Manager
   - VAGY: `devmgmt.msc`

2. **Ports (COM & LPT)** kibontása

3. Keress ilyesmit:
   - "USB Serial Port (COM3)"
   - "Silicon Labs CP210x (COM5)"
   - "USB-SERIAL CH340 (COM7)"

4. Ez a port számot használd: `-p COM3` vagy `-p COM5` stb.

---

## Gyors Parancsok Összefoglaló

```cmd
REM 1. ESP-IDF környezet
D:\Programing\esp-idf\v5.5.1\esp-idf\export.bat

REM 2. Projekt könyvtár
cd D:\Programing\esp-idf\projects\AiAgent\CLCode01

REM 3. Ellenőrzés
check_ble_setup.bat

REM 4. Build
build_ble.bat

REM 5. Flash (cseréld COM3-at!)
idf.py -p COM3 flash monitor

REM Kilépés monitor-ból: Ctrl+]
```

---

## Segítség

Ha elakadtál:

1. Futtasd le: `check_ble_setup.bat`
2. Nézd meg: `FINAL_STEPS.md` - Részletes troubleshooting
3. Nézd meg: `BLE_README.md` - Rendszer áttekintés

---

**Jó buildelést!** 🚀
