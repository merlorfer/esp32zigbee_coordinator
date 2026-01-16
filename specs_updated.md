# Projekt Specifikáció: ESP32-C6 Zigbee Gateway & Automatizációs Központ

## 1. Kontextus és Környezet
- **Target Mikrovezérlő:** ESP32-C6
- **Framework:** ESP-IDF v5.x + ESP-ZIGBEE-SDK
- **Szerepkör:** Zigbee Coordinator (ZC)
- **Rádiókezelés:** Exkluzív mód (Wi-Fi vagy Zigbee vezérlés prioritás)
- **Build Környezet:**
  - Export: `. D:\Programing\esp-idf\v5.5.1\esp-idf\export.ps1`
  - Target: `idf.py set-target esp32c6`
- **Verziókezelés:**
  - **Git használat kötelező:** Minden iterációs változás automatikus commit
  - **Commit üzenetek:** Leíró, értelmes üzenetek (pl. "feat: Add Zigbee permit join functionality", "fix: Correct delay timer calculation")
  - **Automatikus hibajavítás:** Build és runtime hibák automatikus detektálása és javítása
  - **Branch stratégia:** `main` branch a stabil kódhoz, feature branchek fejlesztéshez

## 2. Projekt Struktúra

```
project_root/
├── main/
│   ├── main.c                    # Fő alkalmazás belépési pont
│   ├── wifi_task.c/.h            # Wi-Fi és webszerver kezelés
│   ├── zigbee_task.c/.h          # Zigbee coordinator logika
│   ├── scheduler_task.c/.h       # Automatizációs időzítő
│   ├── button_task.c/.h          # GPIO gomb kezelés
│   ├── led_task.c/.h             # LED állapotjelzés
│   ├── nvs_manager.c/.h          # NVS műveletek
│   ├── device_manager.c/.h       # Eszköz lista kezelés
│   └── CMakeLists.txt
├── int_drivers/                  # ⚠️ Projekt gyökérben (NEM components alatt!)
│   ├── switch_driver.c           # ⚠️ MEGLÉVŐ - Gomb driver debounce-szal
│   ├── switch_driver.h           # ⚠️ MEGLÉVŐ - Gomb driver header
│   └── CMakeLists.txt            # ESP-IDF komponens definíció
├── web/
│   ├── index.html                # Webes felület
│   ├── style.css                 # Stílusok
│   └── script.js                 # JavaScript logika
├── sdkconfig                     # ESP-IDF konfiguráció
├── CMakeLists.txt                # Projekt root CMake
├── .gitignore
└── README.md
```

### Meglévő Komponensek Integrálása
- **`./int_drivers/switch_driver.c` és `.h`:** A gomb kezeléshez használt meglévő driver (projekt gyökérkönyvtárban)
  - Debounce implementáció már készen áll
  - `button_task.c`-ben `#include "switch_driver.h"` használandó
  - Az `int_drivers` könyvtár ESP-IDF komponensként van kezelve
  - **Elérési út:** `./int_drivers/` (relatív a projekt gyökérhez)

### CMakeLists.txt Konfiguráció

**Projekt root CMakeLists.txt példa:**
```cmake
cmake_minimum_required(VERSION 3.16)

# int_drivers komponens hozzáadása
set(EXTRA_COMPONENT_DIRS "./int_drivers")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c6_zigbee_gateway)
```

**int_drivers/CMakeLists.txt példa:**
```cmake
idf_component_register(
    SRCS "switch_driver.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
```

**main/CMakeLists.txt példa:**
```cmake
idf_component_register(
    SRCS "main.c" 
         "wifi_task.c"
         "zigbee_task.c" 
         "scheduler_task.c"
         "button_task.c"
         "led_task.c"
         "nvs_manager.c"
         "device_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash 
             esp_wifi 
             esp_http_server 
             esp_zigbee_lib
             int_drivers  # ← int_drivers komponens hivatkozás
)
```

## 3. Hardver Konfiguráció

### GPIO Kiosztás
- **GPIO 9:** Toggle gomb (Beállítási mód ↔ Automatizációs mód)
- **Beépített LED:** Állapotjelzés (a board beépített LED-je)

### LED Állapotjelzések
| Állapot | LED Viselkedés | Leírás |
|---------|----------------|---------|
| **Normál működés** | Másodpercenként villog | Automatizációs mód aktív, rendszer működik |
| **Wi-Fi AP aktív** | Folyamatos világítás | Beállítási mód, web interfész elérhető |
| **Újraindulás/Óra nincs beállítva** | Gyors villogás (0.5 sec) | Rendszer boot után, RTC nincs inicializálva |
| **Hiba történt** | Speciális minta (pl. 3x gyors villogás, szünet) | Zigbee kommunikációs hiba |

### RTC (Real-Time Clock)
- **Típus:** ESP32-C6 belső RTC
- **Inicializálás:** Webes felületen keresztül manuális beállítás
- **Áramszünet kezelés:** RTC resetelődik 00:00:00-ra, LED gyors villogással jelzi

## 3. Funkcionális Követelmények

### A. Wi-Fi és Konfigurációs Felület (Beállítási Mód)

#### SoftAP Paraméterek
- **SSID:** `ESP32C6_AI_Test`
- **Jelszó:** `12345678`
- **Aktiválás:** GPIO 9 gomb megnyomásával
- **IP Cím:** `192.168.4.1`

#### Webes Felület Funkciók

##### 1. Rendszeróra Beállítás
- **Időbeviteli mező:** `YYYY-MM-DD HH:MM:SS`
- **Kliens oldali frissítés:** JavaScript alapú, másodpercenként frissül
- **Szerver szinkronizáció:** "Óra beállítása" gomb küldés az ESP32-re
- **Visszajelzés:** Sikeres beállítás után a LED normál működésre vált

##### 2. Zigbee Eszközkezelés
- **Permit Join funkció:**
  - Gomb: "Új eszköz hozzáadása" (60 másodperc join időablak)
  - Visszaszámláló timer megjelenítése
  - Automatikus felület frissítés új eszköz csatlakozásakor
  
- **Eszközlista megjelenítés:**
  - Táblázat formátum minden csatlakozott eszközzel
  - Oszlopok:
    - Gyártó / Eszköz típus (két sorban egymás alatt, lekérdezett Zigbee attribútumok)
    - IEEE cím (64-bit hexadecimális)
    - Endpoint ID
    - Egyedi név (felhasználó által szerkeszthető mező)
    - Állapot / Hibák (jelenlegi állapot + hibaüzenet ha van)
    - Műveletek (Szerkesztés, Törlés gombok)

##### 3. Eszköz Automatizáció Beállítása

**Eszközönkénti konfiguráció panel:**

###### Működési Mód Választó (Dropdown)
- **Fix Időpont:** Napi ismétlődő időpontok alapján
- **Delay (Késleltetés):** Wi-Fi AP leállításától számított idő alapján

###### Fix Időpont Mód Beállítások
- **Alapértelmezett:** 1 ON/OFF időpont pár
- **Bővíthetőség:** "Új időpont hozzáadása" gomb (max 5 pár)
- **Időpont bevitel:** 
  - ON idő: `HH:MM` formátum
  - OFF idő: `HH:MM` formátum
- **Törlés:** X gomb minden pár mellett (az első kivételével)
- **Logika:**
  - Ha az időpont elmúlt aznap → másnap ugyanakkor végrehajtás
  - Minden nap automatikusan ismétlődik

Példa konfiguráció:
```
Időpont 1: ON 06:00, OFF 08:00
Időpont 2: ON 18:00, OFF 22:00
Időpont 3: ON 22:30, OFF 23:00
```

###### Delay Mód Beállítások
- **X perc:** Késleltetés a bekapcsolásig (ciklus elejétől számítva)
- **Y perc:** Bekapcsolt állapot időtartama
- **Ismétlődés:** Folyamatos ciklus (X perc → BE, Y perc → KI, X perc → BE, Y perc → KI...)

Példa:
```
AP kikapcsolás: 10:00
X = 30 perc → Eszköz BE: 10:30
Y = 120 perc → Eszköz KI: 12:30
Eszköz BE: 13:00 (újra +30 perc)
Eszköz KI: 15:00 (újra +120 perc)
Eszköz BE: 15:30
Eszköz KI: 17:30
... (folytatódik amíg Wi-Fi AP újra be nem kapcsol)
```

##### 4. Wi-Fi AP Bekapcsolási Viselkedés Beállítás
- **Globális RadioButton opció:**
  - ○ **Állapot megtartása:** Eszközök maradnak jelenlegi állapotukban Wi-Fi AP bekapcsolásakor
  - ○ **Összes kikapcsolása:** Minden eszköz OFF parancs kap Wi-Fi AP bekapcsolásakor (alapértelmezett)
- **Működés:** Amikor a GPIO 9 gombbal aktiválod a Wi-Fi AP-t (és eléred a webes felületet), ez a beállítás határozza meg az eszközök viselkedését

##### 5. Mentés és Alkalmazás
- **"Beállítások mentése" gomb:** 
  - Adatok mentése NVS-be
  - JSON formátumú válasz (sikeres/sikertelen)
  - Toast notification visszajelzés (rövid, automatikusan eltűnő üzenet a képernyő sarkában, pl. "✓ Beállítások sikeresen mentve")
- **"Automatizáció indítása" gomb:**
  - Wi-Fi AP leállítása
  - Zigbee stack aktiválása
  - Scheduler task indítása
  - LED normál működésre vált

##### 6. Hibanapló Megjelenítés
- **Hiba megjelenítés az eszközlistában:**
  - Minden eszköz sorában, az "Állapot / Hibák" oszlopban
  - Jelenlegi állapot (ON/OFF) + hibaüzenet (ha van)
  - Piros háttér/szöveg, figyelmeztető ikon ha hiba van
  - Hibaüzenet: rövid szöveg + időpont (HH:MM)
  - Példa megjelenítés: 
    ```
    ON | ⚠️ Nem válaszol (14:25)
    ```
- **Automatikus törlés:** Wi-Fi AP leállításakor (amikor a webes felület már nem elérhető)

### B. Zigbee Működés és Automatizációs Logika

#### Támogatott Eszköztípusok
- **Jelenlegi:** ON/OFF Switch (Smart Plug, relay, solenoid valve)
- **Jövőbeli:** Analog/Continuous sensors (pl. vízszint, hőmérséklet)
- **Maximum eszközszám:** 10 darab

#### Erőforrás-allokáció Vezérlés
- **Wi-Fi AP BE állapot:**
  - Zigbee stack minimális működés (az ESP Zigbee SDK nem támogatja a teljes leállítást, Zigbee radio kikapcsolás nem lehetséges)
  - Eszközök kezelése a beállított viselkedés szerint (megtartás/kikapcsolás)
  - Prioritás a webszerver számára
  - Scheduler Task felfüggesztése
  
- **Wi-Fi AP KI állapot:**
  - Teljes erőforrás a Zigbee Coordinator-nak
  - Scheduler Task aktiválása
  - Időzítők elindítása (Delay mód esetén)
  - Automatizációs logika végrehajtása

#### Parancsküldés és Visszaigazolás
- **ZCL parancsok:** `ZCL_CMD_ON_OFF_ON` és `ZCL_CMD_ON_OFF_OFF`
- **Visszaigazolás ellenőrzés:**
  - `ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID` esemény figyelése
  - Timeout: 5 másodperc
  - **Retry mechanizmus:** Sikertelen próbálkozás esetén 3 újrapróbálkozás
  - Csak ha mind a 3 újrapróbálkozás sikertelen → hiba rögzítése
  - Újrapróbálkozások közötti várakozás: 2 másodperc

#### Hibakezelés
- **Hiba típusok:**
  - Eszköz nem válaszol (timeout)
  - Negatív visszaigazolás (error status)
  - Eszköz nem elérhető a hálózatban
  
- **Hiba tárolás (NVS):**
  - Eszközönként **1 legutóbbi hiba**
  - Mezők: IEEE cím, timestamp, error message
  - LED speciális hibaminta aktiválása
  
- **Hiba törlés:**
  - Wi-Fi AP leállításakor (amikor "Automatizáció indítása" gomb megnyomásra kerül)
  - Következő hiba felülírja az előzőt ugyanazon eszköznél

## 4. Rendszer Architektúra

### Task Struktúra

#### WiFi_Task
- **Prioritás:** 5
- **Stack méret:** 4096 byte
- **Futási feltétel:** GPIO 9 gomb által aktiválva
- **Felelősségek:**
  - SoftAP indítás/leállítás
  - HTTP szerver kezelés (libmicrohttpd vagy ESP HTTP Server)
  - JSON API endpoint-ok
  - NVS read/write műveletek

#### Zigbee_Task
- **Prioritás:** 10 (magasabb mint WiFi)
- **Stack méret:** 8192 byte
- **Futási feltétel:** Mindig fut (az ESP Zigbee SDK nem támogatja a teljes leállítást)
- **Wi-Fi AP aktív alatt:** Minimális működés, csak a hálózat fenntartása
- **Wi-Fi AP inaktív alatt:** Teljes funkcionalitás
- **Felelősségek:**
  - Coordinator inicializáció
  - Device discovery és binding
  - ZCL parancsok küldése (retry mechanizmussal)
  - Callback kezelés (join, command response)
  - Error detection és logging

#### Scheduler_Task
- **Prioritás:** 8
- **Stack méret:** 4096 byte
- **Futási feltétel:** RTC inicializálva és Wi-Fi AP KI
- **Felelősségek:**
  - RTC óra tick kezelése
  - Fix időpont egyezések ellenőrzése (minutánként)
  - Delay timerek kezelése (FreeRTOS Software Timers)
  - Automatizációs parancsok triggere

#### Button_Task
- **Prioritás:** 12 (legmagasabb - interrupt jellegű)
- **Stack méret:** 2048 byte
- **Implementáció:** `switch_driver.c/.h` integráció (debounce implementáció a driver szerint)
- **Felelősségek:**
  - GPIO 9 interrupt kezelés
  - Debounce (a switch_driver.c/.h implementációja szerint)
  - Mód váltás (Wi-Fi ↔ Zigbee)
  - LED állapot váltás

#### LED_Task
- **Prioritás:** 3 (alacsony - indikáció)
- **Stack méret:** 2048 byte
- **Felelősségek:**
  - LED GPIO vezérlés
  - Villogási minták implementálása
  - Állapot üzenet queue-ból olvasás

### Szinkronizáció és Kommunikáció
- **Event Groups:** Mód váltás jelzésére (WiFi_Mode_Bit, Zigbee_Mode_Bit)
- **Queues:**
  - `cmd_queue`: Scheduler → Zigbee parancsok (ON/OFF)
  - `led_queue`: Állapot üzenetek → LED Task
  - `error_queue`: Zigbee hiba → WiFi/NVS
- **Mutexek:** NVS access, device list modification

## 5. Adatstruktúra és Tárolás

### NVS Namespace-ek
- **`config`:** Globális beállítások
- **`devices`:** Eszköz lista
- **`errors`:** Hibanaplók
- **`rtc`:** Óra állapot (opcionális backup)

### Device Configuration Structure

```c
#define MAX_DEVICE_NAME_LEN 32
#define MAX_MANUFACTURER_LEN 32
#define MAX_MODEL_LEN 32
#define MAX_TIME_PAIRS 5

typedef struct {
    uint8_t hour;
    uint8_t minute;
} time_point_t;

typedef struct {
    time_point_t on_time;
    time_point_t off_time;
} time_pair_t;

typedef enum {
    MODE_FIXED_TIME = 0,
    MODE_DELAY = 1
} automation_mode_t;

typedef struct {
    // Zigbee identifikáció
    uint64_t ieee_addr;               // 64-bit IEEE address
    uint8_t endpoint;                 // Endpoint ID (általában 1)
    
    // Eszköz információk (Zigbee attribútumokból)
    char manufacturer[MAX_MANUFACTURER_LEN];  // ZCL_MANUFACTURER_NAME
    char model[MAX_MODEL_LEN];                // ZCL_MODEL_IDENTIFIER
    
    // Felhasználói beállítások
    char custom_name[MAX_DEVICE_NAME_LEN];    // Webes felületen szerkeszthető
    bool enabled;                              // Automatizáció engedélyezve
    
    // Üzemmód
    automation_mode_t mode;
    
    // Fix időpont mód
    uint8_t time_pair_count;          // 1-5 közötti érték
    time_pair_t time_pairs[MAX_TIME_PAIRS];
    
    // Delay mód (ciklikus működés)
    uint16_t delay_on_minutes;        // X perc késleltetés a BE kapcsolásig
    uint16_t delay_duration_minutes;  // Y perc bekapcsolt állapot időtartama
    // Ciklus: X perc várakozás → BE → Y perc működés → KI → X perc várakozás → ...
    uint32_t delay_cycle_start;       // Ciklus kezdete (unix timestamp, AP leállításkor indul)
    
    // Állapotkövetés
    bool current_state;               // true=ON, false=OFF
    uint32_t last_command_timestamp;  // utolsó parancs unix timestamp
    
} device_config_t;
```

### Error Log Structure

```c
#define MAX_ERROR_MSG_LEN 64

typedef struct {
    uint64_t ieee_addr;               // Melyik eszköz
    uint32_t timestamp;               // Unix timestamp
    char error_message[MAX_ERROR_MSG_LEN];
    bool active;                      // true ha megjelenítendő
} device_error_t;
```

### Global Configuration Structure

```c
typedef struct {
    bool wifi_on_behavior;            // true=megtartás, false=kikapcsolás (Wi-Fi AP bekapcsolásakor)
    uint8_t device_count;             // Jelenleg csatlakozott eszközök száma
    bool rtc_initialized;             // RTC be van-e állítva
    uint32_t last_rtc_set;            // Utolsó RTC beállítás timestamp
} global_config_t;
```

### NVS Kulcsok

| Namespace | Kulcs | Típus | Leírás |
|-----------|-------|-------|--------|
| `config` | `global` | blob | `global_config_t` |
| `config` | `wifi_on_behavior` | u8 | Wi-Fi AP bekapcsoláskor eszköz viselkedés |
| `devices` | `count` | u8 | Eszközök száma |
| `devices` | `dev_0` ... `dev_9` | blob | `device_config_t` tömbök |
| `errors` | `err_0` ... `err_9` | blob | `device_error_t` tömbök |

## 6. API Specifikáció

### HTTP Endpoints

#### GET `/api/status`
**Válasz:**
```json
{
  "rtc_initialized": true,
  "current_time": "2025-01-16 14:30:45",
  "device_count": 3,
  "wifi_active": true,
  "zigbee_active": false
}
```

#### POST `/api/rtc/set`
**Kérés:**
```json
{
  "datetime": "2025-01-16 14:30:45"
}
```
**Válasz:**
```json
{
  "success": true,
  "message": "RTC beállítva"
}
```

#### POST `/api/zigbee/permit-join`
**Kérés:**
```json
{
  "duration": 60
}
```
**Válasz:**
```json
{
  "success": true,
  "expires_at": "2025-01-16 14:31:45"
}
```

#### GET `/api/devices`
**Válasz:**
```json
{
  "devices": [
    {
      "ieee_addr": "0x00124B001F2A3B4C",
      "endpoint": 1,
      "manufacturer": "IKEA",
      "model": "TRADFRI control outlet",
      "custom_name": "Nappali lámpa",
      "enabled": true,
      "mode": "fixed_time",
      "time_pairs": [
        {"on": "06:00", "off": "08:00"},
        {"on": "18:00", "off": "22:00"}
      ],
      "current_state": false,
      "error": null
    },
    {
      "ieee_addr": "0x00124B001F2A3B5D",
      "endpoint": 1,
      "manufacturer": "Sonoff",
      "model": "ZBMINI",
      "custom_name": "Konyha konnektor",
      "enabled": true,
      "mode": "delay",
      "delay_on_minutes": 30,
      "delay_duration_minutes": 120,
      "current_state": true,
      "error": {
        "message": "Nem válaszol",
        "timestamp": "14:25"
      }
    }
  ]
}
```

#### POST `/api/devices/{ieee_addr}/config`
**Kérés:**
```json
{
  "custom_name": "Konyha konnektor",
  "enabled": true,
  "mode": "delay",
  "delay_on_minutes": 30,
  "delay_duration_minutes": 120
}
```

#### DELETE `/api/devices/{ieee_addr}`
**Válasz:**
```json
{
  "success": true,
  "message": "Eszköz törölve"
}
```

#### POST `/api/wifi/shutdown`
**Kérés:**
```json
{
  "wifi_on_behavior": "power_off"  // vagy "maintain_state"
  // Ez a beállítás azt szabályozza, hogy Wi-Fi AP BEKAPCSOLÁSAKOR mi történjen az eszközökkel
}
```
**Válasz:**
```json
{
  "success": true,
  "message": "Wi-Fi leállítása megkezdve, Zigbee automatizáció aktív",
  "errors_cleared": true
}
```

## 7. Build és Automatizált Hibajavítás

### Build Folyamat
```bash
# 1. Környezet beállítás
. D:\Programing\esp-idf\v5.5.1\esp-idf\export.ps1

# 2. Target beállítás (csak első alkalommal)
idf.py set-target esp32c6

# 3. Konfiguráció (menuconfig szükség szerint)
idf.py menuconfig

# 4. Build
idf.py build

# 5. Flash
idf.py -p COMx flash monitor
```

### Automatikus Hibajavítás Követelmények

Claude Code-nak az alábbi módon kell kezelnie a hibákat:

#### Build Time Hibák
1. **Syntax Error:**
   - Automatikus detektálás a build kimenetből
   - Hibaüzenet elemzése (fájl, sor, oszlop)
   - Javítás és újrafordítás
   - Git commit: `fix: Correct syntax error in [filename]`

2. **Linker Error:**
   - Hiányzó függvény definíciók
   - Duplikált szimbólumok
   - CMakeLists.txt korrekciók
   - Git commit: `fix: Resolve linker error - [issue]`

3. **Header/Include Hibák:**
   - Hiányzó include-ok hozzáadása
   - Circular dependency feloldása
   - Git commit: `fix: Add missing headers`

#### Runtime Hibák
1. **ESP_ERROR_CHECK Failures:**
   - NVS inicializálási hibák
   - Wi-Fi/Zigbee stack hibák
   - Automatikus error handling hozzáadása
   - Git commit: `fix: Add error handling for [component]`

2. **Memory Issues:**
   - Stack overflow detektálás
   - Heap fragmentáció
   - Task stack méret növelés
   - Git commit: `fix: Increase stack size for [task_name]`

3. **Logic Errors:**
   - Scheduler timing problémák
   - Race condition-ök
   - Null pointer dereference
   - Git commit: `fix: Resolve [specific issue] in [module]`

### Git Workflow Követelmények

#### Commit Konvenciók
Claude Code minden változtatást az alábbi formátumban commit-oljon:

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Type értékek:**
- `feat`: Új funkció
- `fix`: Hibajavítás
- `refactor`: Kód átstrukturálás
- `docs`: Dokumentáció
- `style`: Formázás
- `test`: Tesztek
- `chore`: Build/config változások

**Példák:**
```bash
feat(wifi): Implement SoftAP configuration endpoint
fix(zigbee): Correct command retry mechanism timeout
refactor(scheduler): Optimize delay timer calculations
docs(readme): Add installation instructions
```

#### Branch Stratégia
- **`main`**: Stabil, működő kód
- **`develop`**: Fejlesztési ág
- **`feature/[name]`**: Új funkciók (pl. `feature/web-interface`)
- **`fix/[issue]`**: Hibajavítások (pl. `fix/nvs-corruption`)

#### Automatikus Git Műveletek
1. **Minden sikeres build után:**
   ```bash
   git add -A
   git commit -m "[type]: [message]"
   ```

2. **Feature befejezése után:**
   ```bash
   git checkout develop
   git merge feature/[name]
   git branch -d feature/[name]
   ```

3. **Hibajavítás után:**
   - Immediate commit
   - Rebuild verification
   - Auto-merge ha build sikeres

### Hibakeresési Stratégia

1. **Compile Error → Javítás → Commit**
2. **Runtime Error → Log elemzés → Javítás → Commit**
3. **Logic Error → Debug, reprodukció → Javítás → Commit**
4. **Minden iteráció után:** `idf.py build` futtatása
5. **Ha build sikeres:** Git commit
6. **Ha build sikertelen:** Újrapróbálkozás max 3x

### Logging és Debug

```c
// Használandó ESP-IDF log szintek
ESP_LOGE(TAG, "Critical error: %s", error_msg);  // Error
ESP_LOGW(TAG, "Warning: %s", warning_msg);       // Warning
ESP_LOGI(TAG, "Info: %s", info_msg);             // Info
ESP_LOGD(TAG, "Debug: %s", debug_msg);           // Debug
ESP_LOGV(TAG, "Verbose: %s", verbose_msg);       // Verbose
```

**Log TAG-ek modulonként:**
- `WIFI_TASK`
- `ZIGBEE_TASK`
- `SCHEDULER_TASK`
- `BUTTON_TASK`
- `LED_TASK`
- `NVS_MANAGER`
- `DEVICE_MANAGER`

### .gitignore Sablon

```gitignore
# ESP-IDF Build
build/
sdkconfig.old
*.bin
*.elf
*.map

# IDE
.vscode/
.idea/
*.swp
*.swo
*~

# OS
.DS_Store
Thumbs.db

# Python
__pycache__/
*.pyc

# Logs
*.log

# Backup
*.bak
*.backup
```

## 8. Fejlesztési Fázisok

### Fázis 0: Projekt Inicializálás és Git Setup (0.5 nap)
- [ ] Git repository inicializálás (`git init`)
- [ ] `.gitignore` létrehozása (build/, sdkconfig.old, .vscode/, stb.)
- [ ] Alap projekt struktúra generálás ESP-IDF példából
- [ ] **Projekt root CMakeLists.txt:** `EXTRA_COMPONENT_DIRS` beállítás `./int_drivers`-re
- [ ] **int_drivers/CMakeLists.txt:** Komponens definíció létrehozása (ha még nincs)
- [ ] **main/CMakeLists.txt:** `int_drivers` hozzáadása a REQUIRES-hez
- [ ] `./int_drivers/switch_driver.c/.h` létezésének ellenőrzése
- [ ] Teszt build: `idf.py build` - int_drivers komponens felismerése
- [ ] Kezdeti commit: `chore: Initialize ESP32-C6 Zigbee Gateway project`
- [ ] **Git commits minden lépésnél**

### Fázis 1: Alapinfrastruktúra (1-2 nap)
- [ ] Projekt generálás ESP-IDF példából
- [ ] GPIO és LED driver implementáció
- [ ] Button Task implementáció (`switch_driver.h` include-tal)
- [ ] Event Group és Queue struktúrák
- [ ] NVS inicializálás és alapfunkciók
- [ ] **Git commit minden működő modul után**

### Fázis 2: Wi-Fi és Webszerver (2-3 nap)
- [ ] SoftAP konfiguráció
- [ ] HTTP szerver beállítás
- [ ] JSON API endpoint implementáció
- [ ] RTC beállítás és szinkronizáció
- [ ] Alapvető HTML/CSS/JS frontend
- [ ] **Git commit minden API endpoint után**

### Fázis 3: Zigbee Coordinator (3-4 nap)
- [ ] Zigbee stack inicializálás
- [ ] Permit Join funkció
- [ ] Device discovery és attribute query
- [ ] ZCL ON/OFF parancs implementáció
- [ ] Command response callback kezelés
- [ ] **Git commit minden Zigbee funkció után**

### Fázis 4: Scheduler és Automatizáció (2-3 nap)
- [ ] RTC tick kezelés
- [ ] Fix időpont ellenőrzés
- [ ] Delay timer implementáció (ciklikus működés)
- [ ] Parancs queue és végrehajtás
- [ ] State machine minden eszközhöz
- [ ] **Git commit minden scheduler funkció után**

### Fázis 5: Hibakezelés és UI finomítás (2-3 nap)
- [ ] Error detection és logging (3x retry mechanizmus)
- [ ] Retry mechanizmus implementálás
- [ ] Hibanapló UI komponens (inline az eszközlistában)
- [ ] LED hiba animáció
- [ ] Teljes weboldal responsive design
- [ ] **Git commit minden hibajavítás után**

### Fázis 6: Tesztelés és Optimalizáció (3-5 nap)
- [ ] Unit tesztek (Scheduler, NVS)
- [ ] Integrációs tesztek (Wi-Fi ↔ Zigbee váltás)
- [ ] Memória profiling
- [ ] Energiafogyasztás optimalizálás
- [ ] Dokumentáció írás
- [ ] **Final commit és tag: `v1.0.0`**

## 8. Kritikus Megfontolások

### Biztonság
- [ ] Wi-Fi jelszó erősségének ellenőrzése
- [ ] HTTPS támogatás megfontolása (ESP-TLS)
- [ ] NVS titkosítás (flash encryption)
- [ ] Zigbee hálózat zárása (install code használat)

### Megbízhatóság
- [ ] Watchdog timer minden task-hoz
- [ ] NVS corruption detection
- [ ] Graceful degradation (partial function ha egy eszköz hibás)
- [ ] Logging és diagnosztika (ESP Log UART)

### Skálázhatóság
- [ ] Dinamikus eszköz limit kezelés (ha 10-nél több is kell)
- [ ] Jövőbeli eszköztípusok plugin architektúra
- [ ] OTA firmware update előkészítés

## 9. Tesztelési Checklist

### Funkcionális Tesztek
- [ ] GPIO 9 gomb minden állapotváltás
- [ ] LED minták minden szcenárióban
- [ ] RTC beállítás és időzítési pontosság
- [ ] Zigbee join és 10 eszköz kezelése
- [ ] Fix időpont minden időzóna (00:00, 12:00, 23:59)
- [ ] Delay számítás különböző időtartamokkal
- [ ] Wi-Fi shutdown viselkedés mindkét opcióval
- [ ] Hibajelzés és napló működés

### Határesetek
- [ ] Áramszünet közben
- [ ] RTC beállítás nélküli működés
- [ ] Eszköz leválasztás működés közben
- [ ] Egyidejű több eszköz parancs (race condition)
- [ ] NVS tele írás (memória limit)
- [ ] Nagyon hosszú delay értékek (overflow ellenőrzés)

### Teljesítmény
- [ ] Weboldal betöltési idő (<2 sec)
- [ ] API válaszidő (<500 ms)
- [ ] Zigbee parancs késleltetés (<1 sec)
- [ ] Memóriahasználat monitoring (heap, stack)
- [ ] CPU load méri task-onként

## 10. Dokumentáció Követelmények

### Felhasználói Dokumentáció
- Gyors telepítési útmutató
- Webes felület használati leírás képekkel
- Hibaelhárítási FAQ
- Zigbee eszköz kompatibilitási lista

### Fejlesztői Dokumentáció
- Architektúra diagram (task-ok, adatfolyam)
- API referencia (minden endpoint példákkal)
- NVS adatstruktúra leírás
- Build és debug útmutató
- Kód kommentek (Doxygen formátum)

---

**Utolsó frissítés:** 2025-01-16  
**Verzió:** 2.1  
**Státusz:** Részletesen specifikált, Git integrációval, kész implementációra

---

## CLAUDE CODE INSTRUKCIÓK

### Általános Munkafolyamat

1. **Projekt indítás:**
   ```bash
   git init
   # Kezdeti struktúra létrehozása
   git add -A
   git commit -m "chore: Initialize ESP32-C6 Zigbee Gateway project"
   ```

2. **Minden egyes funkció implementálása:**
   - Írj kódot
   - Build: `idf.py build`
   - Ha sikeres: `git add -A && git commit -m "[type]: [message]"`
   - Ha sikertelen: Javítsd a hibát és ismételd

3. **Hiba észlelés esetén:**
   - Elemezd a build/runtime hibát
   - Javítsd automatikusan
   - Build újra
   - Commit: `fix: [konkrét probléma leírása]`
   - Maximum 3 újrapróbálkozás, ha nem sikerül → kérj segítséget

4. **Meglévő fájlok használata:**
   - `./int_drivers/switch_driver.c` és `.h` már létezik a projekt gyökérkönyvtárban
   - NE generáld újra, csak include-old: `#include "switch_driver.h"`
   - A debounce logika már implementálva van
   - **Fontos:** A root CMakeLists.txt-ben add hozzá: `set(EXTRA_COMPONENT_DIRS "./int_drivers")`
   - A main/CMakeLists.txt REQUIRES részéhez add hozzá: `int_drivers`

5. **Commit message formátum szigorúan:**
   ```
   feat(wifi): Add SoftAP configuration
   fix(zigbee): Correct retry timeout calculation
   refactor(scheduler): Optimize timer loop
   docs(readme): Add build instructions
   ```

6. **Build ellenőrzés minden commit előtt:**
   - MINDIG futtass `idf.py build`-et
   - Csak sikeres build után commit
   - Syntax/linker hibák → azonnali javítás

7. **Fejlesztési sorrend:**
   - Fázis 0 → Fázis 1 → ... → Fázis 6
   - Egy fázison belül is commitolj minden működő alrendszer után
   - Ne ugorj előre, hadd építsd fel fokozatosan

### Automatikus Hibajavítás Prioritás

1. **Legfontosabb:** Syntax és compile hibák
2. **Közepesen fontos:** Runtime ESP_ERROR_CHECK failures
3. **Kevésbé sürgős:** Memory optimization, code style

### Kommunikáció

- Ha valamihez több információ kell → kérdezz
- Ha valamit nem lehet automatikusan javítani → jelezd
- Ha valami eltér a specs-től → kérdezz mielőtt változtatnál

**KEZDÉS ELŐTT:** Olvasd el a teljes specifikációt, különösen:
- Projekt struktúra (int_drivers a projekt gyökérben: `./int_drivers/`)
- CMakeLists.txt konfigurációk (EXTRA_COMPONENT_DIRS beállítás!)
- Git workflow követelmények
- API endpoint-ok
- Adatstruktúrák (device_config_t, stb.)

**KEZDÉS UTÁN:** Kövess szigorú Git hygiene-t és minden változás után build!
